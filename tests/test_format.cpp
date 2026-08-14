#include "archive_bytes.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <cstdint>
#include <random>
#include <vector>

using namespace uvfs::test;


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

  const auto e0 = entry_offset(bytes, 0);
  // A name that claims to run past the end of the name blob, or to start
  // past it, must never produce a view outside the mapping.
  for (uint16_t evil_size : {uint16_t{0}, uint16_t{0xffff}, uint16_t{4096}})
  {
    auto patched = bytes;
    std::memcpy(patched.data() + e0 + ent::name_size, &evil_size, sizeof evil_size);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK(load_and_read_all(bad).rejected());
  }
  for (uint32_t evil_off : {uint32_t{0xffffffff}, uint32_t{1u << 20}})
  {
    auto patched = bytes;
    std::memcpy(patched.data() + e0 + ent::name_offset, &evil_off, sizeof evil_off);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK(load_and_read_all(bad).rejected());
  }
}

UVFS_TEST("format/rejects_corrupt_entry_offsets")
{
  scratch_dir dir{"offsets"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  const auto e0 = entry_offset(bytes, 0);
  const std::size_t fields[]
      = {ent::data_offset, ent::stored_size, ent::orig_size};
  const int64_t values[] = {
      -1, int64_t{1} << 40, std::numeric_limits<int64_t>::max(),
      std::numeric_limits<int64_t>::min()};
  for (auto f : fields)
    for (auto v : values)
    {
      auto patched = bytes;
      std::memcpy(patched.data() + e0 + f, &v, sizeof v);
      const auto bad = dir / "bad.uvfs";
      spit(bad, patched);
      CHECK(load_and_read_all(bad).rejected());
    }

  // An unknown codec must be refused rather than guessed at.
  {
    auto patched = bytes;
    const uint8_t evil = 99;
    std::memcpy(patched.data() + e0 + ent::method, &evil, sizeof evil);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK(load_and_read_all(bad).rejected());
  }
}

UVFS_TEST("format/rejects_corrupt_header_fields")
{
  scratch_dir dir{"header"};
  const auto arc = build_sample(dir);
  auto bytes = slurp(arc);

  const std::size_t fields[] = {
      hdr::file_size,  hdr::file_count, hdr::index_start,    hdr::index_size,
      hdr::data_start, hdr::data_size,  hdr::table_capacity, hdr::names_size};

  // Values that break a header invariant outright: the header check alone has
  // to refuse these, before a single entry is touched.
  const int64_t impossible[] = {-1, std::numeric_limits<int64_t>::max(),
                                std::numeric_limits<int64_t>::min(), 1LL << 50};
  for (auto off : fields)
    for (auto v : impossible)
    {
      auto patched = bytes;
      std::memcpy(patched.data() + off, &v, sizeof v);
      const auto bad = dir / "bad.uvfs";
      spit(bad, patched);
      CHECK_THROWS(uvfs::reader{bad});
    }

  // A size field shrunk to a small but in-bounds value is *not* something the
  // header check can catch: the regions still fit in the file, there is just
  // slack. It shows up when an entry is used, and the index hash catches it at
  // open. Either way the archive must never be read as if it were intact.
  for (auto off : fields)
  {
    auto patched = bytes;
    const int64_t v = 7;
    std::memcpy(patched.data() + off, &v, sizeof v);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK(load_and_read_all(bad).rejected());
  }

  // Flags this build does not understand mean the archive was written by a
  // newer implementation, so it cannot be read safely.
  {
    auto patched = bytes;
    const uint32_t unknown = 0x8000'0000u;
    std::memcpy(patched.data() + hdr::flags, &unknown, sizeof unknown);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
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
    patched[hdr::version] = 99;
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

  for (std::size_t keep :
       {std::size_t{0}, std::size_t{1}, std::size_t{63}, std::size_t{64},
        std::size_t{127}, std::size_t{128}, bytes.size() / 2, bytes.size() - 1})
  {
    std::vector<char> cut(bytes.begin(), bytes.begin() + static_cast<long>(keep));
    const auto bad = dir / "cut.uvfs";
    spit(bad, cut);
    CHECK_THROWS(uvfs::reader{bad});
  }
}

UVFS_TEST("format/single_byte_corruption_never_crashes")
{
  // The reader must either refuse an archive or read it without ever leaving
  // the mapping. Under ASAN this is the test that proves it; without ASAN it
  // still catches wild pointers that segfault. Note that v2 does not walk the
  // index at open, so a corrupt entry legitimately surfaces on use rather
  // than on open -- both count as handled.
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
    const std::size_t limit = (iter % 4 == 0)
                                  ? patched.size()
                                  : std::min<std::size_t>(patched.size(), 512);
    for (int m = 0, n = 1 + static_cast<int>(rng() % 4); m < n; m++)
      patched[rng() % limit] = static_cast<char>(rng() & 0xff);
    spit(bad, patched);

    const auto r = load_and_read_all(bad);
    if (r.rejected())
      rejected++;
    else
      accepted++;
  }
  std::printf("    (%d read cleanly, %d refused, 0 crashes)\n", accepted, rejected);
  CHECK_EQ(accepted + rejected, 4000);
}
