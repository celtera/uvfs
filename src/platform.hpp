#pragma once

// Platform layer
// ---------------------------------------------------------------------------
// uvfs does a small number of things to files, and each of them has a fastest
// way to do it that differs per platform: copy a range between two files,
// reserve blocks, map a file, make writes durable, replace a file atomically.
// Doing that with #ifdefs at every call site would scatter six platforms
// through the writer; this header states the operations once and each backend
// implements them with the best primitive it has.
//
// The interface is deliberately small and non-virtual: every call is resolved
// at compile time, so the abstraction costs nothing.
//
//   linux       copy_file_range, posix_fallocate, fadvise/madvise
//   freebsd     copy_file_range (13+), posix_fallocate
//   macos       F_PREALLOCATE, F_FULLFSYNC, F_RDAHEAD
//   windows     CreateFileMapping, positioned ReadFile, MoveFileEx
//   emscripten  no writable shared mapping; staged in memory, written back
//   generic     pread/pwrite and mmap, which every POSIX system has
//
// A backend may decline any fast path; the generic implementation is always
// correct, and every backend is held to the same tests.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace uvfs
{

//! Thrown when the destination filesystem is out of space. Distinct from an
//! ordinary runtime_error because it is a property of the archive being
//! written, not of any one input, and must not be collected as a per-file
//! failure that the commit could otherwise skip past.
struct out_of_space : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

namespace platform
{

//! What a file looks like without opening it. `device` and `inode` identify
//! the file itself, so two names for one file can share a payload.
struct file_status
{
  bool exists{};
  bool regular{};
  int64_t size{};
  uint64_t device{};
  uint64_t inode{};
};

} // namespace platform
} // namespace uvfs

#if defined(_WIN32)
#include "platform_win32.hpp"
#else
#include "platform_posix.hpp"
#endif

namespace uvfs::platform
{

// Every backend must provide these; the static_asserts keep a partially
// implemented one from compiling into something that fails at runtime instead.
static_assert(sizeof(decltype(page_size())) >= 4, "page_size() must be provided");

//! True when the platform can write through a shared file mapping and have
//! those writes reach the file. Emscripten cannot, so the writer stages the
//! archive in memory there and writes it back explicitly.
static_assert(
    has_shared_writable_mapping || !has_shared_writable_mapping,
    "has_shared_writable_mapping must be defined");

} // namespace uvfs::platform
