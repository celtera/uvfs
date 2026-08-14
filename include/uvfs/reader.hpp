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

struct UVFS_EXPORT reader
{
public:
  explicit reader(std::string_view path);
  reader(const reader&) = delete;
  auto operator=(const reader&) -> reader& = delete;
  reader(reader&&) noexcept;
  auto operator=(reader&&) noexcept -> reader&;
  ~reader();

  using byte_array = std::string_view;

  [[nodiscard]] auto size() const noexcept -> std::size_t;
  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

  //! A pointer straight into the mapping: no copy, no allocation, 64-byte
  //! aligned. Returns nullopt when the entry is absent *or* is compressed,
  //! since a compressed entry has no verbatim bytes to point at. Use read()
  //! when the entry may be compressed, or stat() to find out which it is.
  [[nodiscard]] auto find(std::string_view path) const noexcept
      -> std::optional<byte_array>;

  //! Metadata without touching the payload.
  [[nodiscard]] auto stat(std::string_view path) const noexcept
      -> std::optional<file_info>;

  //! Decompresses if needed. Works for every entry.
  [[nodiscard]] auto read(std::string_view path) const
      -> std::optional<std::vector<char>>;

  //! Decompresses into caller-provided storage. `out` must be at least
  //! stat()->size bytes. Returns the number of bytes written.
  [[nodiscard]] auto read_into(std::string_view path, char* out, int64_t capacity)
      const -> std::optional<int64_t>;

  // ------------------------------------------------------------- iteration
  // Entries are stored sorted by path, so index order is sorted order. That is
  // what extraction wants, and it means iteration touches the archive in
  // layout order instead of hash order.

  struct iter_entry
  {
    std::string_view path;
    byte_array data; //!< empty for compressed entries; use read()
  };

  //! Number of entries; indices are [0, count()).
  [[nodiscard]] auto count() const noexcept -> int64_t;
  //! Metadata for entry `i`, in sorted path order.
  //! Throws std::out_of_range if `i` is out of range, or std::runtime_error if
  //! the entry is corrupt.
  [[nodiscard]] auto at(int64_t i) const -> file_info;

  //! Visits every entry in sorted path order until the callback returns false.
  void for_each_file(function_ref<bool(iter_entry)> func) const;

  // ------------------------------------------------------------- integrity
  //! True when the archive carries per-entry content hashes.
  [[nodiscard]] auto has_content_hashes() const noexcept -> bool;

  //! Verifies stored content hashes. Reads every payload, so it is as
  //! expensive as reading the archive; that is why it is not done at open.
  //! Returns the paths whose contents did not match.
  [[nodiscard]] auto verify() const -> std::vector<std::string>;

private:
  struct impl;
  std::unique_ptr<struct impl> impl;
};

}
