#include "fd_handle.hpp"
#include "format.hpp"
#include "hash.hpp"
#include "scope_guard.hpp"
#include "zstd_codec.hpp"

#include <uvfs/path.hpp>
#include <uvfs/writer.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

//! An input that survived sizing and has been given a place in the archive.
struct placed
{
  const pending* src{};
  int64_t size{}; //!< the file's size on disk
  int64_t name_offset{};
  uint64_t device{};       //!< st_dev / st_ino identify the file itself, so
  uint64_t inode{};        //!< two names for one file can share a payload
  int64_t shares_with{-1}; //!< index of the entry holding the bytes, or -1

  // Filled in once the payload has actually been placed. With compression the
  // stored size is not known until the bytes have been through the codec, so
  // the index cannot be written until every payload has found its home.
  int64_t data_offset{};
  int64_t stored_size{};
  codec method{codec::store};
  uint64_t content_hash{};
};

void unlink_quietly(const std::string& p) noexcept
{
  ::unlink(p.c_str());
}

//! Copying files is bound by per-file syscalls and page-cache contention, not
//! by CPU, and measurably stops improving past about a dozen threads: on a
//! 48-thread machine, 200k small files take 0.37s with 12 threads and 0.45s
//! with 48. Compression is the opposite -- it is CPU-bound and scales all the
//! way out (1.31s to 0.21s from 1 to 48 threads on the same corpus) -- so the
//! two phases get different defaults.
constexpr int copy_thread_cap = 12;

[[nodiscard]] auto worker_count(int64_t items, int requested, int cap = 0) -> int
{
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
}

//! Runs `body(i)` for i in [0, n) across a pool sized to the work.
template <typename F>
void parallel_for(int64_t n, int requested_threads, F&& body, int cap = 0)
{
  if (n <= 0)
    return;
  const int threads = worker_count(n, requested_threads, cap);
  std::atomic<int64_t> next{0};

  // std::thread rather than std::jthread: jthread needs libc++ 18, which is
  // newer than the toolchains shipped with several targets this has to build
  // on. Nothing here wants a stop token, so the only thing jthread was
  // providing was the join, and a guard does that on every path -- including
  // an exception from emplace_back part way through starting the pool, where
  // leaving a thread unjoined would call std::terminate.
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  const scope_guard join_all{[&]
                             {
                               for (auto& t : pool)
                                 if (t.joinable())
                                   t.join();
                             }};

  for (int t = 0; t < threads; t++)
    pool.emplace_back(
        [&]
        {
          for (;;)
          {
            const int64_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n)
              return;
            body(i);
          }
        });
}

