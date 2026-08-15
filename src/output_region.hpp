#pragma once
#include "platform.hpp"

#include <vector>

namespace uvfs
{

//! The block of bytes the writer builds an archive into.
//!
//! Everywhere with a working shared file mapping this *is* the file: stores go
//! straight to the page cache and flush() pushes them. Emscripten has mmap but
//! a MAP_SHARED mapping is never written back, so an archive built through one
//! would be silently lost; there the region is ordinary memory and flush()
//! writes it out. The writer does not care which it got -- it has a pointer
//! either way -- so there is one code path rather than two.
class output_region
{
public:
  static auto create(platform::file& f, int64_t size) -> output_region
  {
    output_region r;
    r.file_ = &f;
    r.size_ = size;
    if constexpr (platform::has_shared_writable_mapping)
    {
      r.map_ = platform::mapping::read_write(f, size);
      r.base_ = r.map_.data();
    }
    else
    {
      r.staging_.assign(static_cast<std::size_t>(size), '\0');
      r.base_ = r.staging_.data();
    }
    return r;
  }

  output_region() noexcept = default;
  output_region(const output_region&) = delete;
  auto operator=(const output_region&) -> output_region& = delete;
  output_region(output_region&&) noexcept = default;
  auto operator=(output_region&&) noexcept -> output_region& = default;

  [[nodiscard]] auto data() noexcept -> char* { return base_; }
  [[nodiscard]] auto data() const noexcept -> const char* { return base_; }
  [[nodiscard]] auto size() const noexcept -> int64_t { return size_; }

  //! True when writes land in the file as they are made. When false the bytes
  //! only reach the file at flush(), so nothing may read the file behind this
  //! object's back before then.
  [[nodiscard]] static constexpr auto is_mapped() noexcept -> bool
  {
    return platform::has_shared_writable_mapping;
  }

  //! Makes the first `bytes` of the region visible in the file. The archive is
  //! built into a region sized for the uncompressed worst case, so this is
  //! given the real size rather than the region's.
  void flush(int64_t bytes)
  {
    if (bytes <= 0 || !base_)
      return;
    if constexpr (platform::has_shared_writable_mapping)
      map_.flush(bytes);
    else
      file_->write_at(base_, bytes, 0);
  }

  void reset() noexcept
  {
    map_.reset();
    staging_.clear();
    staging_.shrink_to_fit();
    base_ = nullptr;
    size_ = 0;
  }

private:
  platform::file* file_{};
  platform::mapping map_;
  std::vector<char> staging_;
  char* base_{};
  int64_t size_{};
};

} // namespace uvfs
