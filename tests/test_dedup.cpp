#include "archive_bytes.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

using namespace uvfs::test;

UVFS_TEST("dedup/same_file_under_two_names_is_stored_once")
{
  scratch_dir dir{"dedup"};
  const auto src = dir.make_file("shared.bin", 100000, 42);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/a", src);
  w.add_file("/b", src);
  w.add_file("/c", src);
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{3});

  const auto want = expected_bytes(100000, 42);
  auto a = r.find("/a");
  auto b = r.find("/b");
  auto c = r.find("/c");
  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(c.has_value());
  if (a && b && c)
  {
    // All three read correctly...
    CHECK(*a == want);
    CHECK(*b == want);
    CHECK(*c == want);
    // ...from the same bytes.
    CHECK(a->data() == b->data());
    CHECK(a->data() == c->data());
  }

  // One payload, not three.
  const auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  CHECK_EQ(h.data_size, int64_t{100000});
  CHECK(std::filesystem::file_size(arc) < 150000u);
}

UVFS_TEST("dedup/hard_links_share_a_payload")
{
  scratch_dir dir{"hardlink"};
  const auto src = dir.make_file("original.bin", 50000, 7);
  const auto link = dir / "linked.bin";
  std::error_code ec;
  std::filesystem::create_hard_link(src, link, ec);
  if (ec)
  {
    std::printf("    (hard links unsupported here, skipping)\n");
    return;
  }

  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/original", src);
  w.add_file("/linked", link);
  w.commit(arc);

  uvfs::reader r{arc};
  const auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  CHECK_EQ(h.data_size, int64_t{50000});

  auto a = r.find("/original");
  auto b = r.find("/linked");
  CHECK(a.has_value());
  CHECK(b.has_value());
  if (a && b)
  {
    CHECK(*a == expected_bytes(50000, 7));
    CHECK(a->data() == b->data());
  }
}

UVFS_TEST("dedup/distinct_files_of_equal_size_are_not_merged")
{
  // Same size, different inodes, different bytes: these must stay separate.
  scratch_dir dir{"nodedup"};
  const auto a = dir.make_file("a.bin", 4096, 1);
  const auto b = dir.make_file("b.bin", 4096, 2);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/a", a);
  w.add_file("/b", b);
  w.commit(arc);

  uvfs::reader r{arc};
  auto ra = r.find("/a");
  auto rb = r.find("/b");
  CHECK(ra.has_value());
  CHECK(rb.has_value());
  if (ra && rb)
  {
    CHECK(*ra == expected_bytes(4096, 1));
    CHECK(*rb == expected_bytes(4096, 2));
    CHECK(ra->data() != rb->data());
  }
}

UVFS_TEST("dedup/content_hashes_and_verify_still_work")
{
  scratch_dir dir{"dedup-hash"};
  const auto src = dir.make_file("shared.bin", 30000, 9);
  const auto other = dir.make_file("other.bin", 1000, 10);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/x", src);
  w.add_file("/y", src);
  w.add_file("/z", other);
  w.commit(arc);

  uvfs::reader r{arc, uvfs::integrity::index};
  CHECK(r.has_content_hashes());
  CHECK(r.verify().empty());

  // Damaging the shared payload must be reported for both names that use it.
  auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  bytes[static_cast<std::size_t>(h.data_start + 2)] ^= 0x55;
  const auto bad = dir / "bad.uvfs";
  spit(bad, bytes);

  uvfs::reader br{bad};
  CHECK_EQ(br.verify().size(), std::size_t{2});
}

#if defined(UVFS_HAS_ZSTD)
UVFS_TEST("dedup/works_with_compression")
{
  scratch_dir dir{"dedup-zstd"};
  std::string text;
  while (text.size() < 80000)
    text += "the quick brown fox jumps over the lazy dog. ";
  const auto src = dir.make_text("shared.txt", text);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  for (int i = 0; i < 20; i++)
    w.add_file("/copy" + std::to_string(i), src);
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{20});
  for (int i = 0; i < 20; i++)
  {
    auto got = r.read("/copy" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(std::string(got->begin(), got->end()) == text);
  }
  // 20 names, one compressed payload: far below even a single raw copy.
  CHECK(std::filesystem::file_size(arc) < text.size() / 2);
}
#endif
