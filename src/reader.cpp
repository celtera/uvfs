#include "fd_handle.hpp"
#include "format.hpp"
#include "hash.hpp"
#include "zstd_codec.hpp"

#include <uvfs/reader.hpp>

#include <cassert>
#include <mutex>
#include <cstring>
#include <stdexcept>
#include <string>

namespace uvfs
{

struct reader::impl
{
  explicit impl(std::string_view path)
      : handle{fd_handle::open_ro(std::string{path}.c_str())}
  {
  }

  fd_handle handle;
  mmap_handle<const char*> map;
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

  // Opening does not walk the index -- that is the whole point of the format --
  // so entries are checked where they are used instead. These are a handful of
  // comparisons against values already in registers, and they are what keeps a
  // corrupt archive from turning into an out-of-bounds read. Integrity of the
  // index as a whole is a separate question, answered by its hash.
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
    if (e.data_offset > h.data_size
        || e.stored_size > h.data_size - e.data_offset)
      return false;

    switch (e.method)
    {
      case codec::store:
        // A stored payload is the file, so the two sizes must agree.
        return e.orig_size == e.stored_size;
      case codec::zstd:
        return true;
      case codec::zstd_dict:
        return h.has(flag_has_dictionary);
    }
    return false; // unknown codec
  }

  [[nodiscard]] auto name_at(const entry& e) const noexcept -> std::string_view
  {
    return {names + e.name_offset, e.name_size};
  }

