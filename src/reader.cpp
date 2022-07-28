#include "fd_handle.hpp"
#include "format.hpp"

#include <ankerl/unordered_dense.h>
#include <uvfs/reader.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace uvfs
{
struct loaded_file_entry
{
  int64_t start{}, len{};
  const char* data{};
};
struct reader::impl
{
  explicit impl(std::string_view path)
      : handle{fd_handle::open_ro(path)}
  {
  }

  ankerl::unordered_dense::map<std::string_view, loaded_file_entry> entries;
  fd_handle handle;
  decltype(handle.map_ro(1)) data;
};

reader::reader(std::string_view path)
try : impl{std::make_unique<struct impl>(path)}
{
  const auto filesize = impl->handle.filesize();
  if (filesize < static_cast<int64_t>(sizeof(header)))
    throw std::runtime_error("uvfs: invalid file size: ");

  impl->data = impl->handle.map_ro(filesize);
  auto& data = impl->data;

  const auto* const h = reinterpret_cast<const header*>(data.bytes);
  h->validate(filesize);
  int64_t entry_idx = 0;

  const auto* ptr = data.bytes + h->index_start;
  impl->entries.reserve(h->file_count);

  for (int64_t i = 0; i < h->file_count; i++)
  {
    const auto* const e = reinterpret_cast<const entry*>(ptr + entry_idx);

    impl->entries[std::string_view(e->path, e->path_len)] = loaded_file_entry{
        .start = e->data_start,
        .len = e->data_size,
        .data = (data.bytes + h->data_start + e->data_start)};

    if (e->data_start < 0)
      throw std::runtime_error("uvfs: invalid data entry: ");
    if (e->data_size < 0)
      throw std::runtime_error("uvfs: invalid data entry: ");
    if (e->data_size > h->data_size)
      throw std::runtime_error("uvfs: invalid data entry: ");
    if (h->data_start + e->data_start + e->data_size >= filesize)
      throw std::runtime_error("uvfs: invalid data entry: ");

    const auto entry_size = entry::static_size + e->path_len;
    entry_idx = round_up_8(entry_idx + entry_size);
    if (entry_idx > h->index_size)
      throw std::runtime_error("uvfs: invalid index entry: ");
  }
}
catch (const std::runtime_error& e)
{
  throw std::runtime_error(std::string(e.what()).append(path));
}

reader::~reader() = default;

auto reader::find(std::string_view path) const noexcept
    -> std::optional<std::string_view>
{
  assert(idx);
  auto it = impl->entries.find(path);
  if (it == impl->entries.end())
    return std::nullopt;
  else
    return std::string_view(it->second.data, it->second.len);
}

void reader::for_each_file(const std::function<bool(iter_entry)>& func) const
{
  for (auto& f : this->impl->entries)
  {
    if (!func({f.first, std::string_view(f.second.data, f.second.len)}))
      break;
  }
}

auto reader::size() const noexcept -> std::size_t
{
  return this->impl->entries.size();
}

}
