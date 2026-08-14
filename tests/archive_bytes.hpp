#pragma once
// Helpers for tests that poke at the raw bytes of an archive. Offsets come
// from the format header itself so these do not rot when the layout moves.
#include "format.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <fstream>
#include <string>
#include <vector>

namespace uvfs::test
{

// Byte offsets of the header fields, in the order store_to() writes them.
namespace hdr
{
inline constexpr std::size_t magic = 0;
inline constexpr std::size_t version = 7;
inline constexpr std::size_t flags = 8;
inline constexpr std::size_t file_size = 16;
inline constexpr std::size_t file_count = 24;
inline constexpr std::size_t index_start = 32;
inline constexpr std::size_t index_size = 40;
inline constexpr std::size_t data_start = 48;
inline constexpr std::size_t data_size = 56;
inline constexpr std::size_t table_capacity = 64;
inline constexpr std::size_t names_size = 72;
inline constexpr std::size_t dict_start = 80;
inline constexpr std::size_t dict_size = 88;
inline constexpr std::size_t index_hash = 96;
inline constexpr std::size_t header_hash = 120;
} // namespace hdr

// Byte offsets within one index entry.
namespace ent
{
inline constexpr std::size_t data_offset = 0;
inline constexpr std::size_t stored_size = 8;
inline constexpr std::size_t orig_size = 16;
inline constexpr std::size_t name_offset = 24;
inline constexpr std::size_t name_size = 28;
inline constexpr std::size_t method = 30;
} // namespace ent

inline auto slurp(const std::string& p) -> std::vector<char>
{
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline void spit(const std::string& p, const std::vector<char>& b)
{
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}

//! Offset of entry `i` in a well-formed archive.
inline auto entry_offset(const std::vector<char>& bytes, int64_t i) -> std::size_t
{
  const auto h = header::load_from(bytes.data());
  return static_cast<std::size_t>(h.index_start + i * entry_size);
}

//! Builds a small but structurally complete archive: several entries, varied
//! path lengths, varied payload sizes.
inline auto build_sample(const scratch_dir& dir) -> std::string
{
  uvfs::writer w;
  const char* names[] = {"/a", "/bb/cc", "/dddddddddddddddd", "/e.bin", "/f/g/h"};
  int i = 0;
  for (auto* n : names)
  {
    const auto src = dir.make_file(
        "src" + std::to_string(i), 40u + static_cast<unsigned>(i) * 17u,
        static_cast<uint64_t>(i + 1));
    w.add_file(n, src);
    i++;
  }
  const auto arc = dir / "sample.uvfs";
  w.commit(arc);
  return arc;
}

//! Opens an archive and touches every entry. Because v2 does not walk the
//! index at open, a corrupt entry surfaces when it is used rather than when
//! the file is opened -- so "rejected" means either of the two.
struct load_result
{
  bool opened{};
  bool read_everything{};
  std::string error;

  [[nodiscard]] auto rejected() const noexcept -> bool
  {
    return !opened || !read_everything;
  }
};

inline auto load_and_read_all(const std::string& p) -> load_result
{
  load_result r;
  try
  {
    uvfs::reader reader{p};
    r.opened = true;
    std::size_t sink = 0;
    reader.for_each_file(
        [&](uvfs::reader::iter_entry e)
        {
          sink += e.path.size();
          if (!e.data.empty())
            sink += static_cast<unsigned char>(e.data[0])
                    + static_cast<unsigned char>(e.data[e.data.size() - 1]);
          return true;
        });
    for (int64_t i = 0; i < reader.count(); i++)
      sink += static_cast<std::size_t>(reader.at(i).size);
    r.read_everything = sink < (std::size_t{1} << 40);
  }
  catch (const std::exception& e)
  {
    r.error = e.what();
  }
  return r;
}

} // namespace uvfs::test
