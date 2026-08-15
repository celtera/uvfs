#include "format.hpp"
#include "hash.hpp"
#include "output_region.hpp"
#include "platform.hpp"
#include "scope_guard.hpp"
#include "zstd_codec.hpp"

#include <uvfs/path.hpp>
#include <uvfs/writer.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <system_error>
#include <unordered_map>

#if defined(UVFS_HAS_ZSTD)
#include <zdict.h>
#endif

namespace uvfs
{
namespace
{
struct pending
{
  std::string path_in_archive;
  std::string path_in_system;
};

//! An input that survived sizing and has a place in the archive.
struct placed
{
  const pending* src{};
  int64_t size{}; //!< the file's size on disk
  int64_t name_offset{};
  uint64_t device{};       //!< st_dev / st_ino identify the file itself, so
  uint64_t inode{};        //!< two names for one file can share a payload
  int64_t shares_with{-1}; //!< index of the entry holding the bytes, or -1

  // Only known once the payload is placed: with compression the stored size
  // depends on the codec, so the index is written last.
  int64_t data_offset{};
  int64_t stored_size{};
  codec method{codec::store};
  uint64_t content_hash{};
};

void unlink_quietly(const std::string& p) noexcept
{
  platform::remove_quietly(p.c_str());
}

//! Copying is bound by per-file syscalls, not CPU, and stops improving past
//! about a dozen threads. Compression is CPU-bound and scales further, so the
//! two phases get different defaults.
constexpr int copy_thread_cap = 12;

[[nodiscard]] auto worker_count(int64_t items, int requested, int cap = 0) -> int
{
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  // Without pthreads a thread cannot be created at all.
  (void)requested;
  (void)cap;
  (void)items;
  return 1;
#else
  int hw = requested;
  if (hw <= 0)
  {
    hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw <= 0)
      hw = 4;
    if (cap > 0)
      hw = std::min(hw, cap);
  }
  return static_cast<int>(
      std::min<int64_t>(static_cast<int64_t>(hw), std::max<int64_t>(items, 1)));
#endif
}

//! Runs `body(i)` for i in [0, n) across a pool sized to the work.
template <typename F>
void parallel_for(int64_t n, int requested_threads, F&& body, int cap = 0)
{
  if (n <= 0)
    return;
  const int threads = worker_count(n, requested_threads, cap);

  // One worker means the caller's thread and no thread objects at all.
  if (threads <= 1)
  {
    for (int64_t i = 0; i < n; i++)
      body(i);
    return;
  }

  std::atomic<int64_t> next{0};

  // std::thread, not jthread: jthread needs libc++ 18, newer than some targets
  // ship, and only the join was wanted. The guard covers every exit, including
  // an exception part way through starting the pool.
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  const scope_guard join_all{[&]
                             {
                               for (auto& t : pool)
                                 if (t.joinable())
                                   t.join();
                             }};

  const auto worker = [&]
  {
    for (;;)
    {
      const int64_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n)
        return;
      body(i);
    }
  };

  for (int t = 0; t < threads; t++)
  {
    try
    {
      pool.emplace_back(worker);
    }
    catch (const std::system_error&)
    {
      break; // no more threads; the caller's thread takes the rest
    }
  }
  worker();
}

#if defined(UVFS_HAS_ZSTD)
//! Returns false and fills `error` rather than throwing: this runs on worker
//! threads.
[[nodiscard]] auto read_file(
    const std::string& path,
    int64_t expected,
    std::vector<char>& out,
    std::string& error) -> bool
{
  try
  {
    auto fd = platform::file::open_read(path.c_str());
    const auto actual = fd.size();
    if (actual != expected)
    {
      error = "file changed size while the archive was being written ("
              + std::to_string(expected) + " -> " + std::to_string(actual) + ")";
      return false;
    }
    out.resize(static_cast<std::size_t>(actual));
    if (fd.read_at(out.data(), actual, 0) != actual)
    {
      error = "file ended early";
      return false;
    }
    return true;
  }
  catch (const std::exception& e)
  {
    error = e.what();
    return false;
  }
}
#endif // UVFS_HAS_ZSTD

