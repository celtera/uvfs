#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uvfs
{

[[nodiscard]] inline auto errno_string(int e) -> std::string
{
  char buf[256] = {};
  // GNU strerror_r may return a pointer to its own buffer rather than filling
  // the one it was given.
  const char* msg = strerror_r(e, buf, sizeof buf);
  return msg ? std::string(msg) : std::string("errno ") + std::to_string(e);
}

template<typename T>
struct mmap_handle
{
  T bytes{};
  int64_t sz{};

  mmap_handle(T b, int64_t size): bytes{b}, sz{size} { }
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
    if (this != &other)
    {
      reset();
      bytes = other.bytes;
      sz = other.sz;
      other.bytes = nullptr;
    }
    return *this;
  }
  void reset() noexcept
  {
    if (bytes)
      munmap(const_cast<void*>(static_cast<const void*>(bytes)), sz);
    bytes = nullptr;
  }
  ~mmap_handle() { reset(); }
};

struct fd_handle
{
public:
  // These take a `const char*` rather than a `std::string_view` on purpose.
  // A string_view carries no terminator, so passing `.data()` to a C API reads
  // until it happens to find a zero byte -- which silently opened the wrong
  // file when the view was a prefix of a longer buffer.
  static auto open_ro(const char* path) -> fd_handle
  {
    const int handle = open(path, O_RDONLY | O_CLOEXEC);
    if (handle == -1)
      throw std::runtime_error(
          "uvfs: could not open (" + errno_string(errno) + "): ");
    return fd_handle{handle};
  }

  static auto create_rw(const char* path, int mode) -> fd_handle
  {
    const int handle = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (handle == -1)
      throw std::runtime_error(
          "uvfs: could not create (" + errno_string(errno) + "): ");
    return fd_handle{handle};
  }

  [[nodiscard]] auto get() const noexcept -> int { return handle; }

  [[nodiscard]]
  auto filesize() const -> int64_t
  {
    struct stat st{};
    if (fstat(handle, &st))
      throw std::runtime_error(
          "uvfs: could not stat (" + errno_string(errno) + "): ");
    return st.st_size;
  }

  void resize(int64_t size) const
  {
    if (ftruncate(handle, size))
      throw std::runtime_error(
          "uvfs: could not resize (" + errno_string(errno) + "): ");
  }

  // Reserves blocks up front so that running out of space is reported here, as
  // an error, rather than as a SIGBUS when a dirty page of the mapping is
  // written back. Not all filesystems implement it; a refusal is not fatal.
  void reserve_space(int64_t size) const
  {
#if defined(__linux__)
    if (size > 0)
      (void)posix_fallocate(handle, 0, size);
#else
    (void)size;
#endif
  }

  void sync() const
  {
    if (fsync(handle) != 0)
      throw std::runtime_error(
          "uvfs: could not flush (" + errno_string(errno) + "): ");
  }

  [[nodiscard]]
  auto map_ro(int64_t sz) const
  {
    if (sz == 0)
      return mmap_handle<const char*>{nullptr, 0};
    const auto* data
        = static_cast<const char*>(mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, handle, 0));
    if (data == MAP_FAILED)
      throw std::runtime_error(
          "uvfs: could not map (" + errno_string(errno) + "): ");

    return mmap_handle<const char*>{data, sz};
  }

  [[nodiscard]]
  auto map_rw(int64_t sz) const
  {
    if (sz == 0)
      return mmap_handle<char*>{nullptr, 0};
    auto* data = static_cast<char*>(
        mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, handle, 0));
    if (data == MAP_FAILED)
      throw std::runtime_error(
          "uvfs: could not map (" + errno_string(errno) + "): ");

    return mmap_handle<char*>{data, sz};
  }

  fd_handle() noexcept = default;
  fd_handle(const fd_handle&) = delete;
  auto operator=(const fd_handle&) -> fd_handle& = delete;
  fd_handle(fd_handle&& other) noexcept
      : handle{other.handle}
  {
    other.handle = -1;
  }
  auto operator=(fd_handle&& other) noexcept -> fd_handle&
  {
    if (this != &other)
    {
      close_now();
      this->handle = other.handle;
      other.handle = -1;
    }
    return *this;
  }

  void close_now() noexcept
  {
    if (handle != -1)
      close(handle);
    handle = -1;
  }

  ~fd_handle() { close_now(); }

private:
  int handle{-1};

  explicit fd_handle(int fd)
      : handle{fd}
  {
  }
};

}
