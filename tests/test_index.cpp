#include "archive_bytes.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace uvfs::test;

UVFS_TEST("index/entries_are_sorted_by_path")
{
  scratch_dir dir{"sorted"};
  // Register in an order that is neither sorted nor reverse sorted.
  const char* names[] = {"/zebra", "/apple", "/m/n", "/a", "/m", "/zz", "/b/c/d"};
  uvfs::writer w;
  int i = 0;
  for (auto* n : names)
    w.add_file(n, dir.make_file("f" + std::to_string(i++), 16, 1));
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  std::vector<std::string> got;
  for (int64_t k = 0; k < r.count(); k++)
    got.emplace_back(r.at(k).path);

  auto want = got;
  std::sort(want.begin(), want.end());
  CHECK(got == want);

  // for_each_file must agree with at().
  std::vector<std::string> iterated;
  r.for_each_file(
      [&](uvfs::reader::iter_entry e)
      {
        iterated.emplace_back(e.path);
        return true;
      });
  CHECK(iterated == want);
}

UVFS_TEST("index/lookup_survives_hash_collisions_and_probing")
{
  // Enough entries to force the open-addressed table well past a single probe,
  // with names that share long prefixes so a prefix-only comparison would fail.
  scratch_dir dir{"probe"};
  uvfs::writer w;
  const int n = 5000;
  const auto shared = dir.make_file("shared.bin", 8, 42);
  std::vector<std::string> names;
  names.reserve(n);
  for (int i = 0; i < n; i++)
  {
    names.push_back(
        "/very/long/shared/prefix/that/repeats/entry-" + std::to_string(i));
    w.add_file(names.back(), shared);
  }
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.count(), static_cast<int64_t>(n));
  for (auto& name : names)
    CHECK(r.find(name).has_value());

  // Near misses must not resolve.
  CHECK(!r.find("/very/long/shared/prefix/that/repeats/entry-").has_value());
  CHECK(!r.find("/very/long/shared/prefix/that/repeats/entry-99999").has_value());
  CHECK(!r.find("/very/long/shared/prefix/that/repeats/entry-0x").has_value());
  CHECK(!r.find("/very/long/shared/prefix").has_value());
}

UVFS_TEST("index/lookup_of_absent_key_terminates")
{
  // Linear probing must not spin when the table is full-ish and the key is
  // absent; the empty-slot sentinel is what stops it.
  scratch_dir dir{"absent"};
  uvfs::writer w;
  const auto src = dir.make_file("s", 4, 1);
  for (int i = 0; i < 700; i++)
    w.add_file("/k" + std::to_string(i), src);
  const auto arc = dir / "out.uvfs";
  w.commit(arc);

  uvfs::reader r{arc};
  for (int i = 0; i < 2000; i++)
    CHECK(!r.find("/absent" + std::to_string(i)).has_value());
}

UVFS_TEST("index/table_capacity_matches_load_factor")
{
  for (int64_t n : {int64_t{0}, int64_t{1}, int64_t{5}, int64_t{6}, int64_t{100},
                    int64_t{1000}})
  {
    const auto cap = uvfs::table_capacity_for(n);
    if (n == 0)
    {
      CHECK_EQ(cap, int64_t{0});
      continue;
    }
    // Power of two, and never more than 70% full.
    CHECK((cap & (cap - 1)) == 0);
    CHECK(cap * 7 >= n * 10);
    // ...but not needlessly large, once past the minimum table size.
    if (cap > 8)
      CHECK((cap / 2) * 7 < n * 10);
  }
}

