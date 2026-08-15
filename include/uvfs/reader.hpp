#pragma once
#include "config.hpp"
#include "function_ref.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <string_view>

namespace uvfs
{

//! How an entry's bytes are stored in the archive.
enum class stored_as : uint8_t
{
  raw = 0,       //!< verbatim; a pointer into the mapping can be handed out
  compressed = 1 //!< must be decompressed before use
};

//! Everything known about one entry without touching its payload.
struct UVFS_EXPORT file_info
{
  std::string_view path;
  int64_t size{};        //!< size after decompression: the file's real size
  int64_t stored_size{}; //!< bytes this entry occupies in the archive
  stored_as storage{stored_as::raw};
};

//! How much to check when opening. The header and, where present, the
//! dictionary are always checked; entries are always bounds-checked when used,
//! so no setting here can produce an out-of-bounds read. The levels buy
//! detection of structurally plausible damage: a flipped bit inside a name, an
//! offset that still lands in the data region but on the wrong payload.
enum class integrity
{
  //! Header hash only. Opening stays O(1) regardless of archive size.
  header_only,
  //! Also hash the whole index at open: one sequential pass, and catches any
  //! damage to the entries, the table or the names.
  index,
  //! Also check each payload's content hash as it is read, so the cost tracks
  //! what is used. Requires an archive written with content hashes.
  full,
};

struct UVFS_EXPORT reader
{
public:
  explicit reader(std::string_view path, integrity check = integrity::header_only);
  reader(const reader&) = delete;
  auto operator=(const reader&) -> reader& = delete;
  //! Moving leaves the source behaving as an empty archive.
  reader(reader&&) noexcept;
  auto operator=(reader&&) noexcept -> reader&;
  ~reader();

  using byte_array = std::string_view;

  [[nodiscard]] auto size() const noexcept -> std::size_t;
  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  //! A pointer straight into the mapping: no copy, 64-byte aligned. nullopt
  //! when the entry is absent or compressed, since a compressed entry has no
  //! verbatim bytes to point at. Throws only under integrity::full, when the
  //! payload fails its checksum.
  [[nodiscard]] auto find(std::string_view path) const -> std::optional<byte_array>;

  //! Metadata without touching the payload.
  [[nodiscard]] auto
  stat(std::string_view path) const noexcept -> std::optional<file_info>;

  //! Decompresses if needed. Works for every entry.
  [[nodiscard]] auto
  read(std::string_view path) const -> std::optional<std::vector<char>>;

  //! Decompresses into caller-provided storage. `out` must be at least
  //! stat()->size bytes. Returns the number of bytes written.
  [[nodiscard]] auto read_into(std::string_view path, char* out, int64_t capacity) const
      -> std::optional<int64_t>;

  // Entries are stored sorted by path, so index order is sorted order and
  // iteration touches the archive in layout order.

  struct iter_entry
  {
    std::string_view path;
    byte_array data; //!< empty for compressed entries; use read()
  };

  //! Number of entries; indices are [0, count()).
  [[nodiscard]] auto count() const noexcept -> int64_t;
  //! Entry `i` in sorted path order. Throws if out of range or corrupt.
  [[nodiscard]] auto at(int64_t i) const -> file_info;

  //! Visits every entry in sorted path order until the callback returns false.
  void for_each_file(function_ref<bool(iter_entry)> func) const;

  // ------------------------------------------------------------- integrity
  //! True when the archive carries per-entry content hashes.
  [[nodiscard]] auto has_content_hashes() const noexcept -> bool;

  //! Checks every stored content hash, so it costs as much as reading the
  //! archive. Returns the paths that did not match.
  [[nodiscard]] auto verify() const -> std::vector<std::string>;

private:
  struct impl;
  std::unique_ptr<struct impl> impl;
};

}
