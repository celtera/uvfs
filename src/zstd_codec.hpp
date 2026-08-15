#pragma once
#include "format.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(UVFS_HAS_ZSTD)
#include <zstd.h>
#endif

namespace uvfs
{

[[nodiscard]] inline auto zstd_available() noexcept -> bool
{
#if defined(UVFS_HAS_ZSTD)
  return true;
#else
  return false;
#endif
}

[[nodiscard]] inline auto no_zstd_error(std::string_view what) -> std::runtime_error
{
  return std::runtime_error(
      "uvfs: " + std::string{what}
      + " requires zstd, which this build was compiled without");
}

#if defined(UVFS_HAS_ZSTD)

//! Upper bound on the compressed size of `n` bytes.
[[nodiscard]] inline auto zstd_bound(int64_t n) -> int64_t
{
  return static_cast<int64_t>(ZSTD_compressBound(static_cast<size_t>(n)));
}

//! A reusable compression context. zstd allocates real state per context, so
//! one per worker thread is reused across every file that worker handles
//! rather than being rebuilt for each one.
struct zstd_compressor
{
  zstd_compressor()
      : ctx{ZSTD_createCCtx(), &ZSTD_freeCCtx}
  {
    if (!ctx)
      throw std::runtime_error("uvfs: could not create a zstd context");
  }

  //! Compresses into `dst`, returning the compressed size, or 0 if zstd
  //! refused (which callers treat as "store it verbatim").
  [[nodiscard]] auto compress(
      char* dst,
      int64_t dst_capacity,
      const char* src,
      int64_t src_size,
      int level,
      const ZSTD_CDict* dict) const -> int64_t
  {
    size_t n = 0;
    if (dict)
      n = ZSTD_compress_usingCDict(
          ctx.get(),
          dst,
          static_cast<size_t>(dst_capacity),
          src,
          static_cast<size_t>(src_size),
          dict);
    else
      n = ZSTD_compressCCtx(
          ctx.get(),
          dst,
          static_cast<size_t>(dst_capacity),
          src,
          static_cast<size_t>(src_size),
          level);
    if (ZSTD_isError(n))
      return 0;
    return static_cast<int64_t>(n);
  }

  std::unique_ptr<ZSTD_CCtx, size_t (*)(ZSTD_CCtx*)> ctx;
};

struct zstd_decompressor
{
  zstd_decompressor()
      : ctx{ZSTD_createDCtx(), &ZSTD_freeDCtx}
  {
    if (!ctx)
      throw std::runtime_error("uvfs: could not create a zstd context");
  }

  void decompress(
      char* dst,
      int64_t dst_capacity,
      const char* src,
      int64_t src_size,
      const ZSTD_DDict* dict,
      std::string_view what) const
  {
    size_t n = 0;
    if (dict)
      n = ZSTD_decompress_usingDDict(
          ctx.get(),
          dst,
          static_cast<size_t>(dst_capacity),
          src,
          static_cast<size_t>(src_size),
          dict);
    else
      n = ZSTD_decompressDCtx(
          ctx.get(),
          dst,
          static_cast<size_t>(dst_capacity),
          src,
          static_cast<size_t>(src_size));

    if (ZSTD_isError(n))
      throw std::runtime_error(
          "uvfs: could not decompress " + std::string{what} + ": "
          + ZSTD_getErrorName(n));
    if (static_cast<int64_t>(n) != dst_capacity)
      throw std::runtime_error(
          "uvfs: " + std::string{what} + " decompressed to " + std::to_string(n)
          + " bytes but the index says " + std::to_string(dst_capacity));
  }

  std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> ctx;
};

#endif // UVFS_HAS_ZSTD

} // namespace uvfs