UVFS_TEST("index/stat_reports_storage_and_sizes")
{
  scratch_dir dir{"stat"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/a", dir.make_file("a", 1234, 7));
  w.commit(arc);

  uvfs::reader r{arc};
  auto s = r.stat("/a");
  CHECK(s.has_value());
  if (s)
  {
    CHECK(s->path == "/a");
    CHECK_EQ(s->size, int64_t{1234});
    CHECK_EQ(s->stored_size, int64_t{1234});
    CHECK(s->storage == uvfs::stored_as::raw);
  }
  CHECK(!r.stat("/nope").has_value());
}

UVFS_TEST("index/read_matches_find_for_stored_entries")
{
  scratch_dir dir{"read"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  for (int i = 0; i < 20; i++)
    w.add_file(
        "/f" + std::to_string(i),
        dir.make_file("f" + std::to_string(i), 100u + static_cast<unsigned>(i), i + 1));
  w.commit(arc);

  uvfs::reader r{arc};
  for (int i = 0; i < 20; i++)
  {
    const auto key = "/f" + std::to_string(i);
    auto mapped = r.find(key);
    auto copied = r.read(key);
    CHECK(mapped.has_value());
    CHECK(copied.has_value());
    if (mapped && copied)
      CHECK(std::string_view(copied->data(), copied->size()) == *mapped);
  }
  CHECK(!r.read("/absent").has_value());
}

UVFS_TEST("index/read_into_rejects_short_buffers")
{
  scratch_dir dir{"readinto"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/a", dir.make_file("a", 500, 3));
  w.commit(arc);

  uvfs::reader r{arc};
  std::vector<char> small(499);
  CHECK_THROWS((void)r.read_into("/a", small.data(), 499));

  std::vector<char> exact(500);
  auto n = r.read_into("/a", exact.data(), 500);
  CHECK(n.has_value());
  if (n)
    CHECK_EQ(*n, int64_t{500});
  CHECK(std::string_view(exact.data(), exact.size()) == expected_bytes(500, 3));
}

UVFS_TEST("index/at_rejects_out_of_range")
{
  scratch_dir dir{"range"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/a", dir.make_file("a", 8));
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_NOTHROW((void)r.at(0));
  CHECK_THROWS((void)r.at(-1));
  CHECK_THROWS((void)r.at(1));
  CHECK_THROWS((void)r.at(1 << 30));
}

UVFS_TEST("index/reader_is_movable")
{
  scratch_dir dir{"move"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/a", dir.make_file("a", 64, 11));
  w.commit(arc);

  auto make = [&] { return uvfs::reader{arc}; };
  uvfs::reader r = make();
  CHECK_EQ(r.size(), std::size_t{1});

  uvfs::reader moved = std::move(r);
  auto got = moved.find("/a");
  CHECK(got.has_value());
  if (got)
    CHECK(*got == expected_bytes(64, 11));

  // Movable means it can live in a container.
  std::vector<uvfs::reader> readers;
  readers.push_back(make());
  readers.push_back(make());
  CHECK_EQ(readers.size(), std::size_t{2});
  CHECK(readers[1].find("/a").has_value());
}

UVFS_TEST("index/open_cost_does_not_scale_with_entry_count")
{
  // The point of v2: the index is used in place, so opening a big archive
  // costs the same as opening a small one. Comparing a large archive against a
  // small one isolates that from the fixed cost of a mapping, which is what an
  // absolute threshold would be at the mercy of.
  if (!have_current_rss())
  {
    std::printf("    (no /proc, cannot measure current RSS here; skipping)\n");
    return;
  }
  scratch_dir dir{"noalloc"};
  const auto src = dir.make_file("s", 8, 1);

  auto build = [&](int n, const char* name)
  {
    uvfs::writer w;
    for (int i = 0; i < n; i++)
      w.add_file("/entry/number/" + std::to_string(i), src);
    const auto p = dir / name;
    w.commit(p);
    return p;
  };
  const auto small = build(100, "small.uvfs");
  const auto large = build(50000, "large.uvfs");

  auto open_cost = [](const std::string& p)
  {
    const auto before = current_rss_kb();
    uvfs::reader r{p};
    const auto n = r.count();
    const auto after = current_rss_kb();
    return std::pair<long, int64_t>{after - before, n};
  };

  const auto [small_cost, small_n] = open_cost(small);
  const auto [large_cost, large_n] = open_cost(large);
  CHECK_EQ(small_n, int64_t{100});
  CHECK_EQ(large_n, int64_t{50000});

  std::printf(
      "    (open cost: %ld KB for 100 entries, %ld KB for 50000)\n", small_cost,
      large_cost);

  // v1 built a heap hash map here: 50000 entries at ~40 bytes plus table
  // overhead is several megabytes, per process, and it would grow with n.
  // v2 touches the header page and stops, so the two must be within noise.
  CHECK(large_cost - small_cost < 256);

  // The reader is fully usable afterwards despite having done no work.
  uvfs::reader r{large};
  for (int i = 0; i < 100; i++)
    CHECK(r.find("/entry/number/" + std::to_string(i * 499)).has_value());
}
