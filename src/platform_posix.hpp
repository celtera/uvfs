#pragma once
// POSIX backend, with per-system fast paths. Included by platform.hpp.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/sendfile.h>
#include <sys/syscall.h>
#endif

namespace uvfs::platform
{

// --------------------------------------------------------------- capabilities
#if defined(__EMSCRIPTEN__)
//! Emscripten's mmap does not write a MAP_SHARED mapping back to the file, so
//! anything built through a mapping would be silently lost. The writer stages
//! into memory instead and writes the result out.
inline constexpr bool has_shared_writable_mapping = false;
inline constexpr const char* backend_name = "emscripten";
#elif defined(__linux__)
inline constexpr bool has_shared_writable_mapping = true;
inline constexpr const char* backend_name = "linux";
#elif defined(__APPLE__)
inline constexpr bool has_shared_writable_mapping = true;
inline constexpr const char* backend_name = "macos";
#elif defined(__FreeBSD__)
inline constexpr bool has_shared_writable_mapping = true;
inline constexpr const char* backend_name = "freebsd";
#else
inline constexpr bool has_shared_writable_mapping = true;
inline constexpr const char* backend_name = "posix";
#endif

// copy_file_range moves bytes between two files without them entering user
// space, and on filesystems that support reflinks it does not copy at all.
// Linux has had it since 4.5, FreeBSD since 13.
#if defined(__linux__) || (defined(__FreeBSD__) && __FreeBSD__ >= 13)
inline constexpr bool has_kernel_copy = true;
#else
inline constexpr bool has_kernel_copy = false;
#endif

// ---------------------------------------------------------------------- errors
namespace detail
{
// strerror_r comes in two incompatible shapes. The XSI one returns int and
// fills the caller's buffer; the GNU one returns char*, which may or may not
// point at that buffer. Which one is declared depends on the libc and on
// feature-test macros, so picking with #ifdef gets it wrong on some
// combination. Overload resolution just asks the compiler which one it has.
[[nodiscard]] inline auto from_strerror_r(int rc, const char* buf) -> std::string
{
  return rc == 0 ? std::string{buf} : std::string{};
}
[[nodiscard]] inline auto from_strerror_r(const char* msg, const char*) -> std::string
{
  return msg ? std::string{msg} : std::string{};
}
} // namespace detail

[[nodiscard]] inline auto error_text(int code) -> std::string
{
  char buf[256] = {};
  auto msg = detail::from_strerror_r(::strerror_r(code, buf, sizeof buf), buf);
  if (msg.empty())
    msg = "errno " + std::to_string(code);
  return msg;
}

[[nodiscard]] inline auto last_error() -> std::string
{
  return error_text(errno);
}

[[nodiscard]] inline auto page_size() -> int64_t
{
  return static_cast<int64_t>(::sysconf(_SC_PAGESIZE));
}

[[nodiscard]] inline auto process_id() -> uint64_t
{
  return static_cast<uint64_t>(::getpid());
}

// ------------------------------------------------------------------------ file
class file
{
public:
  using native_handle = int;

  static auto open_read(const char* path) -> file
  {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
      throw std::runtime_error("uvfs: could not open (" + last_error() + "): ");
    file f;
    f.fd_ = fd;
#if defined(__APPLE__)
    // Ask the kernel to read ahead: uvfs walks archives forwards far more
    // often than it seeks about in them.
    ::fcntl(fd, F_RDAHEAD, 1);
#endif
    return f;
  }

  static auto create_write(const char* path) -> file
  {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1)
      throw std::runtime_error("uvfs: could not create (" + last_error() + "): ");
    file f;
    f.fd_ = fd;
    return f;
  }

  file() noexcept = default;
  file(const file&) = delete;
  auto operator=(const file&) -> file& = delete;
  file(file&& o) noexcept
      : fd_{o.fd_}
  {
    o.fd_ = -1;
  }
  auto operator=(file&& o) noexcept -> file&
  {
    if (this != &o)
    {
      close();
      fd_ = o.fd_;
      o.fd_ = -1;
    }
    return *this;
  }
  ~file() { close(); }

