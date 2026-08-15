#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <map>

using namespace uvfs::test;

UVFS_TEST("roundtrip/single_file")
{
  scratch_dir dir{"single"};
  const auto src = dir.make_file("a.bin", 100);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/a.bin", src);
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{1});

  auto got = r.find("/a.bin");
  CHECK(got.has_value());
  if (got)
  {
    CHECK_EQ(got->size(), std::size_t{100});
    CHECK(*got == expected_bytes(100));
  }
}

UVFS_TEST("roundtrip/many_files_contents_match")
{
  scratch_dir dir{"many"};
  std::map<std::string, std::string> want;

  uvfs::writer w;
  for (int i = 0; i < 200; i++)
  {
    // deliberately varied sizes, none a multiple of 64 (see format/alignment)
    const std::size_t n = static_cast<std::size_t>(i * 37 + 1);
    const auto leaf = "sub" + std::to_string(i % 7) + "/f" + std::to_string(i);
    const auto src = dir.make_file(leaf, n, static_cast<uint64_t>(i + 1));
    want["/" + leaf] = expected_bytes(n, static_cast<uint64_t>(i + 1));
    w.add_file("/" + leaf, src);
  }
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), want.size());
  for (auto& [path, content] : want)
  {
    auto got = r.find(path);
    CHECK(got.has_value());
    if (got)
      CHECK(*got == content);
  }
}

UVFS_TEST("roundtrip/missing_key_returns_nullopt")
{
  scratch_dir dir{"missing"};
  const auto src = dir.make_file("a.bin", 10);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/a.bin", src);
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK(!r.find("/nope").has_value());
  CHECK(!r.find("").has_value());
  CHECK(!r.find("/a.bin/deeper").has_value());
}

UVFS_TEST("roundtrip/for_each_file_visits_everything")
{
  scratch_dir dir{"iterate"};
  uvfs::writer w;
  for (int i = 0; i < 32; i++)
  {
    const auto leaf = "f" + std::to_string(i);
    w.add_file("/" + leaf, dir.make_file(leaf, 17 + i, i + 1));
  }
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  int seen = 0;
  r.for_each_file(
      [&](uvfs::reader::iter_entry e)
      {
        seen++;
        CHECK(!e.path.empty());
        auto direct = r.find(e.path);
        CHECK(direct.has_value());
        if (direct)
          CHECK(direct->data() == e.data.data());
        return true;
      });
  CHECK_EQ(seen, 32);
}

UVFS_TEST("roundtrip/for_each_file_can_stop_early")
{
  scratch_dir dir{"stop"};
  uvfs::writer w;
  for (int i = 0; i < 10; i++)
    w.add_file("/f" + std::to_string(i), dir.make_file("f" + std::to_string(i), 8));
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  int seen = 0;
  r.for_each_file(
      [&](uvfs::reader::iter_entry)
      {
        seen++;
        return seen < 3;
      });
  CHECK_EQ(seen, 3);
}

UVFS_TEST("roundtrip/large_file")
{
  scratch_dir dir{"large"};
  const std::size_t n = 5u * 1024 * 1024 + 7;
  const auto src = dir.make_file("big.bin", n, 99);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/big.bin", src);
  w.commit(arc);

  uvfs::reader r{arc};
  auto got = r.find("/big.bin");
  CHECK(got.has_value());
  if (got)
  {
    CHECK_EQ(got->size(), n);
    CHECK(*got == expected_bytes(n, 99));
  }
}

UVFS_TEST("roundtrip/payloads_are_64_byte_aligned")
{
  // The format promises 64-byte alignment so payloads can be handed to code
  // with alignment requirements (SIMD, DMA, audio buffers).
  scratch_dir dir{"align"};
  uvfs::writer w;
  for (int i = 0; i < 40; i++)
    w.add_file(
        "/f" + std::to_string(i),
        dir.make_file("f" + std::to_string(i), 1 + static_cast<std::size_t>(i) * 3));
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  r.for_each_file(
      [](uvfs::reader::iter_entry e)
      {
        const auto addr = reinterpret_cast<std::uintptr_t>(e.data.data());
        CHECK_EQ(addr % 64u, std::uintptr_t{0});
        return true;
      });
}

UVFS_TEST("roundtrip/zero_length_file")
{
  scratch_dir dir{"empty-file"};
  const auto a = dir.make_file("empty.bin", 0);
  const auto b = dir.make_file("after.bin", 33);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/empty.bin", a);
  w.add_file("/after.bin", b);
  w.commit(arc);

  uvfs::reader r{arc};
  auto e = r.find("/empty.bin");
  CHECK(e.has_value());
  if (e)
    CHECK_EQ(e->size(), std::size_t{0});
  auto f = r.find("/after.bin");
  CHECK(f.has_value());
  if (f)
    CHECK(*f == expected_bytes(33));
}
