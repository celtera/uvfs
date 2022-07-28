#pragma once

#include <string_view>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uvfs
{


template<typename T>
struct mmap_handle
{
  T bytes{};
  int64_t sz{};

  mmap_handle(T b, int64_t sz): bytes{b}, sz{sz} { }
  mmap_handle() = default;
  mmap_handle(const mmap_handle&) = delete;
  auto operator=(const mmap_handle&) -> mmap_handle& = delete;
  mmap_handle(mmap_handle&& other) noexcept
      : bytes{other.bytes}
      , sz{other.sz}
  {
    other.bytes = nullptr;
  }
  auto operator=(mmap_handle&& other) noexcept -> mmap_handle&
  {
    bytes = other.bytes;
    sz = other.sz;
    other.bytes = nullptr;
    return *this;
  }
  ~mmap_handle() { if(bytes) munmap((void*)(bytes), sz); }
};

struct fd_handle
{
public:
  static auto open_ro(std::string_view path)
  {
    const int handle = open(path.data(), O_RDONLY);
    return fd_handle{handle};
  }

  static auto create_rw(std::string_view path, int mode)
  {
    const int handle = open(path.data(), O_RDWR | O_CREAT | O_TRUNC, mode);
    return fd_handle{handle};
  }

  [[nodiscard]]
  auto filesize() const -> int64_t
  {
    struct stat st{};
    if (fstat(handle, &st))
      throw std::runtime_error("uvfs: could not stat: ");
    return st.st_size;
  }

  void resize(int64_t size) const
  {
    if (ftruncate(handle, size))
      throw std::runtime_error("uvfs: writer: could not resize: ");
  }

  [[nodiscard]]
  auto map_ro(int64_t sz) const
  {
    const auto* data
        = static_cast<const char*>(mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, handle, 0));
    if (data == MAP_FAILED)
      throw std::runtime_error("uvfs: could not map: ");

    return mmap_handle<const char*>{data, sz};
  }

  [[nodiscard]]
  auto map_rw(int64_t sz) const
  {
    auto* data = static_cast<char*>(
        mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, handle, 0));
    if (data == MAP_FAILED)
      throw std::runtime_error("uvfs: could not map: ");

    return mmap_handle<char*>{data, sz};
  }

  ~fd_handle()
  {
    if (handle != -1)
      close(handle);
  }

private:
  int handle{-1};

  explicit fd_handle(int fd)
      : handle{fd}
  {
    if (fd == -1)
      throw std::runtime_error("uvfs: could not open: ");
  }

  fd_handle() noexcept = default;
  fd_handle(const fd_handle&) = delete;
  fd_handle(fd_handle&& other) noexcept
      : handle{other.handle}
  {
    other.handle = -1;
  }
  auto operator=(const fd_handle&) -> fd_handle& = delete;
  auto operator=(fd_handle&& other) noexcept -> fd_handle&
  {
    this->handle = other.handle;
    other.handle = -1;
    return *this;
  }
};

}
