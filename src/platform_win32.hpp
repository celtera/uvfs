#pragma once
// Windows backend. Included by platform.hpp.
//
// Win32 has an equivalent for every primitive uvfs needs, but none of them are
// spelled like their POSIX counterparts: file mappings are objects rather than
// a call, positioned reads go through OVERLAPPED rather than pread, and
// replacing a file atomically is MoveFileEx rather than rename.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace uvfs::platform
{

inline constexpr bool has_shared_writable_mapping = true;
//! Win32 has no equivalent of copy_file_range that writes into the middle of
//! an existing file; CopyFileEx only produces whole files.
inline constexpr bool has_kernel_copy = false;
inline constexpr const char* backend_name = "windows";

// ---------------------------------------------------------------------- errors
[[nodiscard]] inline auto error_text(unsigned long code) -> std::string
{
  if (code == 0)
    return "no error";
  LPSTR buffer = nullptr;
  const DWORD n = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
          | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&buffer),
      0,
      nullptr);
  std::string msg = (n && buffer) ? std::string{buffer, n} : std::string{};
  if (buffer)
    ::LocalFree(buffer);
  // FormatMessage ends its strings with CRLF, which reads badly inside a
  // sentence.
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
    msg.pop_back();
  if (msg.empty())
    msg = "error " + std::to_string(code);
  return msg;
}

[[nodiscard]] inline auto last_error() -> std::string
{
  return error_text(::GetLastError());
}

[[nodiscard]] inline auto page_size() -> int64_t
{
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  return static_cast<int64_t>(si.dwPageSize);
}

[[nodiscard]] inline auto process_id() -> uint64_t
{
  return static_cast<uint64_t>(::GetCurrentProcessId());
}

namespace detail
{
//! Win32's wide-character API is the one without path length and encoding
//! limits, so paths are converted rather than passed to the ANSI entry points.
[[nodiscard]] inline auto widen(const char* utf8) -> std::wstring
{
  if (!utf8 || !*utf8)
    return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (n <= 0)
    return {};
  std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
  return w;
}

[[nodiscard]] inline auto is_out_of_space(DWORD code) noexcept -> bool
{
  return code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL;
}
} // namespace detail

// ------------------------------------------------------------------------ file
class file
{
public:
  using native_handle = HANDLE;

