#pragma once
#include <uvfs/path.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <type_traits>

// uvfs format, version 2
//
//   header  | fixed 128 bytes
//   index   | entries, sorted by name, fixed 32-byte stride
//           | hash table, power-of-two, open addressing
//           | per-entry content hashes (optional)
//           | name blob
//   data    | payloads
//
// The index is stored finished and read in place: opening is one mmap and one
// header check, with nothing parsed or allocated. Entries are a fixed stride
// so the array is directly indexable, and sorted by name so index order is
// also extraction order.

namespace uvfs
{

static constexpr const char magic[4] = {'U', 'V', 'F', 'S'};
static constexpr uint8_t format_version = 2;
static constexpr const char ident[8]
    = {'U', 'V', 'F', 'S', 0, 0, 0, static_cast<char>(format_version)};

//! So payloads can be handed to SIMD, DMA or audio code directly.
static constexpr int64_t payload_alignment = 64;
//! Compressed payloads are copied out anyway.
static constexpr int64_t compressed_alignment = 8;

static constexpr int64_t header_size = 128;
static constexpr int64_t entry_size = 32;

//! u32 entry indices in the hash table put a ceiling on the file count.
static constexpr int64_t max_file_count = (int64_t{1} << 31) - 2;

//! Empty hash table slot.
static constexpr uint64_t empty_slot = ~uint64_t{0};

//! Rejects an index claiming a payload expands beyond what a zstd frame can
//! encode. The densest encoding is a run-length block, 3 bytes of header for
//! up to 128 KiB, so the ceiling is 43691:1; 50000 leaves room.
static constexpr int64_t max_zstd_expansion = 50000;

enum class codec : uint8_t
{
  store = 0,    //!< payload is the file, byte for byte
  zstd = 1,     //!< a raw zstd frame
  zstd_dict = 2 //!< a raw zstd frame using the archive's dictionary
};

enum flags : uint32_t
{
  flag_entry_hashes = 1u << 0, //!< per-entry content hashes are present
  flag_has_dictionary = 1u << 1,
  //! Every flag uvfs understands. An archive setting anything else was written
  //! by a newer implementation and cannot be read safely.
  flag_all_known = flag_entry_hashes | flag_has_dictionary,
};

[[nodiscard]]
static constexpr auto round_up(int64_t x, int64_t multiple) noexcept -> int64_t
{
  const auto m = static_cast<uint64_t>(multiple);
  return static_cast<int64_t>(((static_cast<uint64_t>(x) + m - 1) / m) * m);
}

// Unsigned so rounding cannot overflow; the result is back in range.
// NOLINTNEXTLINE(bugprone-narrowing-conversions)
[[nodiscard]]
static constexpr auto round_up_8(int64_t x) noexcept -> int64_t
{
  return static_cast<int64_t>(((static_cast<uint64_t>(x) + 7) >> 3) << 3);
}

// NOLINTNEXTLINE(bugprone-narrowing-conversions)
[[nodiscard]]
static constexpr auto round_up_64(int64_t x) noexcept -> int64_t
{
  return static_cast<int64_t>(((static_cast<uint64_t>(x) + 63) >> 6) << 6);
}

//! Smallest power of two that keeps the load factor at or below 70%.
[[nodiscard]]
static constexpr auto table_capacity_for(int64_t n) noexcept -> int64_t
{
  if (n <= 0)
    return 0;
  int64_t cap = 8;
  while (cap * 7 < n * 10)
    cap <<= 1;
  return cap;
}

template <typename T>
static constexpr int64_t ssizeof = static_cast<int64_t>(sizeof(T));

// Reads a scalar without assuming the source is aligned: a corrupt header can
// point a region at an odd offset, and a misaligned pointer is UB even where
// the load would have worked. Lowers to a plain load where allowed.
template <typename T>
[[nodiscard]] inline auto load(const char* p) noexcept -> T
{
  static_assert(std::is_trivially_copyable_v<T>);
  T v{};
  memcpy(&v, p, sizeof(T));
  return v;
}

template <typename T>
inline void store(char* p, T v) noexcept
{
  static_assert(std::is_trivially_copyable_v<T>);
  memcpy(p, &v, sizeof(T));
}

//! One index entry. Fixed stride so the array is directly indexable.
struct entry
{
  int64_t data_offset{};  //!< relative to header::data_start
  int64_t stored_size{};  //!< bytes occupied in the archive
  int64_t orig_size{};    //!< bytes after decompression
  uint32_t name_offset{}; //!< offset into the name blob
  uint16_t name_size{};
  codec method{codec::store};

