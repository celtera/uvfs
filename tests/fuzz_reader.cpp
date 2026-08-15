// Fuzz target for the archive parser. For any byte string at all, opening it
// either throws or yields a reader whose entries can be walked and read
// without leaving the mapping. It may never crash.
//
// Build:
//   cmake -DUVFS_BUILD_FUZZERS=ON -DUVFS_SANITIZE=address,undefined \
//         -DCMAKE_CXX_COMPILER=clang++
//   ./uvfs_fuzz_reader corpus/ -max_total_time=300
#include "platform.hpp"

#include <uvfs/reader.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
//! The reader opens a path, so the input has to reach the filesystem. One
//! reused temporary keeps that from dominating the run.
auto scratch_path() -> const std::string&
{
  static const std::string p = [] {
    return "/tmp/uvfs-fuzz-" + std::to_string(uvfs::platform::process_id()) + ".uvfs";
  }();
  return p;
}
} // namespace

extern "C" auto LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) -> int
{
  const auto& path = scratch_path();
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
      return 0;
    if (size)
      std::fwrite(data, 1, size, f);
    std::fclose(f);
  }

  try
  {
    // The weakest setting is the one worth fuzzing: memory safety must not
    // depend on a checksum matching.
    uvfs::reader r{path, uvfs::integrity::header_only};

    std::size_t sink = 0;
    r.for_each_file(
        [&](uvfs::reader::iter_entry e)
        {
          sink += e.path.size();
          if (!e.data.empty())
            sink += static_cast<unsigned char>(e.data[0])
                    + static_cast<unsigned char>(e.data[e.data.size() - 1]);
          return true;
        });

    // Reading too: find() only hands back a pointer, which leaves the whole
    // decompression path and every size the index claims about it untouched.
    std::vector<char> buffer;
    for (int64_t i = 0; i < r.count(); i++)
    {
      const auto info = r.at(i);
      sink += static_cast<std::size_t>(info.stored_size);
      // Look the entry up by its own name: exercises the hash table with keys
      // that are guaranteed to be present.
      (void)r.stat(info.path);
      (void)r.find(info.path);

      // Refusal is correct for a corrupt entry, so failures are swallowed per
      // entry. The cap keeps a large legitimate claim from being reported as
      // an out-of-memory finding.
      constexpr int64_t read_cap = 64 << 20;
      if (info.size < 0 || info.size > read_cap)
        continue;
      try
      {
        if (auto whole = r.read(info.path))
          sink += whole->size();
      }
      catch (const std::exception&)
      {
      }
      try
      {
        buffer.assign(static_cast<std::size_t>(info.size), '\0');
        if (auto n = r.read_into(info.path, buffer.data(), std::ssize(buffer)))
          sink += static_cast<std::size_t>(*n);
      }
      catch (const std::exception&)
      {
      }
    }

    // Lookups that miss must terminate rather than probe forever.
    (void)r.find("");
    (void)r.find("/definitely/not/present");

    if (r.has_content_hashes())
      (void)r.verify();

    if (sink == 0xdeadbeef)
      std::abort(); // keep the work observable
  }
  catch (const std::exception&)
  {
    // A refusal is a correct outcome; a crash is not.
  }
  return 0;
}
