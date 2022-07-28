#pragma once
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace uvfs
{

// Header:
// 0:  UVFS\0\0\0\0
// 8:  [ i64 total file size ]
// 16: [ i64 file count ]
// 24: [ i64 index start ] [ i64 index size ]
// 40: [ i64 data start  ] [ i64 data size ]
// ..: padding
// 64: <index start>

// Index:
// [ [ i64 data start  ]   // relative to h->data_start
//   [ i64 data length ]
//   [ i16 path length ]   // min 2: /a
//   [ utf-8 path ]
// ] *, aligned to 8 bytes

// File: [ data ] *, aligned to 64 bytes

static constexpr const char ident[8] = {'U', 'V', 'F', 'S', 0, 0, 0, 1};

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

struct entry
{
  static constexpr auto static_size = 8 + 8 + 4;
  int64_t data_start;
  int64_t data_size;
  int32_t path_len;
  char path[];
};

struct header
{
  char head[8];
  int64_t file_size;
  int64_t file_count;
  int64_t index_start;
  int64_t index_size;
  int64_t data_start;
  int64_t data_size;
  char padding[8];

  void validate(int64_t filesize) const
  {
    if (memcmp(ident, this->head, sizeof(ident)) != 0)
      throw std::runtime_error("uvfs: invalid file header: ");

    if (file_size < 0)
      throw std::runtime_error("uvfs: invalid header: ");
    if (file_count < 0)
      throw std::runtime_error("uvfs: invalid header: ");

    if (index_start < ssizeof<header> || index_start >= file_size)
      throw std::runtime_error("uvfs: invalid header: ");
    if (index_size < 0)
      throw std::runtime_error("uvfs: invalid header: ");
    if (index_start + index_size > filesize)
      throw std::runtime_error("uvfs: invalid header: ");

    if (data_start < (index_start + index_size))
      throw std::runtime_error("uvfs: invalid header: ");
    if (data_start + data_size > filesize)
      throw std::runtime_error("uvfs: invalid header: ");

    if (index_start + index_size > data_start)
      throw std::runtime_error("uvfs: invalid header: ");

    if (index_size + data_size + ssizeof<header> > filesize)
      throw std::runtime_error("uvfs: invalid header: ");

    if (file_size != filesize)
      throw std::runtime_error("uvfs: invalid file size: ");

    if (file_count >= 0)
    {
      // Imperfect check but better than nothing
      if (file_count * entry::static_size > index_size)
        throw std::runtime_error("uvfs: invalid header: ");
    }
    else
    {
      if (index_size != 0 || data_size != 0)
        throw std::runtime_error("uvfs: invalid header: ");
    }
  }
};
}