  [[nodiscard]] static auto load_from(const char* p) noexcept -> entry
  {
    return entry{
        .data_offset = uvfs::load<int64_t>(p),
        .stored_size = uvfs::load<int64_t>(p + 8),
        .orig_size = uvfs::load<int64_t>(p + 16),
        .name_offset = uvfs::load<uint32_t>(p + 24),
        .name_size = uvfs::load<uint16_t>(p + 28),
        .method = static_cast<codec>(uvfs::load<uint8_t>(p + 30))};
  }

  void store_to(char* p) const noexcept
  {
    uvfs::store<int64_t>(p, data_offset);
    uvfs::store<int64_t>(p + 8, stored_size);
    uvfs::store<int64_t>(p + 16, orig_size);
    uvfs::store<uint32_t>(p + 24, name_offset);
    uvfs::store<uint16_t>(p + 28, name_size);
    uvfs::store<uint8_t>(p + 30, static_cast<uint8_t>(method));
    uvfs::store<uint8_t>(p + 31, 0);
  }
};

static_assert(entry_size == 32);

struct header
{
  char head[8]{};
  uint32_t flag_bits{};
  int64_t file_size{};
  int64_t file_count{};
  int64_t index_start{};
  int64_t index_size{};
  int64_t data_start{};
  int64_t data_size{};
  int64_t table_capacity{};
  int64_t names_size{};
  int64_t dict_start{};
  int64_t dict_size{};
  uint64_t index_hash{};
  //! XXH3 of the dictionary. Nothing else covers it, and damage there changes
  //! what every entry using it decodes to.
  uint64_t dict_hash{};
  uint64_t header_hash{};

  //! Byte range covered by header_hash: everything up to the field itself.
  static constexpr int64_t hashed_prefix = 120;

  [[nodiscard]] static auto load_from(const char* p) noexcept -> header
  {
    header h;
    memcpy(h.head, p, 8);
    h.flag_bits = uvfs::load<uint32_t>(p + 8);
    h.file_size = uvfs::load<int64_t>(p + 16);
    h.file_count = uvfs::load<int64_t>(p + 24);
    h.index_start = uvfs::load<int64_t>(p + 32);
    h.index_size = uvfs::load<int64_t>(p + 40);
    h.data_start = uvfs::load<int64_t>(p + 48);
    h.data_size = uvfs::load<int64_t>(p + 56);
    h.table_capacity = uvfs::load<int64_t>(p + 64);
    h.names_size = uvfs::load<int64_t>(p + 72);
    h.dict_start = uvfs::load<int64_t>(p + 80);
    h.dict_size = uvfs::load<int64_t>(p + 88);
    h.index_hash = uvfs::load<uint64_t>(p + 96);
    h.dict_hash = uvfs::load<uint64_t>(p + 104);
    h.header_hash = uvfs::load<uint64_t>(p + 120);
    return h;
  }

  void store_to(char* p) const noexcept
  {
    memset(p, 0, static_cast<size_t>(header_size));
    memcpy(p, head, 8);
    uvfs::store<uint32_t>(p + 8, flag_bits);
    uvfs::store<int64_t>(p + 16, file_size);
    uvfs::store<int64_t>(p + 24, file_count);
    uvfs::store<int64_t>(p + 32, index_start);
    uvfs::store<int64_t>(p + 40, index_size);
    uvfs::store<int64_t>(p + 48, data_start);
    uvfs::store<int64_t>(p + 56, data_size);
    uvfs::store<int64_t>(p + 64, table_capacity);
    uvfs::store<int64_t>(p + 72, names_size);
    uvfs::store<int64_t>(p + 80, dict_start);
    uvfs::store<int64_t>(p + 88, dict_size);
    uvfs::store<uint64_t>(p + 96, index_hash);
    uvfs::store<uint64_t>(p + 104, dict_hash);
    uvfs::store<uint64_t>(p + 120, header_hash);
  }

  [[nodiscard]] auto version() const noexcept -> uint8_t
  {
    return static_cast<uint8_t>(head[7]);
  }
  [[nodiscard]] auto has(uint32_t f) const noexcept -> bool
  {
    return (flag_bits & f) != 0;
  }