//! How many bytes of input a single compression batch holds in memory at once.
constexpr int64_t compression_batch_bytes = int64_t{64} * 1024 * 1024;
//! Above this, compressibility is decided from a sample rather than by
//! compressing a multi-gigabyte file to find out it was already compressed.
constexpr int64_t sample_decision_threshold = int64_t{1} << 20;
constexpr int64_t sample_bytes = int64_t{256} * 1024;
//! Chunk size when streaming a payload too large to hold in memory.
constexpr int64_t stream_chunk = int64_t{4} * 1024 * 1024;

#if defined(UVFS_HAS_ZSTD)
[[nodiscard]] auto worth_keeping(
    int64_t compressed,
    int64_t original,
    const compression_settings& cs) noexcept -> bool
{
  if (compressed <= 0 || compressed >= original)
    return false;
  if (cs.method == compression::always)
    return true;
  // A couple of percent is not worth losing the zero-copy pointer.
  const int64_t saved = original - compressed;
  return saved * 100 >= original * cs.min_gain_percent;
}

using cdict_ptr = std::unique_ptr<ZSTD_CDict, size_t (*)(ZSTD_CDict*)>;

//! Decides from the first `sample_bytes`.
[[nodiscard]] auto
sample_says_compress(const std::string& path, const compression_settings& cs) -> bool
{
  try
  {
    auto fd = platform::file::open_read(path.c_str());
    std::vector<char> sample(static_cast<std::size_t>(sample_bytes));
    const auto got = fd.read_at(sample.data(), sample_bytes, 0);
    if (got <= 0)
      return false;
    std::vector<char> dst(static_cast<std::size_t>(zstd_bound(got)));
    const zstd_compressor c;
    const auto packed
        = c.compress(dst.data(), std::ssize(dst), sample.data(), got, cs.level, nullptr);
    return worth_keeping(packed, got, cs);
  }
  catch (const std::exception&)
  {
    return false;
  }
}

#endif // UVFS_HAS_ZSTD

#if defined(UVFS_HAS_ZSTD)

//! A shared dictionary is what makes compression work on many small files,
//! which are individually too short for zstd to build any history.
[[nodiscard]] auto train_dictionary_from(
    const std::vector<placed>& plan,
    const compression_settings& cs) -> std::vector<char>
{
  // Small files only: they are what a dictionary helps.
  std::vector<const placed*> candidates;
  for (const auto& p : plan)
    if (p.size > 0 && p.size <= int64_t{64} * 1024)
      candidates.push_back(&p);
  if (candidates.size() < 8)
    return {};

  const auto stride = std::max<std::size_t>(
      1,
      candidates.size() / static_cast<std::size_t>(std::max(1, cs.dictionary_samples)));

  std::vector<char> blob;
  std::vector<std::size_t> sizes;
  std::string error;
  std::vector<char> buffer;
  // The trainer wants a multiple of the dictionary size to work from.
  const auto budget = cs.dictionary_size * 100;
  for (std::size_t i = 0; i < candidates.size() && std::ssize(blob) < budget;
       i += stride)
  {
    const auto& p = *candidates[i];
    if (!read_file(p.src->path_in_system, p.size, buffer, error))
      continue;
    blob.insert(blob.end(), buffer.begin(), buffer.end());
    sizes.push_back(buffer.size());
  }
  if (sizes.size() < 8 || blob.empty())
    return {};

  std::vector<char> dict(static_cast<std::size_t>(cs.dictionary_size));
  const auto produced = ZDICT_trainFromBuffer(
      dict.data(),
      dict.size(),
      blob.data(),
      sizes.data(),
      static_cast<unsigned>(sizes.size()));
  if (ZDICT_isError(produced))
    return {}; // not enough material; carry on without one
  dict.resize(produced);
  return dict;
}

