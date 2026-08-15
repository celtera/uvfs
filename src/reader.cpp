#include "format.hpp"
#include "hash.hpp"
#include "platform.hpp"
#include "zstd_codec.hpp"

#include <uvfs/reader.hpp>

#include <cassert>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace uvfs
{

struct reader::impl
{
  explicit impl(std::string_view path)
      : handle{platform::file::open_read(std::string{path}.c_str())}
  {
  }

  platform::file handle;
  platform::mapping map;
  integrity check{integrity::header_only};

#if defined(UVFS_HAS_ZSTD)
  // One decompression context per reader, reused across reads. Guarded
  // because a reader is shared across threads far more often than it is
  // written to, and zstd contexts are not thread safe.
  mutable std::mutex codec_mutex;
  mutable zstd_decompressor decompressor;
  std::unique_ptr<ZSTD_DDict, size_t (*)(ZSTD_DDict*)> ddict{nullptr, &ZSTD_freeDDict};
#endif

  header h{};
  const char* index{};    //!< start of the index region
  const char* entries{};  //!< entry array, sorted by name
  const char* table{};    //!< hash table slots
  const char* hashes{};   //!< per-entry content hashes, or null
  const char* names{};    //!< name blob
  const char* payloads{}; //!< start of the data region

  [[nodiscard]] auto entry_at(int64_t i) const noexcept -> entry
  {
    return entry::load_from(entries + i * entry_size);
  }

  // Opening does not walk the index, so entries are checked where they are
  // used. A few comparisons on values already in registers; whole-index
  // integrity is a separate question, answered by its hash.
  [[nodiscard]] auto sane(const entry& e) const noexcept -> bool
  {
    if (e.name_size == 0)
      return false;
    const auto name_at_off = static_cast<int64_t>(e.name_offset);
    if (name_at_off > h.names_size
        || static_cast<int64_t>(e.name_size) > h.names_size - name_at_off)
      return false;

    if (e.data_offset < 0 || e.stored_size < 0 || e.orig_size < 0)
      return false;
    if (e.data_offset > h.data_size || e.stored_size > h.data_size - e.data_offset)
      return false;

    switch (e.method)
    {
      case codec::store:
        return e.orig_size == e.stored_size;
      case codec::zstd:
      case codec::zstd_dict:
      {
        if (e.method == codec::zstd_dict && !h.has(flag_has_dictionary))
          return false;
        // Division, not multiplication: stored_size can overflow.
        if (e.stored_size == 0)
          return e.orig_size == 0;
        return e.orig_size / max_zstd_expansion <= e.stored_size;
      }
    }
    return false; // unknown codec
  }

  [[nodiscard]] auto name_at(const entry& e) const noexcept -> std::string_view
  {
    return {names + e.name_offset, e.name_size};
  }

  //! Only under integrity::full, so the cost lands on entries actually read.
  void verify_payload(int64_t i, const entry& e, std::string_view name) const
  {
    if (check != integrity::full || !hashes)
      return;
    const auto want = load<uint64_t>(hashes + i * 8);
    const auto got
        = hash_bytes(payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
    if (got != want)
      throw std::runtime_error(
          "uvfs: content checksum mismatch for " + std::string{name}
          + ", the payload is damaged");
  }

  //! Cross-checks the index's size against the one the frame declares, before
  //! anything is sized from it. The ratio bound in sane() has to admit every
  //! frame zstd can produce, so it is too loose to rely on alone.
  void check_declared_size(const entry& e, std::string_view name) const
  {
    if (e.method == codec::store)
      return;
#if defined(UVFS_HAS_ZSTD)
    const auto declared = ZSTD_getFrameContentSize(
        payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
    if (declared == ZSTD_CONTENTSIZE_ERROR)
      throw std::runtime_error(
          "uvfs: " + std::string{name} + " is not a valid compressed frame");
    if (declared == ZSTD_CONTENTSIZE_UNKNOWN)
      return; // nothing to cross-check against; the ratio bound still applies
    if (static_cast<int64_t>(declared) != e.orig_size)
      throw std::runtime_error(
          "uvfs: index says " + std::string{name} + " is " + std::to_string(e.orig_size)
          + " bytes but its frame declares " + std::to_string(declared)
          + " (corrupt index)");
#else
    (void)name;
#endif
  }

  [[nodiscard]] auto checked_entry_at(int64_t i) const -> entry
  {
    const entry e = entry_at(i);
    if (!sane(e))
      throw std::runtime_error(
          "uvfs: index entry " + std::to_string(i)
          + " points outside the archive (corrupt index)");
    return e;
  }

  //! Open addressing with linear probing. The fingerprint beside each entry
  //! index rejects a colliding slot without touching the entry or the name.
  [[nodiscard]] auto lookup(std::string_view path) const noexcept -> int64_t
  {
    if (h.table_capacity == 0)
      return -1;
    const uint64_t hv = hash_name(path);
    const uint64_t fingerprint = hv >> 32;
    const auto mask = static_cast<uint64_t>(h.table_capacity - 1);
    uint64_t slot = hv & mask;
    // Bounded by the table size rather than by finding an empty slot:
    // validate() checks the table's capacity, not its contents, and a corrupt
    // table with no empty slot would otherwise spin forever.
    for (int64_t probes = 0; probes < h.table_capacity; probes++)
    {
      const uint64_t s = load<uint64_t>(table + slot * 8);
      if (s == empty_slot)
        return -1;
      if ((s >> 32) == fingerprint)
      {
        const auto i = static_cast<int64_t>(static_cast<uint32_t>(s));
        if (i < h.file_count)
        {
          const entry e = entry_at(i);
          // sane() first: the comparison reads the name blob.
          if (sane(e) && e.name_size == path.size()
              && memcmp(names + e.name_offset, path.data(), path.size()) == 0)
            return i;
        }
      }
      slot = (slot + 1) & mask;
    }
    return -1; // every slot probed: the table has no empty slot (corrupt)
  }
};

reader::reader(std::string_view path, integrity check)
try : impl{std::make_unique<struct impl>(path)}
{
  impl->check = check;
  const auto filesize = impl->handle.size();
  if (filesize < header_size)
    throw std::runtime_error("uvfs: file is smaller than a header: ");

  impl->map = platform::mapping::read_only(impl->handle, filesize);
  const char* const base = impl->map.data();

  impl->h = header::load_from(base);

  // Identity first, so a file that is not an archive says so.
  impl->h.check_identity();

  // Then the header hash, before any offset in it is believed. 120 bytes, so
  // it is free and not optional.
  if (impl->h.header_hash != hash_bytes(base, header::hashed_prefix))
    throw std::runtime_error("uvfs: header checksum mismatch, the file is damaged: ");

  impl->h.validate(filesize);

  // Map, check the header, take pointers. No loop over the entries.
  const header& h = impl->h;
  impl->index = base + h.index_start;
  impl->entries = impl->index;
  impl->table = impl->index + h.table_offset();
  impl->hashes = h.has(flag_entry_hashes) ? impl->index + h.hashes_offset() : nullptr;
  impl->names = impl->index + h.names_offset();
  impl->payloads = base + h.data_start;

  if (check != integrity::header_only)
  {
    if (impl->h.index_hash
        != hash_bytes(impl->index, static_cast<std::size_t>(h.index_size)))
      throw std::runtime_error(
          "uvfs: index checksum mismatch, the archive is damaged: ");
  }
  if (check == integrity::full && !impl->hashes)
    throw std::runtime_error(
        "uvfs: full integrity checking was asked for but this archive has no "
        "content hashes: ");

  if (h.has(flag_has_dictionary))
  {
#if defined(UVFS_HAS_ZSTD)
    // Unconditional: a damaged dictionary corrupts every entry that uses it
    // and nothing else can see it. Small, so it costs microseconds.
    if (impl->h.dict_hash
        != hash_bytes(base + h.dict_start, static_cast<std::size_t>(h.dict_size)))
      throw std::runtime_error(
          "uvfs: dictionary checksum mismatch, the archive is damaged: ");

    impl->ddict.reset(
        ZSTD_createDDict(base + h.dict_start, static_cast<std::size_t>(h.dict_size)));
    if (!impl->ddict)
      throw std::runtime_error("uvfs: could not load the archive dictionary: ");
#else
    throw std::runtime_error(
        "uvfs: archive uses a compression dictionary but this build has no "
        "zstd support: ");
#endif
  }
}
catch (const std::runtime_error& e)
{
  throw std::runtime_error(std::string(e.what()).append(path));
}

reader::~reader() = default;
reader::reader(reader&&) noexcept = default;
auto reader::operator=(reader&&) noexcept -> reader& = default;

// A moved-from reader has a null pimpl. "Valid but unspecified" still means
// member functions can be called, so each entry point below treats it as an
// empty archive.

auto reader::size() const noexcept -> std::size_t
{
  return impl ? static_cast<std::size_t>(impl->h.file_count) : 0u;
}

auto reader::count() const noexcept -> int64_t
{
  return impl ? impl->h.file_count : 0;
}

auto reader::has_content_hashes() const noexcept -> bool
{
  return impl && impl->hashes != nullptr;
}

namespace
{
auto to_info(std::string_view name, const entry& e) noexcept -> file_info
{
  return file_info{
      .path = name,
      .size = e.orig_size,
      .stored_size = e.stored_size,
      .storage = e.method == codec::store ? stored_as::raw : stored_as::compressed};
}
} // namespace

auto reader::at(int64_t i) const -> file_info
{
  if (!impl || i < 0 || i >= impl->h.file_count)
    throw std::out_of_range(
        "uvfs: entry index " + std::to_string(i) + " is out of range");
  const entry e = impl->checked_entry_at(i);
  return to_info(impl->name_at(e), e);
}

auto reader::stat(std::string_view path) const noexcept -> std::optional<file_info>
{
  if (!impl)
    return std::nullopt;
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->entry_at(i);
  return to_info(impl->name_at(e), e);
}

auto reader::find(std::string_view path) const -> std::optional<byte_array>
{
  if (!impl)
    return std::nullopt;
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->entry_at(i);
  if (e.method != codec::store)
    return std::nullopt; // no verbatim bytes to point at
  impl->verify_payload(i, e, path);
  return byte_array(
      impl->payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
}

void reader::for_each_file(function_ref<bool(iter_entry)> func) const
{
  if (!impl)
    return;
  const int64_t n = impl->h.file_count;
  for (int64_t i = 0; i < n; i++)
  {
    const entry e = impl->checked_entry_at(i);
    const auto name = impl->name_at(e);
    impl->verify_payload(i, e, name);
    const auto bytes = e.method == codec::store
                           ? byte_array(
                                 impl->payloads + e.data_offset,
                                 static_cast<std::size_t>(e.stored_size))
                           : byte_array{};
    if (!func(iter_entry{name, bytes}))
      break;
  }
}

auto reader::read_into(std::string_view path, char* out, int64_t capacity) const
    -> std::optional<int64_t>
{
  if (!impl)
    return std::nullopt;
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->checked_entry_at(i);
  impl->check_declared_size(e, path);
  impl->verify_payload(i, e, path);
  if (capacity < e.orig_size)
    throw std::runtime_error(
        "uvfs: buffer of " + std::to_string(capacity) + " bytes is too small for "
        + std::string{path} + " (" + std::to_string(e.orig_size) + " bytes)");

  if (e.method == codec::store)
  {
    if (e.orig_size > 0)
      memcpy(out, impl->payloads + e.data_offset, static_cast<size_t>(e.orig_size));
    return e.orig_size;
  }

#if defined(UVFS_HAS_ZSTD)
  const ZSTD_DDict* dict = (e.method == codec::zstd_dict) ? impl->ddict.get() : nullptr;
  if (e.method == codec::zstd_dict && !dict)
    throw std::runtime_error(
        "uvfs: " + std::string{path}
        + " needs the archive dictionary, which is missing");
  {
    const std::lock_guard lock{impl->codec_mutex};
    impl->decompressor.decompress(
        out, e.orig_size, impl->payloads + e.data_offset, e.stored_size, dict, path);
  }
  return e.orig_size;
#else
  throw no_zstd_error("reading " + std::string{path});
#endif
}

auto reader::read(std::string_view path) const -> std::optional<std::vector<char>>
{
  if (!impl)
    return std::nullopt;
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->checked_entry_at(i);
  // Validate before allocating, not after.
  impl->check_declared_size(e, path);

  std::vector<char> out(static_cast<std::size_t>(e.orig_size));
  const auto n = read_into(path, out.data(), static_cast<int64_t>(out.size()));
  if (!n)
    return std::nullopt;
  return out;
}

auto reader::verify() const -> std::vector<std::string>
{
  std::vector<std::string> bad;
  if (!impl || !impl->hashes)
    return bad;

  const int64_t n = impl->h.file_count;
  for (int64_t i = 0; i < n; i++)
  {
    const entry e = impl->checked_entry_at(i);
    const auto want = load<uint64_t>(impl->hashes + i * 8);
    // Hashes the stored bytes, so damage is found without decompressing. The
    // dictionary has its own checksum, verified at open.
    const auto got = hash_bytes(
        impl->payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
    if (got != want)
      bad.emplace_back(impl->name_at(e));
  }
  return bad;
}

}
