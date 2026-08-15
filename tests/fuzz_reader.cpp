// Fuzz target for the archive parser.
//
// The reader is the only part of uvfs that consumes bytes it did not produce,
// so it is the only part where a malformed input must be handled rather than
// trusted. The contract being fuzzed is narrow but absolute: for any byte
// string at all, opening it either throws or yields a reader whose entries can
// all be walked and read without leaving the mapping. It is never allowed to
// crash.
//
// Build:
//   cmake -DUVFS_BUILD_FUZZERS=ON -DUVFS_SANITIZE=address,undefined \
//         -DCMAKE_CXX_COMPILER=clang++
//   ./uvfs_fuzz_reader corpus/ -max_total_time=300
#include <unistd.h>
#include <uvfs/reader.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
//! libFuzzer hands us bytes, but the reader opens a path and mmaps it, so the
//! input has to reach the filesystem. A single reused temporary keeps this
//! from dominating the run.
auto scratch_path() -> const std::string&
{
  static const std::string p
      = [] { return "/tmp/uvfs-fuzz-" + std::to_string(::getpid()) + ".uvfs"; }();
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
    // header_only is the weakest setting, so it is the one worth fuzzing:
    // anything the index hash would have caught has to be survivable without
    // it, because memory safety must not depend on a checksum matching.
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

    // Reading is fuzzed too. The original target stopped at find(), which
    // only ever hands back a pointer into the mapping -- so the entire
    // decompression path, and every size the index claims about it, went
    // unexercised. That is exactly where an unbounded allocation was hiding.
    std::vector<char> buffer;
    for (int64_t i = 0; i < r.count(); i++)
    {
      const auto info = r.at(i);
      sink += static_cast<std::size_t>(info.stored_size);
      // Look the entry up by its own name: exercises the hash table with keys
      // that are guaranteed to be present.
      (void)r.stat(info.path);
      (void)r.find(info.path);

      // A refusal is the correct outcome for a corrupt entry, so failures are
      // swallowed per entry rather than abandoning the rest of the archive.
      // The cap keeps a legitimately large claim from being reported as an
      // out-of-memory finding; anything past it is the allocation bound's
      // problem, and there is a unit test for that.
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