//! For payloads too large to stage. Returns the stored size, or -1 if it
//! would not be smaller than the input.
[[nodiscard]] auto stream_compress_into(
    const std::string& path,
    int64_t size,
    char* dst,
    int64_t dst_capacity,
    const compression_settings& cs,
    const ZSTD_CDict* dict,
    std::string& error) -> int64_t
{
  auto fd = platform::file::open_read(path.c_str());
  const std::unique_ptr<ZSTD_CCtx, size_t (*)(ZSTD_CCtx*)> ctx{
      ZSTD_createCCtx(), &ZSTD_freeCCtx};
  if (!ctx)
  {
    error = "could not create a zstd context";
    return -1;
  }
  if (dict)
    ZSTD_CCtx_refCDict(ctx.get(), dict);
  else
    ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, cs.level);
  ZSTD_CCtx_setPledgedSrcSize(ctx.get(), static_cast<unsigned long long>(size));

  std::vector<char> in(static_cast<std::size_t>(stream_chunk));
  ZSTD_outBuffer out{dst, static_cast<std::size_t>(dst_capacity), 0};

  int64_t consumed = 0;
  while (consumed < size)
  {
    const auto want = std::min<int64_t>(stream_chunk, size - consumed);
    const auto got = fd.read_at(in.data(), want, consumed);
    if (got <= 0)
    {
      error = "file ended early";
      return -1;
    }
    consumed += got;
    ZSTD_inBuffer input{in.data(), static_cast<std::size_t>(got), 0};
    const auto mode = (consumed >= size) ? ZSTD_e_end : ZSTD_e_continue;
    while (input.pos < input.size || mode == ZSTD_e_end)
    {
      const auto rc = ZSTD_compressStream2(ctx.get(), &out, &input, mode);
      if (ZSTD_isError(rc))
      {
        error = ZSTD_getErrorName(rc);
        return -1;
      }
      if (out.pos >= static_cast<std::size_t>(dst_capacity))
        return -1; // would not be smaller; store it verbatim instead
      if (mode == ZSTD_e_end && rc == 0)
        break;
      if (mode == ZSTD_e_continue && input.pos >= input.size)
        break;
    }
  }
  return static_cast<int64_t>(out.pos);
}

#endif // UVFS_HAS_ZSTD

} // namespace

