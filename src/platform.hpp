#pragma once

// File operations, implemented per platform with the fastest primitive each
// one offers. Everything here is resolved at compile time.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace uvfs
{

//! Out of space on the destination. A separate type because it concerns the
//! archive, not one input, so it must not be collected as a skippable
//! per-file failure.
struct out_of_space : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

namespace platform
{

//! `device` and `inode` identify the file itself, so two names for one file
//! can share a payload.
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

// Catch a partially implemented backend at compile time.
static_assert(sizeof(decltype(page_size())) >= 4, "page_size() must be provided");
static_assert(
    has_shared_writable_mapping || !has_shared_writable_mapping,
    "has_shared_writable_mapping must be defined");

} // namespace uvfs::platform