  //! Checks one payload against its stored content hash. Only called when the
  //! reader was opened with integrity::full, so the cost lands on the entries
  //! actually read rather than on every open.
  void verify_payload(int64_t i, const entry& e, std::string_view name) const
  {
    if (check != integrity::full || !hashes)
      return;
    const auto want = load<uint64_t>(hashes + i * 8);
    const auto got = hash_bytes(
        payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
    if (got != want)
      throw std::runtime_error(
          "uvfs: content checksum mismatch for " + std::string{name}
          + ", the payload is damaged");
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

  //! Open addressing with linear probing. The 32-bit fingerprint stored beside
  //! the entry index means a probe that hits an occupied but different slot is
  //! rejected without touching the entry or the name blob, which is what keeps
  //! the cost at roughly one cache miss.
  [[nodiscard]] auto lookup(std::string_view path) const noexcept -> int64_t
  {
    if (h.table_capacity == 0)
      return -1;
    const uint64_t hv = hash_name(path);
    const uint64_t fingerprint = hv >> 32;
    const auto mask = static_cast<uint64_t>(h.table_capacity - 1);
    uint64_t slot = hv & mask;
    // The probe is bounded by the table size rather than relying on finding an
    // empty slot. A well-formed table is at most 70% full, so a miss stops
    // after a couple of probes; but validate() checks the table's *capacity*,
    // not its contents, and a corrupt table with no empty slot anywhere would
    // otherwise spin forever. After `capacity` probes every slot has been
    // visited, so there is nothing left to find.
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
          // sane() first: comparing the name reads the blob.
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
  const auto filesize = impl->handle.filesize();
  if (filesize < header_size)
    throw std::runtime_error("uvfs: file is smaller than a header: ");

  impl->map = impl->handle.map_ro(filesize);
  const char* const base = impl->map.bytes;

  impl->h = header::load_from(base);

  // Identity first, so a file that is not an archive at all, or is a newer
  // format, gets a message that says so.
  impl->h.check_identity();

  // Then the header hash, before any offset in it is believed: every other
  // region in the file is located through these 128 bytes. It covers 120
  // bytes, so it is free, and therefore not optional.
  if (impl->h.header_hash != hash_bytes(base, header::hashed_prefix))
    throw std::runtime_error(
        "uvfs: header checksum mismatch, the file is damaged: ");

  impl->h.validate(filesize);

  // Opening is exactly this: map, check the header, take pointers. There is
  // no loop over the entries -- the index is already an index.
  const header& h = impl->h;
  impl->index = base + h.index_start;
  impl->entries = impl->index;
  impl->table = impl->index + h.table_offset();
  impl->hashes
      = h.has(flag_entry_hashes) ? impl->index + h.hashes_offset() : nullptr;
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
    // Checked unconditionally rather than by integrity level: a damaged
    // dictionary corrupts every entry that uses it, and no other check in the
    // archive can see it. Dictionaries are small, so this costs a few
    // microseconds and only for archives that carry one.
    if (impl->h.dict_hash
        != hash_bytes(base + h.dict_start, static_cast<std::size_t>(h.dict_size)))
      throw std::runtime_error(
          "uvfs: dictionary checksum mismatch, the archive is damaged: ");

    impl->ddict.reset(ZSTD_createDDict(
        base + h.dict_start, static_cast<std::size_t>(h.dict_size)));
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

auto reader::size() const noexcept -> std::size_t
{
  return static_cast<std::size_t>(impl->h.file_count);
}

auto reader::count() const noexcept -> int64_t
{
  return impl->h.file_count;
}

auto reader::has_content_hashes() const noexcept -> bool
{
  return impl->hashes != nullptr;
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
  if (i < 0 || i >= impl->h.file_count)
    throw std::out_of_range(
        "uvfs: entry index " + std::to_string(i) + " is out of range");
  const entry e = impl->checked_entry_at(i);
  return to_info(impl->name_at(e), e);
}

auto reader::stat(std::string_view path) const noexcept -> std::optional<file_info>
{
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->entry_at(i);
  return to_info(impl->name_at(e), e);
}

auto reader::find(std::string_view path) const -> std::optional<byte_array>
{
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
  const int64_t n = impl->h.file_count;
  for (int64_t i = 0; i < n; i++)
  {
    const entry e = impl->checked_entry_at(i);
    const auto name = impl->name_at(e);
    impl->verify_payload(i, e, name);
    const auto bytes
        = e.method == codec::store
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
  const auto i = impl->lookup(path);
  if (i < 0)
    return std::nullopt;
  const entry e = impl->checked_entry_at(i);
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
  const ZSTD_DDict* dict
      = (e.method == codec::zstd_dict) ? impl->ddict.get() : nullptr;
  if (e.method == codec::zstd_dict && !dict)
    throw std::runtime_error(
        "uvfs: " + std::string{path}
        + " needs the archive dictionary, which is missing");
  {
    const std::lock_guard lock{impl->codec_mutex};
    impl->decompressor.decompress(
        out, e.orig_size, impl->payloads + e.data_offset, e.stored_size, dict,
        path);
  }
  return e.orig_size;
#else
  throw no_zstd_error("reading " + std::string{path});
#endif
}

auto reader::read(std::string_view path) const -> std::optional<std::vector<char>>
{
  const auto info = stat(path);
  if (!info)
    return std::nullopt;
  std::vector<char> out(static_cast<std::size_t>(info->size));
  const auto n = read_into(path, out.data(), static_cast<int64_t>(out.size()));
  if (!n)
    return std::nullopt;
  return out;
}

auto reader::verify() const -> std::vector<std::string>
{
  std::vector<std::string> bad;
  if (!impl->hashes)
    return bad;

  const int64_t n = impl->h.file_count;
  for (int64_t i = 0; i < n; i++)
  {
    const entry e = impl->checked_entry_at(i);
    const auto want = load<uint64_t>(impl->hashes + i * 8);
    // Hash the bytes as they sit in the archive, which detects damage without
    // paying for decompression. Note this covers the payload only: what a
    // compressed payload decodes *to* also depends on the dictionary, which is
    // why that has a checksum of its own, verified at open.
    const auto got = hash_bytes(
        impl->payloads + e.data_offset, static_cast<std::size_t>(e.stored_size));
    if (got != want)
      bad.emplace_back(impl->name_at(e));
  }
  return bad;
}

}