namespace
{
//! Places every payload, compressing where it pays; returns bytes of data
//! region used. Batched so memory stays bounded by the batch, with offsets
//! assigned in sorted order so the output is reproducible despite the
//! parallelism.
template <typename OnError>
auto place_compressed(
    std::vector<placed>& plan,
    char* payloads,
    const platform::file& out,
    int64_t data_base,
    const std::vector<char>& dictionary,
    const compression_settings& cs,
    bool want_hashes,
    int threads,
    OnError&& on_error) -> int64_t
{
#if !defined(UVFS_HAS_ZSTD)
  (void)plan;
  (void)payloads;
  (void)out;
  (void)data_base;
  (void)dictionary;
  (void)cs;
  (void)want_hashes;
  (void)threads;
  (void)on_error;
  throw no_zstd_error("compression");
#else
  // Reserved ahead of the cursor in chunks: the final size is unknown until
  // everything is compressed, and writing into a hole the filesystem cannot
  // fill is a SIGBUS rather than an error.
  int64_t reserved = 0;
  bool can_reserve = true;
  const auto reserve_through = [&](int64_t data_end)
  {
    if (!can_reserve || data_end <= reserved)
      return;
    const int64_t grow_to = std::max(data_end, reserved + (int64_t{32} << 20));
    can_reserve = out.reserve(data_base + reserved, grow_to - reserved);
    reserved = grow_to;
  };
  const int64_t n = std::ssize(plan);
  cdict_ptr cdict{nullptr, &ZSTD_freeCDict};
  if (!dictionary.empty())
    cdict.reset(ZSTD_createCDict(dictionary.data(), dictionary.size(), cs.level));
  const codec dict_codec = cdict ? codec::zstd_dict : codec::zstd;

  int64_t offset = 0;
  int64_t i = 0;
  std::vector<std::vector<char>> staging;

  while (i < n)
  {
    // Too large to stage: streamed on its own, at a known offset.
    if (plan[static_cast<std::size_t>(i)].shares_with >= 0)
    {
      i++; // placed by the entry it shares with
      continue;
    }

    if (plan[static_cast<std::size_t>(i)].size > compression_batch_bytes)
    {
      auto& p = plan[static_cast<std::size_t>(i)];
      const bool try_it = (cs.method == compression::always)
                          || sample_says_compress(p.src->path_in_system, cs);

      int64_t written = -1;
      if (try_it)
      {
        // Aligned as if compressed; a fallback to store is still 64-byte
        // aligned because that alignment divides it.
        const int64_t at = round_up(offset, payload_alignment);
        std::string error;
        try
        {
          reserve_through(at + p.size);
          written = stream_compress_into(
              p.src->path_in_system,
              p.size,
              payloads + at,
              p.size,
              cs,
              cdict.get(),
              error);
        }
        catch (const out_of_space&)
        {
          throw; // about the archive, not this input
        }
        catch (const std::exception& ex)
        {
          // stream_compress_into opens the source itself, so an unreadable or
          // vanished input surfaces here rather than through read_file.
          on_error(p, ex.what());
          i++;
          continue;
        }
        if (written >= 0 && worth_keeping(written, p.size, cs))
        {
          p.data_offset = at;
          p.stored_size = written;
          p.method = dict_codec;
          if (want_hashes)
            p.content_hash
                = hash_bytes(payloads + at, static_cast<std::size_t>(written));
          offset = at + written;
          i++;
          continue;
        }
        if (written < 0 && !error.empty()
            && error.find("would not") == std::string::npos)
        {
          // A genuine I/O or codec failure, not "compression did not help".
          on_error(p, error);
          i++;
          continue;
        }
      }

      // Store it verbatim, copied straight from the source.
      const int64_t at = round_up(offset, payload_alignment);
      try
      {
        reserve_through(at + p.size);
        auto fd = platform::file::open_read(p.src->path_in_system.c_str());
        platform::copy_file_into(fd, out, data_base + at, payloads + at, p.size);
        p.data_offset = at;
        p.stored_size = p.size;
        p.method = codec::store;
        if (want_hashes)
          p.content_hash = hash_bytes(payloads + at, static_cast<std::size_t>(p.size));
        offset = at + p.size;
      }
      catch (const std::exception& e)
      {
        on_error(p, e.what());
      }
      i++;
      continue;
    }

    // Otherwise take as many files as fit in one batch.
    int64_t j = i;
    int64_t batch_bytes = 0;
    while (j < n
           && (j == i
               || batch_bytes + plan[static_cast<std::size_t>(j)].size
                      <= compression_batch_bytes))
    {
      batch_bytes += plan[static_cast<std::size_t>(j)].size;
      j++;
    }
    const int64_t count = j - i;
    staging.assign(static_cast<std::size_t>(count), {});

    parallel_for(
        count,
        threads,
        [&](int64_t k)
        {
          auto& p = plan[static_cast<std::size_t>(i + k)];
          auto& slot = staging[static_cast<std::size_t>(k)];
          if (p.size == 0 || p.shares_with >= 0)
          {
            p.method = codec::store;
            return;
          }

          // Runs on a worker thread, where an escaping exception means
          // std::terminate. The buffers here are the size of the payload.
          try
          {
            std::vector<char> raw;
            std::string error;
            if (!read_file(p.src->path_in_system, p.size, raw, error))
            {
              on_error(p, error);
              return;
            }

            const bool eligible = p.size >= cs.min_size;
            const bool sample_first = p.size >= sample_decision_threshold
                                      && cs.method == compression::automatic;
            bool try_it = eligible;
            if (try_it && sample_first)
            {
              const zstd_compressor probe;
              std::vector<char> tmp(static_cast<std::size_t>(zstd_bound(sample_bytes)));
              const auto packed = probe.compress(
                  tmp.data(),
                  std::ssize(tmp),
                  raw.data(),
                  std::min<int64_t>(sample_bytes, p.size),
                  cs.level,
                  nullptr);
              try_it
                  = worth_keeping(packed, std::min<int64_t>(sample_bytes, p.size), cs);
            }

            if (try_it)
            {
              thread_local const zstd_compressor compressor;
              std::vector<char> packed(static_cast<std::size_t>(zstd_bound(p.size)));
              const auto size = compressor.compress(
                  packed.data(),
                  std::ssize(packed),
                  raw.data(),
                  p.size,
                  cs.level,
                  cdict.get());
              if (worth_keeping(size, p.size, cs))
              {
                packed.resize(static_cast<std::size_t>(size));
                slot = std::move(packed);
                p.method = dict_codec;
                return;
              }
            }
            slot = std::move(raw);
            p.method = codec::store;
          }
          catch (const std::exception& ex)
          {
            on_error(p, ex.what());
          }
          catch (...)
          {
            on_error(p, "unknown error while compressing");
          }
        });

    // Sorted order, so the layout does not depend on thread scheduling.
    for (int64_t k = 0; k < count; k++)
    {
      auto& p = plan[static_cast<std::size_t>(i + k)];
      if (p.shares_with >= 0)
        continue;
      const int64_t align
          = (p.method == codec::store) ? payload_alignment : compressed_alignment;
      offset = round_up(offset, align);
      p.data_offset = offset;
      p.stored_size = std::ssize(staging[static_cast<std::size_t>(k)]);
      offset += p.stored_size;
    }
    // All offsets known, so reserve before any worker stores a byte.
    reserve_through(offset);

    parallel_for(
        count,
        threads,
        [&](int64_t k)
        {
          auto& p = plan[static_cast<std::size_t>(i + k)];
          if (p.shares_with >= 0)
            return;
          try
          {
            auto& slot = staging[static_cast<std::size_t>(k)];
            char* const dst = payloads + p.data_offset;
            if (!slot.empty())
              memcpy(dst, slot.data(), slot.size());
            if (want_hashes)
              p.content_hash = hash_bytes(dst, static_cast<std::size_t>(p.stored_size));
            std::vector<char>{}.swap(slot); // release the batch as we go
          }
          catch (const std::exception& ex)
          {
            on_error(p, ex.what());
          }
          catch (...)
          {
            on_error(p, "unknown error while writing a payload");
          }
        });

    i = j;
  }
  return offset;
#endif
}
} // namespace

