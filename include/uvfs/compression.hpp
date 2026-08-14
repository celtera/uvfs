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
  //! Compress, but keep the raw bytes whenever compression does not pay for
  //! itself. This is the right setting for mixed content: already-compressed
  //! audio, video and images fall back to being stored, which keeps their
  //! zero-copy pointer and costs nothing at read time, while text and other
  //! compressible data still shrink.
  automatic,
};

struct UVFS_EXPORT compression_settings
{
  compression method{compression::none};

  //! zstd compression level. 3 is zstd's default and a good balance; 19 is
  //! much slower for a few percent more.
  int level{3};

  //! A compressed payload is only kept if it saves at least this percentage
  //! of the original size. Below it, the entry is stored verbatim instead.
  //!
  //! This is deliberately not zero. A payload that compresses by 2% has given
  //! up its zero-copy pointer and gained a decompression step on every read,
  //! in exchange for almost nothing -- so uvfs stores it instead. Only applies
  //! to compression::automatic.
  int min_gain_percent{10};

  //! Payloads smaller than this are stored verbatim. Below a few dozen bytes a
  //! zstd frame header costs more than the content saves.
  int64_t min_size{64};

  //! When non-zero, train a zstd dictionary of this size from the inputs and
  //! store it in the archive. A shared dictionary is what makes compression
  //! work on many small files, where each payload is too short to build up any
  //! history of its own. Useless for large or already-compressed payloads.
  int64_t dictionary_size{0};

  //! How many inputs to sample when training the dictionary.
  int dictionary_samples{16384};
};

} // namespace uvfs
