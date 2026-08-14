#include "fd_handle.hpp"
#include "format.hpp"
#include "hash.hpp"

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
  int64_t size{};
  int64_t target_offset{};
  int64_t name_offset{};
};

//! Removes a file, ignoring failure. Used on the error path, where the
//! original error is the one worth reporting.
void unlink_quietly(const std::string& p) noexcept
{
  ::unlink(p.c_str());
}
} // namespace

struct writer::impl
{
  std::vector<pending> entries;
  std::vector<skipped_file> skipped;
  on_unreadable policy{on_unreadable::fail};
  on_duplicate duplicates{on_duplicate::fail};
};

writer::writer()
    : impl{std::make_unique<struct impl>()}
{
}

writer::~writer() = default;

void writer::add_file(
    std::string_view path_in_archive, std::string_view path_in_system)
{
  if (const auto problem = check_archive_path(path_in_archive);
      problem != path_problem::ok)
    throw std::invalid_argument(
        std::string{"uvfs: rejecting archive path \""}
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

auto writer::skipped() const noexcept -> const std::vector<skipped_file>&
{
  return impl->skipped;
}

void writer::commit(std::string_view path)
{
  const std::string out{path};
  impl->skipped.clear();

  // ------------------------------------------------------------ duplicates
  // The same archive path twice used to produce an archive whose header
  // claimed N files while the reader exposed N-1, with one payload written but
  // permanently unreachable.
  {
    const std::size_t n = impl->entries.size();
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; i++)
      order[i] = i;
    // A stable sort keeps equal paths in registration order, so within a run
    // of duplicates the last element is the most recent registration.
    std::stable_sort(
        order.begin(), order.end(),
        [&](std::size_t a, std::size_t b) {
          return impl->entries[a].path_in_archive < impl->entries[b].path_in_archive;
        });

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

      // on_duplicate::replace -- keep the last registration of each path,
      // in the original registration order.
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
    // stat() is given a NUL-terminated std::string, never a string_view's
    // .data(), which would read past the end of the view.
    struct stat st
    {
    };
    if (::stat(e.path_in_system.c_str(), &st) != 0)
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system,
           "could not stat: " + errno_string(errno)});
      continue;
    }
    if (!S_ISREG(st.st_mode))
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system, "not a regular file"});
      continue;
    }
    if (::access(e.path_in_system.c_str(), R_OK) != 0)
    {
      impl->skipped.push_back(
          {e.path_in_archive, e.path_in_system,
           "not readable: " + errno_string(errno)});
      continue;
    }
    plan.push_back(placed{.src = &e, .size = st.st_size});
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
        "uvfs: " + std::to_string(plan.size())
        + " files exceeds the format's limit of "
        + std::to_string(max_file_count));

  // Entries are stored sorted by archive path. That is what makes the entry
  // array binary searchable and makes iteration come out in extraction order.
  std::sort(
      plan.begin(), plan.end(),
      [](const placed& a, const placed& b)
      { return a.src->path_in_archive < b.src->path_in_archive; });

  header h{};
  memcpy(h.head, uvfs::ident, sizeof(uvfs::ident));

  int64_t names_size = 0;
  int64_t data_size = 0;
  for (auto& p : plan)
  {
    p.name_offset = names_size;
    names_size += std::ssize(p.src->path_in_archive);

    // Stored payloads carry the 64-byte alignment promise.
    p.target_offset = round_up(data_size, payload_alignment);
    data_size = p.target_offset + p.size;
  }

  h.file_count = std::ssize(plan);
  h.table_capacity = table_capacity_for(h.file_count);
  h.names_size = names_size;
  h.index_start = round_up_64(header_size);
  h.index_size = h.names_offset() + names_size;
  h.data_start = round_up_64(h.index_start + h.index_size);
  h.data_size = data_size;
  h.file_size = h.data_start + h.data_size;

  // ------------------------------------------------------------- temp file
  // The archive is built under a temporary name in the destination directory
  // and renamed into place at the end, so a failure part way through cannot
  // leave a corrupt archive where a good one used to be.
  const auto slash = out.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string{"."}
                                                       : out.substr(0, slash);
  const std::string tmp
      = dir + "/.uvfs-tmp-" + std::to_string(::getpid()) + "-"
        + std::to_string(reinterpret_cast<uintptr_t>(&out));

  std::vector<skipped_file> copy_errors;
  try
  {
    auto handle = fd_handle::create_rw(tmp.c_str(), 0644);
    handle.reserve_space(h.file_size);
    handle.resize(h.file_size);

    {
      const auto data = handle.map_rw(h.file_size);
      h.store_to(data.bytes);

      // ------------------------------------------------------------- index
      // The index is written in its final, ready-to-use form: a sorted entry
      // array, a populated hash table and a name blob. A reader maps the file
      // and uses them directly; none of this is recomputed at open.
      auto* const index = data.bytes + h.index_start;
      auto* const table = index + h.table_offset();
      auto* const names = index + h.names_offset();

      for (int64_t i = 0; i < h.table_capacity; i++)
        store<uint64_t>(table + i * 8, empty_slot);

      const auto mask = static_cast<uint64_t>(h.table_capacity - 1);
      for (int64_t i = 0; i < h.file_count; i++)
      {
        const auto& p = plan[static_cast<std::size_t>(i)];
        const auto& name = p.src->path_in_archive;

        const entry e{
            .data_offset = p.target_offset,
            .stored_size = p.size,
            .orig_size = p.size,
            .name_offset = static_cast<uint32_t>(p.name_offset),
            .name_size = static_cast<uint16_t>(name.size()),
            .method = codec::store};
        e.store_to(index + i * entry_size);
        memcpy(names + p.name_offset, name.data(), name.size());

        // Insert into the open-addressed table. The high 32 bits of the hash
        // ride along as a fingerprint so a lookup can reject a colliding slot
        // without dereferencing the entry or the name.
        const uint64_t hv = hash_name(name);
        uint64_t slot = hv & mask;
        while (load<uint64_t>(table + slot * 8) != empty_slot)
          slot = (slot + 1) & mask;
        store<uint64_t>(
            table + slot * 8,
            ((hv >> 32) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(i)));
      }

      // ----------------------------------------------------------- payload
      // Workers pull from a shared counter instead of being fed by a queue,
      // so there is no producer to wait on, nothing spins, and a thread that
      // finishes early simply takes the next item.
      const int64_t n = std::ssize(plan);
      unsigned hw = std::thread::hardware_concurrency();
      if (hw == 0)
        hw = 4;
      const auto threads = static_cast<int>(
          std::min<int64_t>(static_cast<int64_t>(hw), std::max<int64_t>(n, 1)));

      std::atomic<int64_t> next{0};
      std::mutex errors_mutex;

      const auto worker = [&]
      {
        for (;;)
        {
          const int64_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= n)
            return;
          const auto& p = plan[static_cast<size_t>(i)];
          if (p.size == 0)
            continue;

          // An exception escaping a thread's entry point calls
          // std::terminate, which is why archiving a live directory used to
          // abort the process. Every failure below is recorded and reported
          // by commit() instead.
          try
          {
            auto fd = fd_handle::open_ro(p.src->path_in_system.c_str());
            const auto actual = fd.filesize();
            if (actual != p.size)
              throw std::runtime_error(
                  "file changed size while the archive was being written ("
                  + std::to_string(p.size) + " -> " + std::to_string(actual)
                  + "): ");

            auto srcfile = fd.map_ro(actual);
            if (actual > 0)
              memcpy(
                  data.bytes + h.data_start + p.target_offset, srcfile.bytes,
                  static_cast<size_t>(actual));
          }
          catch (const std::exception& ex)
          {
            const std::lock_guard lock{errors_mutex};
            copy_errors.push_back(
                {p.src->path_in_archive, p.src->path_in_system, ex.what()});
          }
        }
      };

      {
        std::vector<std::jthread> pool;
        pool.reserve(static_cast<size_t>(threads));
        for (int i = 0; i < threads; i++)
          pool.emplace_back(worker);
      } // joins

      if (copy_errors.empty())
      {
        // Push our own dirty pages, rather than every dirty page on the
        // machine, which is what a bare sync() does.
        if (msync(data.bytes, static_cast<size_t>(h.file_size), MS_SYNC) != 0)
          throw std::runtime_error(
              "uvfs: could not flush mapping (" + errno_string(errno) + "): ");
      }
    } // unmap

    if (!copy_errors.empty())
    {
      handle.close_now();
      unlink_quietly(tmp);
      auto msg = "uvfs: " + std::to_string(copy_errors.size())
                 + " file(s) failed while being copied, archive not written; "
                   "first: "
                 + copy_errors.front().path_in_system + " ("
                 + copy_errors.front().reason + ")";
      throw commit_error{msg, std::move(copy_errors)};
    }

    handle.sync();
    handle.close_now();

    if (::rename(tmp.c_str(), out.c_str()) != 0)
    {
      const auto err = errno_string(errno);
      unlink_quietly(tmp);
      throw std::runtime_error("uvfs: could not publish archive (" + err + "): ");
    }
  }
  catch (const commit_error&)
  {
    throw;
  }
  catch (const std::runtime_error& e)
  {
    unlink_quietly(tmp);
    throw std::runtime_error(std::string(e.what()).append(out));
  }
}
}