  static auto open_read(const char* path) -> file
  {
    const auto w = detail::widen(path);
    HANDLE h = ::CreateFileW(
        w.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
      throw std::runtime_error("uvfs: could not open (" + last_error() + "): ");
    file f;
    f.h_ = h;
    return f;
  }

  static auto create_write(const char* path) -> file
  {
    const auto w = detail::widen(path);
    HANDLE h = ::CreateFileW(
        w.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
      throw std::runtime_error("uvfs: could not create (" + last_error() + "): ");
    file f;
    f.h_ = h;
    return f;
  }

  file() noexcept = default;
  file(const file&) = delete;
  auto operator=(const file&) -> file& = delete;
  file(file&& o) noexcept
      : h_{o.h_}
  {
    o.h_ = INVALID_HANDLE_VALUE;
  }
  auto operator=(file&& o) noexcept -> file&
  {
    if (this != &o)
    {
      close();
      h_ = o.h_;
      o.h_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  ~file() { close(); }

  [[nodiscard]] auto valid() const noexcept -> bool
  {
    return h_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] auto native() const noexcept -> native_handle { return h_; }

  void close() noexcept
  {
    if (h_ != INVALID_HANDLE_VALUE)
      ::CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] auto size() const -> int64_t
  {
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h_, &li))
      throw std::runtime_error("uvfs: could not stat (" + last_error() + "): ");
    return static_cast<int64_t>(li.QuadPart);
  }

  void resize(int64_t bytes) const
  {
    LARGE_INTEGER li{};
    li.QuadPart = bytes;
    if (!::SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) || !::SetEndOfFile(h_))
    {
      const auto code = ::GetLastError();
      if (detail::is_out_of_space(code))
        throw out_of_space(
            "uvfs: not enough space for the archive (" + error_text(code) + "): ");
      throw std::runtime_error("uvfs: could not resize (" + error_text(code) + "): ");
    }
  }

  //! Setting the end of file allocates the space on NTFS, so growing the file
  //! is itself the reservation: the blocks exist before anything writes
  //! through the mapping. There is no separate call to make, and no way to
  //! reserve a sub-range, so this only reports whether the space is there.
  auto reserve(int64_t offset, int64_t length) const -> bool
  {
    if (length <= 0)
      return true;
    const int64_t needed = offset + length;
    if (size() >= needed)
      return true;
    resize(needed); // throws out_of_space when the volume is full
    return true;
  }

  void flush() const
  {
    if (!::FlushFileBuffers(h_))
      throw std::runtime_error("uvfs: could not flush (" + last_error() + "): ");
  }

  [[nodiscard]] auto read_at(void* dst, int64_t n, int64_t offset) const -> int64_t
  {
    auto* out = static_cast<char*>(dst);
    int64_t done = 0;
    while (done < n)
    {
      OVERLAPPED ov{};
      const auto at = static_cast<unsigned long long>(offset + done);
      ov.Offset = static_cast<DWORD>(at & 0xffffffffull);
      ov.OffsetHigh = static_cast<DWORD>(at >> 32);
      const DWORD want
          = static_cast<DWORD>(std::min<int64_t>(n - done, int64_t{64} * 1024 * 1024));
      DWORD got = 0;
      if (!::ReadFile(h_, out + done, want, &got, &ov))
      {
        const auto code = ::GetLastError();
        if (code == ERROR_HANDLE_EOF)
          break;
        throw std::runtime_error("uvfs: read failed (" + error_text(code) + "): ");
      }
      if (got == 0)
        break;
      done += got;
    }
    return done;
  }

  void write_at(const void* src, int64_t n, int64_t offset) const
  {
    const auto* in = static_cast<const char*>(src);
    int64_t done = 0;
    while (done < n)
    {
      OVERLAPPED ov{};
      const auto at = static_cast<unsigned long long>(offset + done);
      ov.Offset = static_cast<DWORD>(at & 0xffffffffull);
      ov.OffsetHigh = static_cast<DWORD>(at >> 32);
      const DWORD want
          = static_cast<DWORD>(std::min<int64_t>(n - done, int64_t{64} * 1024 * 1024));
      DWORD put = 0;
      if (!::WriteFile(h_, in + done, want, &put, &ov))
      {
        const auto code = ::GetLastError();
        if (detail::is_out_of_space(code))
          throw out_of_space(
              "uvfs: not enough space for the archive (" + error_text(code) + "): ");
        throw std::runtime_error("uvfs: write failed (" + error_text(code) + "): ");
      }
      if (put == 0)
        throw std::runtime_error("uvfs: write made no progress: ");
      done += put;
    }
  }

private:
  HANDLE h_{INVALID_HANDLE_VALUE};
};

// --------------------------------------------------------------------- mapping
class mapping
{
public:
  mapping() noexcept = default;
  mapping(const mapping&) = delete;
  auto operator=(const mapping&) -> mapping& = delete;
  mapping(mapping&& o) noexcept
      : section_{o.section_}
      , base_{o.base_}
      , size_{o.size_}
      , writable_{o.writable_}
  {
    o.section_ = nullptr;
    o.base_ = nullptr;
    o.size_ = 0;
  }
  auto operator=(mapping&& o) noexcept -> mapping&
  {
    if (this != &o)
    {
      reset();
      section_ = o.section_;
      base_ = o.base_;
      size_ = o.size_;
      writable_ = o.writable_;
      o.section_ = nullptr;
      o.base_ = nullptr;
      o.size_ = 0;
    }
    return *this;
  }
  ~mapping() { reset(); }

  static auto read_only(const file& f, int64_t length) -> mapping
  {
    return create(f, length, false);
  }
  static auto read_write(const file& f, int64_t length) -> mapping
  {
    return create(f, length, true);
  }

  [[nodiscard]] auto data() noexcept -> char* { return base_; }
  [[nodiscard]] auto data() const noexcept -> const char* { return base_; }
  [[nodiscard]] auto size() const noexcept -> int64_t { return size_; }

  void flush(int64_t length) const
  {
    if (!base_ || length <= 0 || !writable_)
      return;
    if (!::FlushViewOfFile(base_, static_cast<SIZE_T>(length)))
      throw std::runtime_error("uvfs: could not flush mapping (" + last_error() + "): ");
  }

  void advise_random() const noexcept { }

  void advise_sequential() const noexcept
  {
    // PrefetchVirtualMemory is the closest equivalent to MADV_WILLNEED, and is
    // only present from Windows 8 onwards; missing is not an error.
    if (!base_)
      return;
    using prefetch_fn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PVOID, ULONG);
    static const auto fn = reinterpret_cast<prefetch_fn>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory")));
    if (!fn)
      return;
    WIN32_MEMORY_RANGE_ENTRY range{};
    range.VirtualAddress = base_;
    range.NumberOfBytes = static_cast<SIZE_T>(size_);
    fn(::GetCurrentProcess(), 1, &range, 0);
  }