  // Offsets of the index sub-regions, relative to index_start. Only meaningful
  // once validate() has accepted the header.
  [[nodiscard]] auto entries_bytes() const noexcept -> int64_t
  {
    return file_count * entry_size;
  }
  [[nodiscard]] auto table_offset() const noexcept -> int64_t { return entries_bytes(); }
  [[nodiscard]] auto table_bytes() const noexcept -> int64_t
  {
    return table_capacity * 8;
  }
  [[nodiscard]] auto hashes_offset() const noexcept -> int64_t
  {
    return table_offset() + table_bytes();
  }
  [[nodiscard]] auto hashes_bytes() const noexcept -> int64_t
  {
    return has(flag_entry_hashes) ? file_count * 8 : 0;
  }
  [[nodiscard]] auto names_offset() const noexcept -> int64_t
  {
    return hashes_offset() + hashes_bytes();
  }

  // Comparisons are unsigned and against a remaining-space budget: `a + b >
  // limit` overflows for corrupt values, and signed overflow would make the
  // check itself the bug.
  //! Checked before the header hash, so a file that is not an archive at all
  //! says so rather than reporting a checksum mismatch.
  void check_identity() const
  {
    if (memcmp(magic, this->head, sizeof(magic)) != 0)
      throw std::runtime_error("uvfs: not a uvfs archive: ");

    if (version() != format_version)
      throw std::runtime_error(
          "uvfs: unsupported format version " + std::to_string(version())
          + " (this build reads version " + std::to_string(format_version) + "): ");
  }

  void validate(int64_t filesize) const
  {
    check_identity();

    if ((flag_bits & ~static_cast<uint32_t>(flag_all_known)) != 0)
      throw std::runtime_error(
          "uvfs: archive uses features this build does not know about "
          "(flags 0x"
          + std::to_string(flag_bits) + "): ");

    if (filesize < header_size)
      throw std::runtime_error("uvfs: file is smaller than a header: ");

    if (file_size != filesize)
      throw std::runtime_error(
          "uvfs: header says " + std::to_string(file_size) + " bytes but file is "
          + std::to_string(filesize) + " (truncated or appended to?): ");

    if (file_count < 0 || index_start < 0 || index_size < 0 || data_start < 0
        || data_size < 0 || table_capacity < 0 || names_size < 0 || dict_start < 0
        || dict_size < 0)
      throw std::runtime_error("uvfs: negative field in header: ");

    if (file_count > max_file_count)
      throw std::runtime_error("uvfs: file count is implausibly large: ");

    // Lookup masks with capacity-1, so it must be a power of two.
    if (table_capacity != table_capacity_for(file_count))
      throw std::runtime_error(
          "uvfs: hash table capacity does not match the file count: ");

    const auto total = static_cast<uint64_t>(filesize);
    const auto idx_at = static_cast<uint64_t>(index_start);
    const auto idx_sz = static_cast<uint64_t>(index_size);
    const auto dat_at = static_cast<uint64_t>(data_start);
    const auto dat_sz = static_cast<uint64_t>(data_size);

    if (idx_at < static_cast<uint64_t>(header_size) || idx_at > total)
      throw std::runtime_error("uvfs: index starts outside the file: ");
    if (idx_sz > total - idx_at)
      throw std::runtime_error("uvfs: index extends past the end of the file: ");

    // file_count is bounded above, so these products cannot overflow.
    const auto need
        = static_cast<uint64_t>(names_offset()) + static_cast<uint64_t>(names_size);
    if (need > idx_sz)
      throw std::runtime_error(
          "uvfs: index is too small for its entries, table and names: ");

    if (dat_at < idx_at + idx_sz)
      throw std::runtime_error("uvfs: data region overlaps the index: ");
    if (dat_at > total)
      throw std::runtime_error("uvfs: data starts outside the file: ");
    if (dat_sz > total - dat_at)
      throw std::runtime_error("uvfs: data extends past the end of the file: ");

    // Callers rely on the pointers they get back being aligned.
    if (data_start % payload_alignment != 0)
      throw std::runtime_error("uvfs: data region is not 64-byte aligned: ");

    if (has(flag_has_dictionary))
    {
      const auto d_at = static_cast<uint64_t>(dict_start);
      const auto d_sz = static_cast<uint64_t>(dict_size);
      if (d_at < static_cast<uint64_t>(header_size) || d_at > total
          || d_sz > total - d_at || d_sz == 0)
        throw std::runtime_error("uvfs: dictionary lies outside the file: ");
    }
    else if (dict_size != 0)
    {
      throw std::runtime_error("uvfs: dictionary present but the flag is not set: ");
    }
  }
};
}
