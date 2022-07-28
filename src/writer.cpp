#include "fd_handle.hpp"
#include "format.hpp"

#include <cmath>
#include <readerwriterqueue.h>
#include <uvfs/writer.hpp>

#include <array>
#include <thread>
#include <vector>

namespace uvfs
{
struct writer::impl
{
  impl()
  try
  {
    entries.reserve(100000);
  }
  catch (...)
  {
  }

  struct entry
  {
    std::string path_in_archive;
    std::string path_in_system;
    int64_t size{};
    int64_t target_offset{};
  };

  std::vector<entry> entries;

  int64_t entry_offset = 0;
  int64_t data_offset = 0;
};

writer::writer()
    : impl{std::make_unique<struct impl>()}
{
}

writer::~writer() = default;

void writer::add_file(std::string_view path_in_archive, std::string_view path_in_system)
{
  struct stat st
  {
  };
  stat(path_in_system.data(), &st);
  const auto filesize = st.st_size;

  impl->entries.push_back(impl::entry{
      .path_in_archive = std::string{path_in_archive},
      .path_in_system = std::string{path_in_system},
      .size = filesize,
      .target_offset = impl->data_offset});

  impl->entry_offset = round_up_8(
      impl->entry_offset + entry::static_size + std::ssize(path_in_archive));
  impl->data_offset = round_up_64(impl->data_offset + filesize);
}

void writer::commit(std::string_view path)
try
{
  auto handle = fd_handle::create_rw(path, 0644);

  header h{};
  memcpy(h.head, uvfs::ident, sizeof(uvfs::ident));
  h.file_count = std::ssize(impl->entries);
  h.index_start = round_up_64(sizeof(uvfs::header));
  h.index_size = impl->entry_offset;
  h.data_start = round_up_64(h.index_start + h.index_size);
  h.data_size = impl->data_offset;
  h.file_size = h.data_start + h.data_size;

  handle.resize(h.file_size);

  const auto data = handle.map_rw(h.file_size);
  memcpy(data.bytes, &h, sizeof(header));

  auto* const entry_ptr = data.bytes + round_up_64(sizeof(header));

  constexpr int threads = 16;
  struct msg
  {
    std::string_view path;
    int64_t size{};
    char* offset{};
  };

  using queue = moodycamel::ReaderWriterQueue<msg>;
  struct tstate_t
  {
    std::array<queue, threads> msgs;
    std::array<std::atomic_int64_t, threads> submission = {};
    std::array<std::atomic_int64_t, threads> processed = {};

    std::atomic_bool done = false;
  } tstate;

  std::array<std::jthread, threads> thr;
  for (int i = 0; i < threads; i++)
  {
    thr[i] = std::jthread{
        [&q = tstate.msgs[i], &done = tstate.done, &processed = tstate.processed[i]]
        {
          while (!done.load(std::memory_order_acquire))
          {
            msg m;
            while (q.try_dequeue(m))
            {
              auto fd = fd_handle::open_ro(m.path.data());

              const auto filesize = fd.filesize();
              if (filesize != m.size)
                throw std::runtime_error("uvfs: file size changed: ");

              auto srcfile = fd.map_ro(filesize);

              memcpy(m.offset, srcfile.bytes, filesize);
              processed.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }};
  }

  int64_t entry_pos = 0;
  for (auto& e : impl->entries)
  {
    auto* e_file = reinterpret_cast<entry*>(entry_ptr + entry_pos);
    e_file->data_start = e.target_offset;
    e_file->data_size = e.size;
    e_file->path_len = std::ssize(e.path_in_archive);
    memcpy(e_file->path, e.path_in_archive.data(), e.path_in_archive.size());

    entry_pos
        = round_up_8(entry_pos + entry::static_size + std::ssize(e.path_in_archive));

    if (e.size == 0)
      continue;

    int64_t smol_count = std::numeric_limits<int64_t>::max();
    int64_t smol_idx = 0;
    for (int i = 0; i < threads; i++)
    {
      int64_t cnt = tstate.submission[i] - tstate.processed[i];
      if (cnt == 0)
      {
        smol_idx = i;
        break;
      }
      else
      {
        if (cnt < smol_count)
        {
          smol_count = cnt;
          smol_idx = i;
        }
      }
    }

    tstate.submission[smol_idx].fetch_add(1, std::memory_order_relaxed);
    tstate.msgs[smol_idx].enqueue(
        msg{.path = e.path_in_system,
            .size = e.size,
            .offset = data.bytes + h.data_start + e.target_offset});
  }

  while (tstate.processed != tstate.submission)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  tstate.done.store(true, std::memory_order_release);

  sync();
}
catch (const std::runtime_error& e)
{
  throw std::runtime_error(std::string(e.what()).append(path));
}
}
