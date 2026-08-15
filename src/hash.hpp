#pragma once

// XXH3 is used for two different jobs, both of which want the same thing: the
// fastest 64-bit hash available, with no cryptographic properties required.
//
//   - the in-file lookup table, where hashing the key is on the critical path
//     of every find();
//   - integrity checking, where the input is the whole index or a whole
//     payload and throughput is what matters.
//
// The threat model is corruption -- bad disks, truncated downloads, half-
// written files -- not an adversary choosing inputs, so a non-cryptographic
// hash is the right tool. XXH3 reaches tens of GB/s with SIMD, well past what
// hardware CRC32C manages, and it is already the hash zstd carries.
//
// XXH_INLINE_ALL puts the implementation in this translation unit so the small
// key hashes inline at the call site rather than going through a call.

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