#if defined(UVFS_HAS_ZSTD)
//! Reads a whole file. Returns false and fills `error` rather than throwing,
//! because this runs on worker threads. Only the compression path needs it;
//! the store path copies straight into the mapping.
[[nodiscard]] auto read_file(
    const std::string& path,
    int64_t expected,
    std::vector<char>& out,
    std::string& error) -> bool
{
  try
  {
    auto fd = fd_handle::open_ro(path.c_str());
    const auto actual = fd.filesize();
    if (actual != expected)
    {
      error = "file changed size while the archive was being written ("
              + std::to_string(expected) + " -> " + std::to_string(actual) + ")";
      return false;
    }
    out.resize(static_cast<std::size_t>(actual));
    int64_t done = 0;
    while (done < actual)
    {
      const auto got = ::pread(
          fd.get(), out.data() + done, static_cast<size_t>(actual - done), done);
      if (got < 0)
      {
        error = "read failed: " + errno_string(errno);
        return false;
      }
      if (got == 0)
      {
        error = "file ended early";
        return false;
      }
      done += got;
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

//! Above this size a payload is copied by the kernel with copy_file_range
//! instead of being pulled through user space. Below it the extra syscall
//! costs more than the copy saves.
constexpr int64_t kernel_copy_threshold = int64_t{256} * 1024;

//! Copies `size` bytes of `src` into the destination file at `dst_offset`,
//! which is also mapped at `dst`.
//!
//! Small files go through pread. mmap-per-file used to be used here, and it
//! cost two syscalls plus a TLB shootdown broadcast to every core -- with
//! dozens of writer threads that was the single most expensive thing the
//! writer did. Large files are handed to copy_file_range so the bytes never
//! enter user space at all; it can also reflink instead of copying on
//! filesystems that support it.
void copy_payload(
    const fd_handle& src,
    int dst_fd,
    int64_t dst_offset,
    char* dst,
    int64_t size)
{
#if defined(__linux__)
  if (size >= kernel_copy_threshold)
  {
    int64_t done = 0;
    bool usable = true;
    while (done < size && usable)
    {
      off_t in_off = done;
      off_t out_off = dst_offset + done;
      const auto moved = ::copy_file_range(
          src.get(), &in_off, dst_fd, &out_off, static_cast<size_t>(size - done), 0);
      if (moved > 0)
        done += moved;
      else
        usable = false; // not supported here (EXDEV, EINVAL, ...); fall back
    }
    if (done == size)
      return;
    // Partially copied by the kernel: finish the rest through pread. The
    // source offset stays absolute (done + rest) while the destination is
    // addressed through the mapping, so dst_offset plays no further part.
    dst += done;
    size -= done;
    int64_t rest = 0;
    while (rest < size)
    {
      const auto got = ::pread(
          src.get(), dst + rest, static_cast<size_t>(size - rest), done + rest);
      if (got < 0)
        throw std::runtime_error("read failed: " + errno_string(errno));
      if (got == 0)
        throw std::runtime_error("file ended early");
      rest += got;
    }
    return;
  }
#else
  (void)dst_fd;
  (void)dst_offset;
#endif

  int64_t done = 0;
  while (done < size)
  {
    const auto got
        = ::pread(src.get(), dst + done, static_cast<size_t>(size - done), done);
    if (got < 0)
      throw std::runtime_error("read failed: " + errno_string(errno));
    if (got == 0)
      throw std::runtime_error("file ended early");
    done += got;
  }
}

//! How many bytes of input a single compression batch holds in memory at once.
constexpr int64_t compression_batch_bytes = int64_t{64} * 1024 * 1024;
//! Above this size, whether a payload is worth compressing is decided from a
//! sample rather than by compressing the whole thing. This is what keeps a
//! multi-gigabyte video from being compressed in full only to discover that it
//! was already compressed.
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
  // Saving a couple of percent is not worth giving up the zero-copy pointer
  // and paying to decompress on every read.
  const int64_t saved = original - compressed;
  return saved * 100 >= original * cs.min_gain_percent;
}

using cdict_ptr = std::unique_ptr<ZSTD_CDict, size_t (*)(ZSTD_CDict*)>;

//! Decides from the first `sample_bytes` whether a large payload is worth
//! compressing at all.
[[nodiscard]] auto
sample_says_compress(const std::string& path, const compression_settings& cs) -> bool
{
  try
  {
    auto fd = fd_handle::open_ro(path.c_str());
    std::vector<char> sample(static_cast<std::size_t>(sample_bytes));
    const auto got = ::pread(fd.get(), sample.data(), sample.size(), 0);
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

//! Trains a zstd dictionary from a sample of the inputs. A shared dictionary
//! is what makes compression work on many small files: each payload on its own
//! is too short for zstd to build up any history, but they resemble each other.
[[nodiscard]] auto train_dictionary_from(
    const std::vector<placed>& plan,
    const compression_settings& cs) -> std::vector<char>
{
  // Sample small files: they are the ones a dictionary helps, and feeding
  // multi-megabyte payloads to the trainer wastes time and skews the result.
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
  // The trainer wants a decent multiple of the dictionary size to work from.
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
    return {}; // not enough material to learn from; carry on without one
  dict.resize(produced);
  return dict;
}

//! Compresses a payload too large to stage in memory, straight into the
//! mapping. Returns the stored size, or -1 if the result would not be smaller
//! than the input, in which case the caller stores the raw bytes instead.
[[nodiscard]] auto stream_compress_into(
    const std::string& path,
    int64_t size,
    char* dst,
    int64_t dst_capacity,
    const compression_settings& cs,
    const ZSTD_CDict* dict,
    std::string& error) -> int64_t
{
  auto fd = fd_handle::open_ro(path.c_str());
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
    const auto got
        = ::pread(fd.get(), in.data(), static_cast<std::size_t>(want), consumed);
    if (got <= 0)
    {
      error = got < 0 ? "read failed: " + errno_string(errno) : "file ended early";
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
//! Places every payload, compressing where it pays. Returns the number of
//! bytes of data region used.
//!
//! Work is done in batches so that memory stays bounded by the batch rather
//! than by the archive, and offsets are assigned sequentially in sorted order
//! within each batch, which keeps the output byte-for-byte reproducible even
//! though the compression itself runs in parallel.
template <typename OnError>
auto place_compressed(
    std::vector<placed>& plan,
    char* payloads,
    const fd_handle& out,
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
  const int dst_fd = out.get();

  // Blocks are reserved ahead of the cursor, in chunks, because the final size
  // is only known once every payload has been compressed. Writing through the
  // mapping into a hole that the filesystem cannot fill is a SIGBUS, not an
  // error return, so this has to happen before the bytes are stored.
  int64_t reserved = 0;
  bool can_reserve = true;
  const auto reserve_through = [&](int64_t data_end)
  {
    if (!can_reserve || data_end <= reserved)
      return;
    const int64_t grow_to = std::max(data_end, reserved + (int64_t{32} << 20));
    can_reserve = out.reserve_range(data_base + reserved, grow_to - reserved);
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
    // A payload too large to stage goes on its own and is streamed straight
    // into the mapping, where its offset is already known.
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
        // Aligning as if compressed; if it falls back to store the offset is
        // still 64-byte aligned because compressed alignment divides it.
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
          throw; // about the archive, not about this input
        }
        catch (const std::exception& ex)
        {
          // stream_compress_into opens the source itself, so an unreadable or
          // vanished input surfaces here rather than through read_file. Left
          // unhandled it escaped as a bare runtime_error, losing the per-file
          // list the caller needs.
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
        auto fd = fd_handle::open_ro(p.src->path_in_system.c_str());
        copy_payload(fd, dst_fd, data_base + at, payloads + at, p.size);
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

          // Everything below runs on a worker thread, where an escaping
          // exception means std::terminate. The staging and compression
          // buffers are the size of the payload, so std::bad_alloc here is not
          // hypothetical: it is what a large batch under memory pressure does.
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

    // Offsets in sorted order, so the layout does not depend on which thread
    // finished first.
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
    // Every offset in this batch is known now, so the blocks behind them can
    // be reserved in one go before any worker stores a byte.
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
    // A stable sort keeps equal paths in registration order, so within a run
    // of duplicates the last element is the most recent registration.
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
  // Everything is sized before anything is laid out, so that files which
  // cannot be read are removed from the plan rather than leaving a hole in a
  // layout that has already been computed.
  std::vector<placed> plan;
  plan.reserve(impl->entries.size());

  for (const auto& e : impl->entries)
  {
    struct stat st
    {
    };
    if (::stat(e.path_in_system.c_str(), &st) != 0)
    {
      impl->skipped.push_back(
          {e.path_in_archive,
           e.path_in_system,
           "could not stat: " + errno_string(errno)});
      continue;
    }
    if (!S_ISREG(st.st_mode))
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system, "not a regular file"});
      continue;
    }
    // Only worth a syscall per file when the caller wants unreadable inputs
    // silently skipped; otherwise the copy will fail and report it anyway.
    if (impl->policy == on_unreadable::skip
        && ::access(e.path_in_system.c_str(), R_OK) != 0)
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system, "not readable: " + errno_string(errno)});
      continue;
    }
    plan.push_back(placed{
        .src = &e,
        .size = st.st_size,
        .device = static_cast<uint64_t>(st.st_dev),
        .inode = static_cast<uint64_t>(st.st_ino)});
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

  // The same file can appear under several archive paths, either because the
  // caller added it twice or because the tree contains hard links. Storing one
  // copy costs a lookup here and nothing at all at read time: the format never
  // required payload offsets to be distinct, so two entries can simply point
  // at the same bytes.
  {
    std::unordered_map<uint64_t, int64_t> first_by_file;
    first_by_file.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; i++)
    {
      auto& p = plan[static_cast<std::size_t>(i)];
      if (p.size == 0)
        continue;
      // Mixing the two into one key keeps this a single hash lookup; a
      // collision would only ever be resolved by the check below.
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
    // A payload is never stored larger than the file: if compression does not
    // shrink it, the raw bytes are stored instead. So the uncompressed layout
    // is an upper bound on the compressed one.
    worst_case_data = round_up(worst_case_data, payload_alignment) + p.size;
  }

  // Every entry locates its name by a 32-bit offset into one blob. Past 4 GiB
  // of names those offsets silently wrap, and the archive that comes out is
  // structurally valid, passes its own index hash, and hands back other
  // entries' names -- with commit() reporting success. Refusing is the only
  // honest option, since the field cannot address the data.
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

  // ------------------------------------------------------------- temp file
  // The archive is built under a temporary name in the destination directory
  // and renamed into place at the end, so a failure part way through cannot
  // leave a corrupt archive where a good one used to be.
  const auto slash = out.find_last_of('/');
  const std::string dir
      = (slash == std::string::npos) ? std::string{"."} : out.substr(0, slash);
  const std::string tmp = dir + "/.uvfs-tmp-" + std::to_string(::getpid()) + "-"
                          + std::to_string(reinterpret_cast<uintptr_t>(&out));

  // Compression makes the final size unknown until every payload is placed, so
  // the file is mapped at its uncompressed upper bound and truncated back down
  // once the real size is known. The unwritten tail is a hole and costs no
  // blocks. Only the uncompressed path can reserve space up front.
  const int64_t upper_bound = h.data_start + worst_case_data;

  std::vector<skipped_file> copy_errors;
  std::mutex errors_mutex;
  std::atomic<bool> had_error{false};

  // Removing the temporary in catch clauses only cleaned up for the exception
  // types those clauses happened to name: a std::bad_alloc or a
  // std::length_error escaped both of them and left a full-size file behind,
  // which for a large archive is a gigabyte of litter. The guard does not need
  // to know what went wrong.
  scope_guard discard_temp{[&] { unlink_quietly(tmp); }};

  try
  {
    auto handle = fd_handle::create_rw(tmp.c_str(), 0644);
    // The header, index and dictionary are written through the mapping too, so
    // they need real blocks behind them just as much as the payloads do.
    handle.reserve_range(0, h.data_start);
    if (!compressing)
    {
      // Sizes are final, so the whole thing can be reserved in one call.
      handle.reserve_range(h.data_start, worst_case_data);
    }
    // With compression the final size is not known yet, so the data region is
    // reserved incrementally as payloads are placed; see place_compressed.
    handle.resize(upper_bound);

    int64_t data_used = 0;
    {
      const auto data = handle.map_rw(upper_bound);
      char* const payloads = data.bytes + h.data_start;

      if (!dictionary.empty())
        memcpy(data.bytes + h.dict_start, dictionary.data(), dictionary.size());

      // Called from worker threads, so it must not throw: an exception here
      // would escape the very handler that exists to stop exceptions escaping.
      // If even recording the detail fails, the flag still fails the commit.
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

      const int dst_fd = handle.get();
      const bool want_hashes = impl->content_hashes;
      const int threads = impl->threads;
      if (!compressing)
      {
        // Sizes are already final, so offsets can be assigned up front and
        // every payload copied straight into its slot in one parallel pass.
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
              // std::terminate, which is why archiving a live directory used
              // to abort the process. Failures are recorded, not thrown.
              try
              {
                auto fd = fd_handle::open_ro(p.src->path_in_system.c_str());
                const auto actual = fd.filesize();
                if (actual != p.size)
                  throw std::runtime_error(
                      "file changed size while the archive was being written ("
                      + std::to_string(p.size) + " -> " + std::to_string(actual) + ")");
                copy_payload(fd, dst_fd, h.data_start + p.data_offset, dst, p.size);
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
        // Entries sharing a payload adopt the placement of the entry that
        // actually wrote the bytes. A batch never precedes its own primary,
        // because the primary is the first occurrence in sorted order.
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

        // --------------------------------------------------------- index
        // Written only now, because with compression the stored size and
        // offset of a payload are not known until it has been placed. The
        // index is written in its final, ready-to-use form: a sorted entry
        // array, a populated hash table and a name blob. A reader maps the
        // file and uses them directly.
        h.data_size = data_used;
        h.file_size = h.data_start + data_used;

        auto* const index = data.bytes + h.index_start;
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

          // Insert into the open-addressed table. The high 32 bits of the
          // hash ride along as a fingerprint so a lookup can reject a
          // colliding slot without dereferencing the entry or the name.
          const uint64_t hv = hash_name(name);
          uint64_t slot = hv & mask;
          while (load<uint64_t>(table + slot * 8) != empty_slot)
            slot = (slot + 1) & mask;
          store<uint64_t>(
              table + slot * 8,
              ((hv >> 32) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(i)));
        }

        // ----------------------------------------------------- integrity
        // Order matters: the content hashes live inside the index, the index
        // hash covers the index, and the header hash covers the header --
        // including the index hash. So they are written outwards.
        if (h.has(flag_entry_hashes))
        {
          auto* const hashes = index + h.hashes_offset();
          for (int64_t i = 0; i < n; i++)
            store<uint64_t>(
                hashes + i * 8, plan[static_cast<std::size_t>(i)].content_hash);
        }

        h.index_hash = hash_bytes(index, static_cast<std::size_t>(h.index_size));
        h.store_to(data.bytes);
        h.header_hash = hash_bytes(data.bytes, header::hashed_prefix);
        store<uint64_t>(data.bytes + header::hashed_prefix, h.header_hash);

        // Push our own dirty pages, rather than every dirty page on the
        // machine, which is what a bare sync() does.
        if (msync(data.bytes, static_cast<std::size_t>(h.file_size), MS_SYNC) != 0)
          throw std::runtime_error(
              "uvfs: could not flush mapping (" + errno_string(errno) + "): ");
      }
    } // unmap

    if (had_error.load(std::memory_order_relaxed) || !copy_errors.empty())
    {
      if (copy_errors.empty())
        copy_errors.push_back({"", "", "out of memory while building the archive"});
      handle.close_now();
      auto msg = "uvfs: " + std::to_string(copy_errors.size())
                 + " file(s) failed while being copied, archive not written; "
                   "first: "
                 + copy_errors.front().path_in_system + " ("
                 + copy_errors.front().reason + ")";
      throw commit_error{msg, std::move(copy_errors)};
    }

    // Give back whatever compression saved.
    handle.resize(h.file_size);
    handle.sync();
    handle.close_now();

    if (::rename(tmp.c_str(), out.c_str()) != 0)
      throw std::runtime_error(
          "uvfs: could not publish archive (" + errno_string(errno) + "): ");

    // The archive is in place under its final name; there is no temporary
    // left to remove.
    discard_temp.dismiss();
  }
  catch (const commit_error&)
  {
    throw;
  }
  catch (const std::runtime_error& e)
  {
    // Messages here end in ": " by convention and are about the output, which
    // is the only thing left that can fail once per-input errors are collected.
    throw std::runtime_error(std::string(e.what()).append(out));
  }
  // Every other exception type propagates unchanged, and the guard still runs.
}

}