struct writer::impl
{
  std::vector<pending> entries;
  std::vector<skipped_file> skipped;
  on_unreadable policy{on_unreadable::fail};
  on_duplicate duplicates{on_duplicate::fail};
  bool content_hashes{true};
  int threads{0};
  compression_settings compress{};
};

writer::writer()
    : impl{std::make_unique<struct impl>()}
{
}

writer::~writer() = default;

void writer::add_file(std::string_view path_in_archive, std::string_view path_in_system)
{
  if (const auto problem = check_archive_path(path_in_archive);
      problem != path_problem::ok)
    throw std::invalid_argument(std::string{"uvfs: rejecting archive path \""}
                                    .append(path_in_archive)
                                    .append("\": ")
                                    .append(describe(problem)));

  impl->entries.push_back(pending{
      .path_in_archive = std::string{path_in_archive},
      .path_in_system = std::string{path_in_system}});
}

void writer::set_unreadable_policy(on_unreadable policy) noexcept
{
  impl->policy = policy;
}

void writer::set_duplicate_policy(on_duplicate policy) noexcept
{
  impl->duplicates = policy;
}

void writer::set_content_hashes(bool enabled) noexcept
{
  impl->content_hashes = enabled;
}

void writer::set_thread_count(int threads) noexcept
{
  impl->threads = threads;
}

void writer::set_compression(const compression_settings& settings)
{
  if (settings.method != compression::none && !zstd_available())
    throw no_zstd_error("compression");
  if (settings.dictionary_size != 0 && !zstd_available())
    throw no_zstd_error("dictionary training");
  impl->compress = settings;
}

auto writer::skipped() const noexcept -> const std::vector<skipped_file>&
{
  return impl->skipped;
}