  [[nodiscard]] auto valid() const noexcept -> bool { return fd_ != -1; }
  [[nodiscard]] auto native() const noexcept -> native_handle { return fd_; }

  void close() noexcept
  {
    if (fd_ != -1)
      ::close(fd_);
    fd_ = -1;
  }

  [[nodiscard]] auto size() const -> int64_t
  {
    struct stat st
    {
    };
    if (::fstat(fd_, &st) != 0)
      throw std::runtime_error("uvfs: could not stat (" + last_error() + "): ");
    return static_cast<int64_t>(st.st_size);
  }

  void resize(int64_t bytes) const
  {
    if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0)
      throw std::runtime_error("uvfs: could not resize (" + last_error() + "): ");
  }

  //! Reserves blocks so that running out of space is reported here rather than
  //! as a SIGBUS when the mapping is written through: a store to a page the
  //! filesystem cannot back is not something the kernel can turn into an error
  //! return. Returns false where the platform offers no such guarantee.
  auto reserve(int64_t offset, int64_t length) const -> bool
  {
    if (length <= 0)
      return true;
#if defined(__linux__) || defined(__FreeBSD__)
    const int rc = ::posix_fallocate(fd_, offset, length);
    if (rc == 0)
      return true;
    if (rc == ENOSPC || rc == EDQUOT)
      throw out_of_space(
          "uvfs: not enough space for the archive (" + error_text(rc) + "): ");
    return false; // EOPNOTSUPP and friends
#elif defined(__APPLE__)
    // macOS has no posix_fallocate. F_PREALLOCATE is the equivalent, and it
    // only extends: the file still has to be grown separately.
    fstore_t store{};
    store.fst_flags = F_ALLOCATECONTIG;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_offset = 0;
    store.fst_length = offset + length;
    if (::fcntl(fd_, F_PREALLOCATE, &store) == -1)
    {
      // Contiguous allocation failed; any allocation will do.
      store.fst_flags = F_ALLOCATEALL;
      if (::fcntl(fd_, F_PREALLOCATE, &store) == -1)
      {
        if (errno == ENOSPC || errno == EDQUOT)
          throw out_of_space(
              "uvfs: not enough space for the archive (" + last_error() + "): ");
        return false;
      }
    }
    return true;
#else
    (void)offset;
    return false;
#endif
  }

  //! Makes previously written bytes durable.
  void flush() const
  {
#if defined(__APPLE__)
    // fsync on macOS only hands the data to the drive; F_FULLFSYNC is what
    // actually makes it survive power loss. Fall back if the filesystem says
    // it does not support it.
    if (::fcntl(fd_, F_FULLFSYNC) != -1)
      return;
#endif
    if (::fsync(fd_) != 0)
      throw std::runtime_error("uvfs: could not flush (" + last_error() + "): ");
  }

  //! Positioned read. Returns bytes read; 0 means end of file.
  [[nodiscard]] auto read_at(void* dst, int64_t n, int64_t offset) const -> int64_t
  {
    auto* out = static_cast<char*>(dst);
    int64_t done = 0;
    while (done < n)
    {
      const auto got = ::pread(
          fd_,
          out + done,
          static_cast<size_t>(n - done),
          static_cast<off_t>(offset + done));
      if (got < 0)
      {
        if (errno == EINTR)
          continue;
        throw std::runtime_error("uvfs: read failed (" + last_error() + "): ");
      }
      if (got == 0)
        break;
      done += got;
    }
    return done;
  }

  //! Positioned write. Writes all `n` bytes or throws.
  void write_at(const void* src, int64_t n, int64_t offset) const
  {
    const auto* in = static_cast<const char*>(src);
    int64_t done = 0;
    while (done < n)
    {
      const auto put = ::pwrite(
          fd_,
          in + done,
          static_cast<size_t>(n - done),
          static_cast<off_t>(offset + done));
      if (put < 0)
      {
        if (errno == EINTR)
          continue;
        if (errno == ENOSPC || errno == EDQUOT)
          throw out_of_space(
              "uvfs: not enough space for the archive (" + last_error() + "): ");
        throw std::runtime_error("uvfs: write failed (" + last_error() + "): ");
      }
      done += put;
    }
  }

