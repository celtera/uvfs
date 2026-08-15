#pragma once
#include "config.hpp"

#include <cstdint>

namespace uvfs
{

//! How the writer decides what to do with a payload.
enum class compression
{
  //! Store every payload verbatim. Every entry keeps a zero-copy pointer.
  none,
  //! Compress every payload, whether or not it helps.
  always,
  //! Compress, but keep the raw bytes where compression does not pay. The
  //! setting for mixed content: already-compressed media falls back to being
  //! stored and keeps its zero-copy pointer.
  automatic,
};

struct UVFS_EXPORT compression_settings
{
  compression method{compression::none};

  //! zstd level. 3 is the default; 19 is much slower for a few percent.
  int level{3};

  //! Minimum saving for a compressed payload to be kept; below it the entry is
  //! stored verbatim. Not zero: a payload that compresses by 2% has given up
  //! its zero-copy pointer for almost nothing. automatic only.
  int min_gain_percent{10};

  //! Below a few dozen bytes a zstd frame header costs more than it saves.
  int64_t min_size{64};

  //! Train a dictionary of this size and store it in the archive. What makes
  //! compression work on many small files; useless for large ones.
  int64_t dictionary_size{0};

  //! How many inputs to sample when training the dictionary.
  int dictionary_samples{16384};
};

} // namespace uvfs
