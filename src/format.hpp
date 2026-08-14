#pragma once
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace uvfs
{

// Header:
// 0:  UVFS\0\0\0\1        // 4 byte magic, 3 reserved, 1 version
// 8:  [ i64 total file size ]
// 16: [ i64 file count ]
// 24: [ i64 index start ] [ i64 index size ]
// 40: [ i64 data start  ] [ i64 data size ]
// 56: padding
// 64: <index start>

// Index:
// [ [ i64 data start  ]   // relative to h->data_start
//   [ i64 data length ]
//   [ i32 path length ]   // at least 1
//   [ utf-8 path ]
// ] *, each entry aligned to 8 bytes

// File: [ data ] *, aligned to 64 bytes

static constexpr const char magic[4] = {'U', 'V', 'F', 'S'};
static constexpr uint8_t format_version = 1;
static constexpr const char ident[8]
    = {'U', 'V', 'F', 'S', 0, 0, 0, static_cast<char>(format_version)};

// Payloads are aligned so they can be handed straight to code with alignment
// requirements: SIMD loads, DMA, audio buffers.
static constexpr int64_t payload_alignment = 64;

[[nodiscard]]
static constexpr auto round_up_8(int64_t x) noexcept -> int64_t
{
  return ((static_cast<uint64_t>(x) + 7) >> 3) << 3;
}

[[nodiscard]]
static constexpr auto round_up_64(int64_t x) noexcept -> int64_t
{
  return ((static_cast<uint64_t>(x) + 63) >> 6) << 6;
}

template<typename T>
static constexpr int64_t ssizeof = static_cast<int64_t>(sizeof(T));

// Reads a scalar out of the mapping without assuming anything about the
// alignment of the source. A corrupt or hostile header can point the index at
// an odd offset, and forming a misaligned `entry*` there is undefined
// behaviour even on architectures where the load itself would have worked.
// Compilers lower this to a plain load when the target allows it.
template<typename T>
[[nodiscard]] inline auto load(const char* p) noexcept -> T
{
  static_assert(std::is_trivially_copyable_v<T>);
  T v{};
  memcpy(&v, p, sizeof(T));
  return v;
}

template<typename T>
inline void store(char* p, T v) noexcept
{
  static_assert(std::is_trivially_copyable_v<T>);
  memcpy(p, &v, sizeof(T));
}

struct entry
{
  static constexpr int64_t static_size = 8 + 8 + 4;

  int64_t data_start{};
  int64_t data_size{};
  int32_t path_len{};

  [[nodiscard]] static auto load_from(const char* p) noexcept -> entry
  {
    return entry{
        .data_start = uvfs::load<int64_t>(p),
        .data_size = uvfs::load<int64_t>(p + 8),
        .path_len = uvfs::load<int32_t>(p + 16)};
  }

  void store_to(char* p) const noexcept
  {
    uvfs::store<int64_t>(p, data_start);
    uvfs::store<int64_t>(p + 8, data_size);
    uvfs::store<int32_t>(p + 16, path_len);
  }

  [[nodiscard]] static auto path_of(const char* p) noexcept -> const char*
  {
    return p + static_size;
  }
  [[nodiscard]] static auto path_of(char* p) noexcept -> char*
  {
    return p + static_size;
  }
};

struct header
{
  char head[8]{};
  int64_t file_size{};
  int64_t file_count{};
  int64_t index_start{};
  int64_t index_size{};
  int64_t data_start{};
  int64_t data_size{};
  char padding[8]{};

  [[nodiscard]] static auto load_from(const char* p) noexcept -> header
  {
    header h;
    memcpy(h.head, p, 8);
    h.file_size = uvfs::load<int64_t>(p + 8);
    h.file_count = uvfs::load<int64_t>(p + 16);
    h.index_start = uvfs::load<int64_t>(p + 24);
    h.index_size = uvfs::load<int64_t>(p + 32);
    h.data_start = uvfs::load<int64_t>(p + 40);
    h.data_size = uvfs::load<int64_t>(p + 48);
    return h;
  }

  void store_to(char* p) const noexcept
  {
    memcpy(p, head, 8);
    uvfs::store<int64_t>(p + 8, file_size);
    uvfs::store<int64_t>(p + 16, file_count);
    uvfs::store<int64_t>(p + 24, index_start);
    uvfs::store<int64_t>(p + 32, index_size);
    uvfs::store<int64_t>(p + 40, data_start);
    uvfs::store<int64_t>(p + 48, data_size);
    memset(p + 56, 0, 8);
  }

  [[nodiscard]] auto version() const noexcept -> uint8_t
  {
    return static_cast<uint8_t>(head[7]);
  }

  // Checks the header against the real size of the file on disk. Every
  // comparison is done in unsigned arithmetic against a remaining-space budget
  // rather than by adding two fields together: `a + b > limit` overflows for
  // hostile or corrupt values, and signed overflow is undefined behaviour, so
  // the check itself would be the bug. `a > limit - b` cannot overflow once
  // `b <= limit` is known.
  void validate(int64_t filesize) const
  {
    if (memcmp(magic, this->head, sizeof(magic)) != 0)
      throw std::runtime_error("uvfs: not a uvfs archive: ");

    if (version() != format_version)
      throw std::runtime_error(
          "uvfs: unsupported format version " + std::to_string(version())
          + " (this build reads version " + std::to_string(format_version)
          + "): ");

    if (filesize < ssizeof<header>)
      throw std::runtime_error("uvfs: file is smaller than a header: ");

    if (file_size != filesize)
      throw std::runtime_error(
          "uvfs: header says " + std::to_string(file_size) + " bytes but file is "
          + std::to_string(filesize) + " (truncated or appended to?): ");

    if (file_count < 0 || index_start < 0 || index_size < 0 || data_start < 0
        || data_size < 0)
      throw std::runtime_error("uvfs: negative field in header: ");

    const auto total = static_cast<uint64_t>(filesize);
    const auto idx_at = static_cast<uint64_t>(index_start);
    const auto idx_sz = static_cast<uint64_t>(index_size);
    const auto dat_at = static_cast<uint64_t>(data_start);
    const auto dat_sz = static_cast<uint64_t>(data_size);

    if (idx_at < static_cast<uint64_t>(ssizeof<header>) || idx_at > total)
      throw std::runtime_error("uvfs: index starts outside the file: ");
    if (idx_sz > total - idx_at)
      throw std::runtime_error("uvfs: index extends past the end of the file: ");

    if (dat_at < idx_at + idx_sz)
      throw std::runtime_error("uvfs: data region overlaps the index: ");
    if (dat_at > total)
      throw std::runtime_error("uvfs: data starts outside the file: ");
    if (dat_sz > total - dat_at)
      throw std::runtime_error("uvfs: data extends past the end of the file: ");

    // The alignment guarantee is part of the format, so a reader must enforce
    // it rather than assume it: callers are entitled to rely on the pointers
    // they get back being suitably aligned.
    if (data_start % payload_alignment != 0)
      throw std::runtime_error("uvfs: data region is not 64-byte aligned: ");

    // Each entry needs at least its fixed-size part plus one path byte.
    if (static_cast<uint64_t>(file_count) > idx_sz / (entry::static_size + 1))
      throw std::runtime_error(
          "uvfs: index is too small to hold " + std::to_string(file_count)
          + " entries: ");
  }
};
}