void writer::commit(std::string_view path)
{
  const std::string out{path};
  impl->skipped.clear();

  // ------------------------------------------------------------ duplicates
  {
    const std::size_t n = impl->entries.size();
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; i++)
      order[i] = i;
    // Stable, so within a run of duplicates the last is the most recent.
    std::stable_sort(
        order.begin(),
        order.end(),
        [&](std::size_t a, std::size_t b)
        { return impl->entries[a].path_in_archive < impl->entries[b].path_in_archive; });

    std::vector<bool> superseded(n, false);
    std::size_t duplicate_count = 0;
    const std::string* first_duplicate = nullptr;
    for (std::size_t i = 1; i < n; i++)
    {
      const auto& prev = impl->entries[order[i - 1]].path_in_archive;
      const auto& cur = impl->entries[order[i]].path_in_archive;
      if (prev != cur)
        continue;
      superseded[order[i - 1]] = true;
      duplicate_count++;
      if (!first_duplicate)
        first_duplicate = &cur;
    }

    if (duplicate_count > 0)
    {
      if (impl->duplicates == on_duplicate::fail)
        throw std::invalid_argument(
            "uvfs: " + std::to_string(duplicate_count)
            + " duplicate archive path(s), archive not written; first: "
            + *first_duplicate);

      std::vector<pending> deduped;
      deduped.reserve(n - duplicate_count);
      for (std::size_t i = 0; i < n; i++)
        if (!superseded[i])
          deduped.push_back(std::move(impl->entries[i]));
      impl->entries = std::move(deduped);
    }
  }

  // ---------------------------------------------------------------- sizing
  // Sized before anything is laid out, so unreadable files are dropped rather
  // than leaving a hole in a computed layout.
  std::vector<placed> plan;
  plan.reserve(impl->entries.size());

  for (const auto& e : impl->entries)
  {
    const auto st = platform::stat_path(e.path_in_system.c_str());
    if (!st.exists)
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system, "could not stat the file"});
      continue;
    }
    if (!st.regular)
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system, "not a regular file"});
      continue;
    }
    // Only worth a syscall per file under the skip policy; otherwise the copy
    // reports it anyway.
    if (impl->policy == on_unreadable::skip
        && !platform::readable(e.path_in_system.c_str()))
    {
      impl->skipped.push_back({e.path_in_archive, e.path_in_system, "not readable"});
      continue;
    }
    plan.push_back(
        placed{.src = &e, .size = st.size, .device = st.device, .inode = st.inode});
  }

  if (!impl->skipped.empty() && impl->policy == on_unreadable::fail)
  {
    auto msg = "uvfs: " + std::to_string(impl->skipped.size())
               + " file(s) could not be read, archive not written; first: "
               + impl->skipped.front().path_in_system + " ("
               + impl->skipped.front().reason + ")";
    throw commit_error{msg, impl->skipped};
  }

  // ---------------------------------------------------------------- layout
  if (std::ssize(plan) > max_file_count)
    throw std::invalid_argument(
        "uvfs: " + std::to_string(plan.size()) + " files exceeds the format's limit of "
        + std::to_string(max_file_count));

  // Entries are stored sorted by archive path. That is what makes the entry
  // array binary searchable and makes iteration come out in extraction order.
  std::sort(
      plan.begin(),
      plan.end(),
      [](const placed& a, const placed& b)
      { return a.src->path_in_archive < b.src->path_in_archive; });

  const int64_t n = std::ssize(plan);
  const bool compressing = impl->compress.method != compression::none;

  // One file can appear under several archive paths, added twice or hard
  // linked. Payload offsets were never required to be distinct, so entries can
  // share bytes at no read-time cost.
  {
    std::unordered_map<uint64_t, int64_t> first_by_file;
    first_by_file.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; i++)
    {
      auto& p = plan[static_cast<std::size_t>(i)];
      if (p.size == 0)
        continue;
      // One key, one lookup; a collision is resolved by the check below.
      const uint64_t key = p.device * 0x9e3779b97f4a7c15ull + p.inode;
      const auto [it, inserted] = first_by_file.try_emplace(key, i);
      if (inserted)
        continue;
      const auto& primary = plan[static_cast<std::size_t>(it->second)];
      if (primary.device == p.device && primary.inode == p.inode
          && primary.size == p.size)
        p.shares_with = it->second;
    }
  }

  header h{};
  memcpy(h.head, uvfs::ident, sizeof(uvfs::ident));

  int64_t names_size = 0;
  int64_t worst_case_data = 0;
  for (auto& p : plan)
  {
    p.name_offset = names_size;
    names_size += std::ssize(p.src->path_in_archive);
    if (p.shares_with >= 0)
      continue; // its bytes are already accounted for
    // A payload is never stored larger than the file, so the uncompressed
    // layout is an upper bound.
    worst_case_data = round_up(worst_case_data, payload_alignment) + p.size;
  }

  // Names are located by a 32-bit offset into one blob, so past 4 GiB the
  // offsets wrap and produce a structurally valid archive holding the wrong
  // names.
  if (names_size > max_names_size)
    throw std::invalid_argument(
        "uvfs: archive paths total " + std::to_string(names_size)
        + " bytes, over the format's limit of " + std::to_string(max_names_size)
        + "; use shorter names or split the archive");

  h.file_count = n;
  h.table_capacity = table_capacity_for(n);
  h.names_size = names_size;
  if (impl->content_hashes)
    h.flag_bits |= flag_entry_hashes;
  h.index_start = round_up_64(header_size);
  h.index_size = h.names_offset() + names_size;

  // ------------------------------------------------------------ dictionary
  std::vector<char> dictionary;
#if defined(UVFS_HAS_ZSTD)
  if (compressing && impl->compress.dictionary_size > 0 && n > 0)
    dictionary = train_dictionary_from(plan, impl->compress);
