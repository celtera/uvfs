#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <cstdint>
#include <fstream>
#include <random>
#include <vector>

using namespace uvfs::test;

namespace
{
auto slurp(const std::string& p) -> std::vector<char>
{
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void spit(const std::string& p, const std::vector<char>& b)
{
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}

// Builds a small but structurally complete archive: several entries, varied
// path lengths, varied payload sizes.
auto build_sample(const scratch_dir& dir) -> std::string
{
  uvfs::writer w;
  const char* names[] = {"/a", "/bb/cc", "/dddddddddddddddd", "/e.bin", "/f/g/h"};
  int i = 0;
  for (auto* n : names)
  {
    const auto src
        = dir.make_file("src" + std::to_string(i), 40u + static_cast<unsigned>(i) * 17u,
                        static_cast<uint64_t>(i + 1));
    w.add_file(n, src);
    i++;
  }
  const auto arc = dir / "sample.uvfs";
  w.commit(arc);
  return arc;
}
} // namespace

// -------------------------------------------------------------------- R1
UVFS_TEST("format/last_payload_ending_exactly_at_eof")
{
  // A payload whose size is a multiple of 64 needs no alignment padding, so
  // the archive's last byte is also the payload's last byte. The bounds check
  // used to reject that, making roughly one archive in 64 unreadable.
  for (std::size_t sz : {1u, 63u, 64u, 65u, 127u, 128u, 129u, 4096u})
  {
    scratch_dir dir{"eof"};
    const auto src = dir.make_file("a.bin", sz, sz + 1);
    const auto arc = dir / "out.uvfs";

    uvfs::writer w;
    w.add_file("/a.bin", src);
    w.commit(arc);

    CHECK_NOTHROW({
      uvfs::reader r{arc};
      auto got = r.find("/a.bin");
      CHECK(got.has_value());
      if (got)
      {
        CHECK_EQ(got->size(), sz);
        CHECK(*got == expected_bytes(sz, sz + 1));
      }
    });
  }
}

UVFS_TEST("format/every_payload_size_class_roundtrips")
{
  // The multiple-of-64 case only broke for the *last* entry, so cover the
  // interior positions too.
  scratch_dir dir{"sizes"};
  const std::size_t sizes[] = {0, 1, 63, 64, 65, 127, 128, 191, 192, 1000};
  uvfs::writer w;
  int i = 0;
  for (auto sz : sizes)
  {
    w.add_file(
        "/f" + std::to_string(i),
        dir.make_file("f" + std::to_string(i), sz, sz + 7));
    i++;
  }
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  i = 0;
  for (auto sz : sizes)
  {
    auto got = r.find("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
    {
      CHECK_EQ(got->size(), sz);
      CHECK(*got == expected_bytes(sz, sz + 7));
    }
    i++;
  }
}

// -------------------------------------------------------------------- R2
UVFS_TEST("format/empty_archive_roundtrips")
{
  scratch_dir dir{"empty"};
  const auto arc = dir / "empty.uvfs";

  uvfs::writer w;
  w.commit(arc);

  CHECK_NOTHROW({
    uvfs::reader r{arc};
    CHECK_EQ(r.size(), std::size_t{0});
    CHECK(!r.find("/anything").has_value());
    int seen = 0;
    r.for_each_file(
        [&](uvfs::reader::iter_entry)
        {
          seen++;
          return true;
        });
    CHECK_EQ(seen, 0);
  });
}

// -------------------------------------------------------------------- R4
UVFS_TEST("format/rejects_corrupt_path_length")
{
  scratch_dir dir{"pathlen"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  // path_len of the first index entry lives at index_start + 16.
  const std::size_t off = 64 + 16;
  for (int32_t evil : {int32_t{0x7fffffff}, int32_t{-1}, int32_t{0}, int32_t{1 << 20}})
  {
    auto patched = bytes;
    std::memcpy(patched.data() + off, &evil, sizeof evil);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK_THROWS(uvfs::reader{bad});
  }
}

UVFS_TEST("format/rejects_corrupt_entry_offsets")
{
  scratch_dir dir{"offsets"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  struct { std::size_t off; int64_t value; } cases[] = {
      {64 + 0, int64_t{-1}},                       // data_start negative
      {64 + 0, int64_t{1} << 40},                  // data_start past the data region
      {64 + 8, int64_t{-1}},                       // data_size negative
      {64 + 8, int64_t{1} << 40},                  // data_size past the data region
      {64 + 0, std::numeric_limits<int64_t>::max()},
      {64 + 8, std::numeric_limits<int64_t>::max()},
  };
  for (auto& c : cases)
  {
    auto patched = bytes;
    std::memcpy(patched.data() + c.off, &c.value, sizeof c.value);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK_THROWS(uvfs::reader{bad});
  }
}

UVFS_TEST("format/rejects_corrupt_header_fields")
{
  scratch_dir dir{"header"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  // offsets of the int64 fields inside the header
  const std::size_t fields[]
      = {8 /*file_size*/, 16 /*file_count*/, 24 /*index_start*/,
         32 /*index_size*/, 40 /*data_start*/, 48 /*data_size*/};
  const int64_t values[] = {-1, std::numeric_limits<int64_t>::max(),
                            std::numeric_limits<int64_t>::min(), 1LL << 50, 7};
  for (auto off : fields)
    for (auto v : values)
    {
      auto patched = bytes;
      std::memcpy(patched.data() + off, &v, sizeof v);
      const auto bad = dir / "bad.uvfs";
      spit(bad, patched);
      // Must reject or accept cleanly, never crash. Every one of these is a
      // structural inconsistency, so rejection is the expected outcome.
      CHECK_THROWS(uvfs::reader{bad});
    }
}

UVFS_TEST("format/rejects_bad_magic_and_version")
{
  scratch_dir dir{"magic"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  {
    auto patched = bytes;
    patched[0] = 'X';
    const auto bad = dir / "bad-magic.uvfs";
    spit(bad, patched);
    CHECK_THROWS(uvfs::reader{bad});
  }
  {
    auto patched = bytes;
    patched[7] = 99; // version byte
    const auto bad = dir / "bad-version.uvfs";
    spit(bad, patched);
    bool threw = false;
    std::string msg;
    try
    {
      uvfs::reader r{bad};
    }
    catch (const std::exception& e)
    {
      threw = true;
      msg = e.what();
    }
    CHECK(threw);
    // The message must name the version, not just say "invalid".
    CHECK(msg.find("version 99") != std::string::npos);
  }
}

UVFS_TEST("format/rejects_truncated_files")
{
  scratch_dir dir{"truncated"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);

  for (std::size_t keep : {std::size_t{0}, std::size_t{1}, std::size_t{63},
                           std::size_t{64}, bytes.size() / 2, bytes.size() - 1})
  {
    std::vector<char> cut(bytes.begin(), bytes.begin() + static_cast<long>(keep));
    const auto bad = dir / "cut.uvfs";
    spit(bad, cut);
    CHECK_THROWS(uvfs::reader{bad});
  }
}

UVFS_TEST("format/single_byte_corruption_never_crashes")
{
  // The reader must either reject an archive or read it without leaving the
  // mapping. Under ASAN this is the test that proves it; without ASAN it still
  // catches wild pointers that segfault.
  scratch_dir dir{"mutate"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);

  std::mt19937_64 rng{20240814};
  int accepted = 0, rejected = 0;
  const auto bad = dir / "mutant.uvfs";
  for (int iter = 0; iter < 4000; iter++)
  {
    auto patched = bytes;
    // Bias toward the header and index, where the structure lives.
    const std::size_t limit
        = (iter % 4 == 0) ? patched.size() : std::min<std::size_t>(patched.size(), 256);
    for (int m = 0, n = 1 + static_cast<int>(rng() % 4); m < n; m++)
      patched[rng() % limit] = static_cast<char>(rng() & 0xff);
    spit(bad, patched);

    try
    {
      uvfs::reader r{bad};
      // Touching every payload is what would fault on a bad pointer.
      std::size_t total = 0;
      r.for_each_file(
          [&](uvfs::reader::iter_entry e)
          {
            total += e.path.size();
            if (!e.data.empty())
              total += static_cast<unsigned char>(e.data[0])
                       + static_cast<unsigned char>(e.data[e.data.size() - 1]);
            return true;
          });
      CHECK(total < (std::size_t{1} << 40));
      accepted++;
    }
    catch (const std::exception&)
    {
      rejected++;
    }
  }
  std::printf("    (%d accepted, %d rejected, 0 crashes)\n", accepted, rejected);
  CHECK(accepted + rejected == 4000);
}
