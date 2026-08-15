#pragma once

// XXH3 for both the lookup table and integrity checking. The threat model is
// corruption, not an adversary, so a non-cryptographic hash is the right tool.
// XXH_INLINE_ALL lets the small key hashes inline at the call site.

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>

#include <string_view>

namespace uvfs
{

[[nodiscard]] inline auto hash_bytes(const void* p, std::size_t n) noexcept -> uint64_t
{
  return XXH3_64bits(p, n);
}

[[nodiscard]] inline auto hash_name(std::string_view s) noexcept -> uint64_t
{
  return XXH3_64bits(s.data(), s.size());
}

} // namespace uvfs