private:
  int fd_{-1};
};

// --------------------------------------------------------------------- mapping
class mapping
{
public:
  mapping() noexcept = default;
  mapping(const mapping&) = delete;
  auto operator=(const mapping&) -> mapping& = delete;
  mapping(mapping&& o) noexcept
      : base_{o.base_}
      , size_{o.size_}
      , writable_{o.writable_}
  {
    o.base_ = nullptr;
    o.size_ = 0;
  }
  auto operator=(mapping&& o) noexcept -> mapping&
  {
    if (this != &o)
    {
      reset();
      base_ = o.base_;
      size_ = o.size_;
      writable_ = o.writable_;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~mapping() { reset(); }

  static auto read_only(const file& f, int64_t length) -> mapping
  {
    mapping m;
    if (length == 0)
      return m;
    void* p = ::mmap(
        nullptr, static_cast<size_t>(length), PROT_READ, MAP_PRIVATE, f.native(), 0);
    if (p == MAP_FAILED)
      throw std::runtime_error("uvfs: could not map (" + last_error() + "): ");
    m.base_ = static_cast<char*>(p);
    m.size_ = length;
    m.writable_ = false;
    return m;
  }

  static auto read_write(const file& f, int64_t length) -> mapping
  {
    mapping m;
    if (length == 0)
      return m;
    void* p = ::mmap(
        nullptr,
        static_cast<size_t>(length),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        f.native(),
        0);
    if (p == MAP_FAILED)
      throw std::runtime_error("uvfs: could not map (" + last_error() + "): ");
    m.base_ = static_cast<char*>(p);
    m.size_ = length;
    m.writable_ = true;
    return m;
  }

  [[nodiscard]] auto data() noexcept -> char* { return base_; }
  [[nodiscard]] auto data() const noexcept -> const char* { return base_; }
  [[nodiscard]] auto size() const noexcept -> int64_t { return size_; }

  //! Pushes this mapping's dirty pages, rather than every dirty page on the
  //! machine, which is what a bare sync() would do.
  void flush(int64_t length) const
  {
    if (!base_ || length <= 0 || !writable_)
      return;
    if (::msync(base_, static_cast<size_t>(length), MS_SYNC) != 0)
      throw std::runtime_error("uvfs: could not flush mapping (" + last_error() + "): ");
  }

  //! Hints. Advisory everywhere: a platform that ignores them is still correct.
  void advise_random() const noexcept
  {
#if defined(MADV_RANDOM)
    if (base_)
      ::madvise(base_, static_cast<size_t>(size_), MADV_RANDOM);
#endif
  }
  void advise_sequential() const noexcept
  {
#if defined(MADV_SEQUENTIAL)
    if (base_)
      ::madvise(base_, static_cast<size_t>(size_), MADV_SEQUENTIAL);
#endif
  }

  void reset() noexcept
  {
    if (base_)
      ::munmap(base_, static_cast<size_t>(size_));
    base_ = nullptr;
    size_ = 0;
  }

private:
  char* base_{};
  int64_t size_{};
  bool writable_{};
};

// ------------------------------------------------------------------ copy path
//! Copies `n` bytes from the start of `src` into `dst` at `dst_offset`.
//! `dst_mapped` points at that offset within a writable mapping of `dst`, or
//! is null when the caller has no mapping.
//!
//! Small files go through pread: mmap-per-file costs two syscalls plus a TLB
//! shootdown broadcast to every core, which with many writer threads was the
//! most expensive thing the writer did. Large files are handed to the kernel
//! where that is possible.
inline void copy_file_into(
    const file& src,
    const file& dst,
    int64_t dst_offset,
    char* dst_mapped,
    int64_t n)
{
  if (n <= 0)
    return;

  // Below this the extra syscall costs more than the copy saves.
  constexpr int64_t kernel_copy_threshold = int64_t{256} * 1024;

  if constexpr (has_kernel_copy)
  {
    if (n >= kernel_copy_threshold)
    {
      int64_t done = 0;
      while (done < n)
      {
        off_t in_off = done;
        off_t out_off = dst_offset + done;
#if defined(__linux__)
        const auto moved = ::copy_file_range(
            src.native(),
            &in_off,
            dst.native(),
            &out_off,
            static_cast<size_t>(n - done),
            0);
#else
        const auto moved = ::copy_file_range(
            src.native(),
            &in_off,
            dst.native(),
            &out_off,
            static_cast<size_t>(n - done),
            0);
#endif
        if (moved > 0)
        {
          done += moved;
          continue;
        }
        if (moved < 0 && errno == EINTR)
          continue;
        break; // not supported here (EXDEV, EINVAL, ...); finish by hand
      }
      if (done == n)
        return;
      // Partially copied by the kernel: finish the rest ourselves. The source
      // offset stays absolute while the destination is addressed through the
      // mapping when there is one.
      if (dst_mapped)
      {
        const auto got = src.read_at(dst_mapped + done, n - done, done);
        if (got != n - done)
          throw std::runtime_error("uvfs: file ended early: ");
        return;
      }
      std::vector<char> buf(static_cast<std::size_t>(n - done));
      const auto got = src.read_at(buf.data(), n - done, done);
      if (got != n - done)
        throw std::runtime_error("uvfs: file ended early: ");
      dst.write_at(buf.data(), got, dst_offset + done);
      return;
    }
  }

  if (dst_mapped)
  {
    const auto got = src.read_at(dst_mapped, n, 0);
    if (got != n)
      throw std::runtime_error("uvfs: file ended early: ");
    return;
  }

  // No mapping to read into: stage through a bounded buffer.
  constexpr int64_t chunk = int64_t{1} << 20;
  std::vector<char> buf(static_cast<std::size_t>(std::min(chunk, n)));
  int64_t done = 0;
  while (done < n)
  {
    const auto want = std::min<int64_t>(std::ssize(buf), n - done);
    const auto got = src.read_at(buf.data(), want, done);
    if (got != want)
      throw std::runtime_error("uvfs: file ended early: ");
    dst.write_at(buf.data(), got, dst_offset + done);
    done += got;
  }
}

// ------------------------------------------------------------- filesystem ops
[[nodiscard]] inline auto stat_path(const char* path) -> file_status
{
  file_status s;
  struct stat st
  {
  };
  if (::stat(path, &st) != 0)
    return s;
  s.exists = true;
  s.regular = S_ISREG(st.st_mode);
  s.size = static_cast<int64_t>(st.st_size);
  s.device = static_cast<uint64_t>(st.st_dev);
  s.inode = static_cast<uint64_t>(st.st_ino);
  return s;
}

[[nodiscard]] inline auto readable(const char* path) -> bool
{
  return ::access(path, R_OK) == 0;
}

//! Replaces `to` with `from` in one step, so a reader either sees the old file
//! or the new one and never a partial write.
[[nodiscard]] inline auto rename_replace(const char* from, const char* to) -> bool
{
  return ::rename(from, to) == 0;
}

inline void remove_quietly(const char* path) noexcept
{
  ::unlink(path);
}

//! Drops a file from the page cache. Used by benchmarks to measure cold reads.
inline void evict_from_cache(const char* path) noexcept
{
#if defined(POSIX_FADV_DONTNEED)
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0)
    return;
  ::fsync(fd);
  ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  ::close(fd);
#elif defined(__APPLE__)
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0)
    return;
  ::fcntl(fd, F_NOCACHE, 1);
  ::close(fd);
#else
  (void)path;
#endif
}

} // namespace uvfs::platform