#endif

  if (!dictionary.empty())
  {
    h.flag_bits |= flag_has_dictionary;
    h.dict_hash = hash_bytes(dictionary.data(), dictionary.size());
    h.dict_start = round_up_8(h.index_start + h.index_size);
    h.dict_size = std::ssize(dictionary);
    h.data_start = round_up_64(h.dict_start + h.dict_size);
  }
  else
  {
    h.data_start = round_up_64(h.index_start + h.index_size);
  }

  // Built under a temporary name in the destination's own directory and
  // renamed into place, so a failure cannot leave a corrupt archive and the
  // rename stays within one filesystem.
  std::filesystem::path out_dir = std::filesystem::path{out}.parent_path();
  if (out_dir.empty())
    out_dir = std::filesystem::path{"."};
  const std::string tmp = (out_dir
                           / (".uvfs-tmp-" + std::to_string(platform::process_id()) + "-"
                              + std::to_string(reinterpret_cast<uintptr_t>(&out))))
                              .string();

  // With compression the final size is unknown until every payload is placed,
  // so the file is sized to the uncompressed upper bound and truncated back
  // down. The unwritten tail is a hole.
  const int64_t upper_bound = h.data_start + worst_case_data;

  std::vector<skipped_file> copy_errors;
  std::mutex errors_mutex;
  std::atomic<bool> had_error{false};

  // A guard rather than catch clauses, so cleanup does not depend on naming
  // the right exception types.
  scope_guard discard_temp{[&] { unlink_quietly(tmp); }};

  try
  {
    auto handle = platform::file::create_write(tmp.c_str());
    // Written through the mapping too, so they need real blocks.
    handle.reserve(0, h.data_start);
    if (!compressing)
    {
      // Sizes are final, so reserve in one call.
      handle.reserve(h.data_start, worst_case_data);
    }
    // Otherwise reserved incrementally; see place_compressed.
    handle.resize(upper_bound);

    int64_t data_used = 0;
    {
      auto data = output_region::create(handle, upper_bound);
      char* const payloads = data.data() + h.data_start;

      if (!dictionary.empty())
        memcpy(data.data() + h.dict_start, dictionary.data(), dictionary.size());

      // Called from worker threads, so it must not throw. If recording the
      // detail fails, the flag still fails the commit.
      const auto record_error = [&](const placed& p, const std::string& why) noexcept
      {
        had_error.store(true, std::memory_order_relaxed);
        try
        {
          const std::lock_guard lock{errors_mutex};
          copy_errors.push_back({p.src->path_in_archive, p.src->path_in_system, why});
        }
        // Deliberately empty: if even recording the detail fails we have
        // nothing left to say, and the flag above has already failed the
        // commit. Rethrowing here would escape a worker thread.
        // NOLINTNEXTLINE(bugprone-empty-catch)
        catch (...)
        {
        }
      };

      const bool want_hashes = impl->content_hashes;
      const int threads = impl->threads;
      if (!compressing)
      {
        // Sizes are final, so offsets are assigned up front and every payload
        // copied into its slot in one pass.
        int64_t offset = 0;
        for (auto& p : plan)
        {
          p.method = codec::store;
          if (p.shares_with >= 0)
            continue;
          offset = round_up(offset, payload_alignment);
          p.data_offset = offset;
          p.stored_size = p.size;
          offset += p.size;
        }
        data_used = offset;

        parallel_for(
            n,
            threads,
            [&](int64_t i)
            {
              auto& p = plan[static_cast<std::size_t>(i)];
              if (p.shares_with >= 0)
                return; // written by the entry it shares with
              char* const dst = payloads + p.data_offset;
              if (p.size == 0)
              {
                if (want_hashes)
                  p.content_hash = hash_bytes("", 0);
                return;
              }
              // An exception escaping a thread's entry point calls
              // std::terminate, so failures are recorded, not thrown.
              try
              {
                auto fd = platform::file::open_read(p.src->path_in_system.c_str());
                const auto actual = fd.size();
                if (actual != p.size)
                  throw std::runtime_error(
                      "file changed size while the archive was being written ("
                      + std::to_string(p.size) + " -> " + std::to_string(actual) + ")");
                platform::copy_file_into(
                    fd, handle, h.data_start + p.data_offset, dst, p.size);
                if (want_hashes)
                  p.content_hash = hash_bytes(dst, static_cast<std::size_t>(p.size));
              }
              catch (const std::exception& ex)
              {
                record_error(p, ex.what());
              }
            },
            copy_thread_cap);
      }
      else
      {
        data_used = place_compressed(
            plan,
            payloads,
            handle,
            h.data_start,
            dictionary,
            impl->compress,
            impl->content_hashes,
            impl->threads,
            record_error);
      }

      if (!had_error.load(std::memory_order_relaxed) && copy_errors.empty())
      {
        // Sharers adopt the placement of the entry that wrote the bytes; the
        // primary is the first occurrence in sorted order.
        for (auto& p : plan)
        {
          if (p.shares_with < 0)
            continue;
          const auto& primary = plan[static_cast<std::size_t>(p.shares_with)];
          p.data_offset = primary.data_offset;
          p.stored_size = primary.stored_size;
          p.method = primary.method;
          p.content_hash = primary.content_hash;
        }

        // Written only now: with compression a payload's size and offset are
        // not known until it is placed. The result is the finished index a
        // reader uses directly.
        h.data_size = data_used;
        h.file_size = h.data_start + data_used;

        auto* const index = data.data() + h.index_start;
        auto* const table = index + h.table_offset();
        auto* const names = index + h.names_offset();

        for (int64_t i = 0; i < h.table_capacity; i++)
          store<uint64_t>(table + i * 8, empty_slot);

        const auto mask = static_cast<uint64_t>(h.table_capacity - 1);
        for (int64_t i = 0; i < n; i++)
        {
          const auto& p = plan[static_cast<std::size_t>(i)];
          const auto& name = p.src->path_in_archive;

          const entry e{
              .data_offset = p.data_offset,
              .stored_size = p.stored_size,
              .orig_size = p.size,
              .name_offset = static_cast<uint32_t>(p.name_offset),
              .name_size = static_cast<uint16_t>(name.size()),
              .method = p.method};
          e.store_to(index + i * entry_size);
          // The name blob is length-prefixed by the index, so terminators are
          // neither stored nor wanted.
          // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
          memcpy(names + p.name_offset, name.data(), name.size());

          // The high 32 bits ride along as a fingerprint, so a lookup can
          // reject a colliding slot without touching the entry or the name.
          const uint64_t hv = hash_name(name);
          uint64_t slot = hv & mask;
          while (load<uint64_t>(table + slot * 8) != empty_slot)
            slot = (slot + 1) & mask;
          store<uint64_t>(
              table + slot * 8,
              ((hv >> 32) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(i)));
        }

        // Written outwards: content hashes live in the index, the index hash
        // covers the index, the header hash covers the index hash.
        if (h.has(flag_entry_hashes))
        {
          auto* const hashes = index + h.hashes_offset();
          for (int64_t i = 0; i < n; i++)
            store<uint64_t>(
                hashes + i * 8, plan[static_cast<std::size_t>(i)].content_hash);
        }

        h.index_hash = hash_bytes(index, static_cast<std::size_t>(h.index_size));
        h.store_to(data.data());
        h.header_hash = hash_bytes(data.data(), header::hashed_prefix);
        store<uint64_t>(data.data() + header::hashed_prefix, h.header_hash);

        data.flush(h.file_size);
      }
    } // unmap

    if (had_error.load(std::memory_order_relaxed) || !copy_errors.empty())
    {
      if (copy_errors.empty())
        copy_errors.push_back({"", "", "out of memory while building the archive"});
      handle.close();
      auto msg = "uvfs: " + std::to_string(copy_errors.size())
                 + " file(s) failed while being copied, archive not written; "
                   "first: "
                 + copy_errors.front().path_in_system + " ("
                 + copy_errors.front().reason + ")";
      throw commit_error{msg, std::move(copy_errors)};
    }

    handle.resize(h.file_size);
    handle.flush();
    handle.close();

    if (!platform::rename_replace(tmp.c_str(), out.c_str()))
      throw std::runtime_error(
          "uvfs: could not publish archive (" + platform::last_error() + "): ");

    discard_temp.dismiss();
  }
  catch (const commit_error&)
  {
    throw;
  }
  catch (const std::runtime_error& e)
  {
    // These end in ": " by convention and concern the output, the only thing
    // left that can fail once per-input errors are collected.
    throw std::runtime_error(std::string(e.what()).append(out));
  }
  // Anything else propagates; the guard still runs.
}

}
