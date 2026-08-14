#include "fd_handle.hpp"
#include "format.hpp"

#include <ankerl/unordered_dense.h>
#include <uvfs/reader.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>

namespace uvfs
{
struct loaded_file_entry
{
  int64_t len{};
  const char* data{};
};
struct reader::impl
{
  explicit impl(std::string_view path)
      : handle{fd_handle::open_ro(std::string{path}.c_str())}
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

  const header h = header::load_from(data.bytes);
  h.validate(filesize);

  const auto* const index = data.bytes + h.index_start;
  const auto* const payloads = data.bytes + h.data_start;
  impl->entries.reserve(h.file_count);

  int64_t entry_idx = 0;
  for (int64_t i = 0; i < h.file_count; i++)
  {
    // Every field below is checked before it is used to form a pointer or a
    // view. Reading first and validating afterwards would mean the corrupt
    // value had already been dereferenced.
    if (entry_idx > h.index_size - entry::static_size)
      throw std::runtime_error("uvfs: index entry extends past the index: ");

    const auto* const raw = index + entry_idx;
    const entry e = entry::load_from(raw);

    const int64_t path_len = e.path_len;
    if (path_len <= 0)
      throw std::runtime_error("uvfs: entry has a non-positive path length: ");
    if (path_len > h.index_size - entry_idx - entry::static_size)
      throw std::runtime_error("uvfs: entry path extends past the index: ");

    if (e.data_start < 0 || e.data_size < 0)
      throw std::runtime_error("uvfs: entry has a negative offset or size: ");
    if (e.data_start > h.data_size || e.data_size > h.data_size - e.data_start)
      throw std::runtime_error("uvfs: entry payload extends past the data region: ");

    impl->entries[std::string_view(
        entry::path_of(raw), static_cast<size_t>(path_len))]
        = loaded_file_entry{.len = e.data_size, .data = payloads + e.data_start};

    entry_idx = round_up_8(entry_idx + entry::static_size + path_len);
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
  assert(impl);
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