  void reset() noexcept
  {
    if (base_)
      ::UnmapViewOfFile(base_);
    if (section_)
      ::CloseHandle(section_);
    base_ = nullptr;
    section_ = nullptr;
    size_ = 0;
  }

private:
  static auto create(const file& f, int64_t length, bool writable) -> mapping
  {
    mapping m;
    if (length == 0)
      return m;

    // A zero size in CreateFileMapping means "as large as the file", which is
    // what we want, but the file must already be that large.
    HANDLE section = ::CreateFileMappingW(
        f.native(),
        nullptr,
        writable ? PAGE_READWRITE : PAGE_READONLY,
        static_cast<DWORD>(static_cast<unsigned long long>(length) >> 32),
        static_cast<DWORD>(static_cast<unsigned long long>(length) & 0xffffffffull),
        nullptr);
    if (!section)
      throw std::runtime_error("uvfs: could not map (" + last_error() + "): ");

    void* p = ::MapViewOfFile(
        section,
        writable ? FILE_MAP_WRITE : FILE_MAP_READ,
        0,
        0,
        static_cast<SIZE_T>(length));
    if (!p)
    {
      const auto code = ::GetLastError();
      ::CloseHandle(section);
      throw std::runtime_error("uvfs: could not map (" + error_text(code) + "): ");
    }
    m.section_ = section;
    m.base_ = static_cast<char*>(p);
    m.size_ = length;
    m.writable_ = writable;
    return m;
  }

  HANDLE section_{};
  char* base_{};
  int64_t size_{};
  bool writable_{};
};

// ------------------------------------------------------------------ copy path
inline void copy_file_into(
    const file& src,
    const file& dst,
    int64_t dst_offset,
    char* dst_mapped,
    int64_t n)
{
  if (n <= 0)
    return;
  if (dst_mapped)
  {
    const auto got = src.read_at(dst_mapped, n, 0);
    if (got != n)
      throw std::runtime_error("uvfs: file ended early: ");
    return;
  }
  constexpr int64_t chunk = int64_t{1} << 20;
  std::vector<char> buf(static_cast<std::size_t>(std::min(chunk, n)));
  int64_t done = 0;
  while (done < n)
  {
    const auto want = std::min<int64_t>(static_cast<int64_t>(buf.size()), n - done);
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
  const auto w = detail::widen(path);
  WIN32_FILE_ATTRIBUTE_DATA attr{};
  if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &attr))
    return s;
  s.exists = true;
  s.regular = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
              && (attr.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0;
  s.size = (static_cast<int64_t>(attr.nFileSizeHigh) << 32)
           | static_cast<int64_t>(attr.nFileSizeLow);

  // The volume serial number and file index together identify the file itself,
  // which is what lets two names for one file share a payload. They are only
  // available through an open handle.
  if (s.regular)
  {
    HANDLE h = ::CreateFileW(
        w.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
      BY_HANDLE_FILE_INFORMATION info{};
      if (::GetFileInformationByHandle(h, &info))
      {
        s.device = info.dwVolumeSerialNumber;
        s.inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32)
                  | static_cast<uint64_t>(info.nFileIndexLow);
      }
      ::CloseHandle(h);
    }
  }
  return s;
}

[[nodiscard]] inline auto readable(const char* path) -> bool
{
  const auto w = detail::widen(path);
  HANDLE h = ::CreateFileW(
      w.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  ::CloseHandle(h);
  return true;
}

[[nodiscard]] inline auto rename_replace(const char* from, const char* to) -> bool
{
  const auto wf = detail::widen(from);
  const auto wt = detail::widen(to);
  // rename() on Windows refuses to overwrite; MoveFileEx with
  // MOVEFILE_REPLACE_EXISTING is the atomic replace POSIX gets from rename.
  return ::MoveFileExW(
             wf.c_str(), wt.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
         != 0;
}

inline void remove_quietly(const char* path) noexcept
{
  const auto w = detail::widen(path);
  ::DeleteFileW(w.c_str());
}

inline void evict_from_cache(const char* path) noexcept
{
  // Reopening with FILE_FLAG_NO_BUFFERING does not purge what is already
  // cached, and there is no unprivileged way to drop a file from the cache.
  (void)path;
}

} // namespace uvfs::platform
