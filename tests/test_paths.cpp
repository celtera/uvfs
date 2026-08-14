#include "framework.hpp"

#include <uvfs/path.hpp>
#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <fstream>

using namespace uvfs::test;

UVFS_TEST("path/accepts_ordinary_names")
{
  for (std::string_view p :
       {"/a", "a", "/a/b/c.txt", "a/b", "/deep/ly/nes/ted/file.bin",
        "/name with spaces", "/naïve-utf8-é", "/a..b", "/..a", "/a..",
        "/...", "/file.tar.gz"})
    CHECK(uvfs::is_safe_archive_path(p));
}

UVFS_TEST("path/rejects_traversal_and_malformed")
{
  struct { std::string_view path; uvfs::path_problem want; } cases[] = {
      {"", uvfs::path_problem::empty},
      {"/", uvfs::path_problem::empty_component},
      {"//a", uvfs::path_problem::empty_component},
      {"a//b", uvfs::path_problem::empty_component},
      {"a/", uvfs::path_problem::empty_component},
      {"/a/", uvfs::path_problem::empty_component},
      {"..", uvfs::path_problem::dotdot_component},
      {"/..", uvfs::path_problem::dotdot_component},
      {"../etc/passwd", uvfs::path_problem::dotdot_component},
      {"/a/../../etc/passwd", uvfs::path_problem::dotdot_component},
      {"a/b/..", uvfs::path_problem::dotdot_component},
      {".", uvfs::path_problem::dot_component},
      {"/./a", uvfs::path_problem::dot_component},
      {"a/./b", uvfs::path_problem::dot_component},
      {"a\\b", uvfs::path_problem::contains_backslash},
      {"..\\..\\windows", uvfs::path_problem::contains_backslash},
      {std::string_view{"a\0b", 3}, uvfs::path_problem::contains_nul},
  };
  for (auto& c : cases)
  {
    const auto got = uvfs::check_archive_path(c.path);
    CHECK_EQ(static_cast<int>(got), static_cast<int>(c.want));
  }
}

UVFS_TEST("path/rejects_over_long")
{
  const std::string big(uvfs::max_archive_path_size + 1, 'x');
  CHECK_EQ(
      static_cast<int>(uvfs::check_archive_path("/" + big)),
      static_cast<int>(uvfs::path_problem::too_long));
  const std::string ok(uvfs::max_archive_path_size - 1, 'x');
  CHECK(uvfs::is_safe_archive_path("/" + ok));
}

UVFS_TEST("path/writer_rejects_unsafe_paths")
{
  scratch_dir dir{"unsafe"};
  const auto src = dir.make_file("a.bin", 10);

  for (std::string_view bad : {"", "..", "/../escape", "/a/./b", "a//b", "x\\y"})
  {
    uvfs::writer w;
    bool threw = false;
    try
    {
      w.add_file(bad, src);
    }
    catch (const std::invalid_argument&)
    {
      threw = true;
    }
    CHECK(threw);
  }
}

UVFS_TEST("path/duplicate_archive_paths_are_rejected")
{
  scratch_dir dir{"dup"};
  const auto a = dir.make_file("a.bin", 100, 1);
  const auto b = dir.make_file("b.bin", 200, 2);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/same", a);
  w.add_file("/same", b);
  CHECK_THROWS(w.commit(arc));
  CHECK(!std::filesystem::exists(arc));
}

UVFS_TEST("path/duplicate_archive_paths_can_be_replaced")
{
  scratch_dir dir{"dup-replace"};
  const auto a = dir.make_file("a.bin", 100, 1);
  const auto b = dir.make_file("b.bin", 200, 2);
  const auto c = dir.make_file("c.bin", 300, 3);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.set_duplicate_policy(uvfs::on_duplicate::replace);
  w.add_file("/same", a);
  w.add_file("/other", c);
  w.add_file("/same", b); // wins
  w.commit(arc);

  uvfs::reader r{arc};
  // The header count and what is reachable must agree.
  CHECK_EQ(r.size(), std::size_t{2});
  auto got = r.find("/same");
  CHECK(got.has_value());
  if (got)
  {
    CHECK_EQ(got->size(), std::size_t{200});
    CHECK(*got == expected_bytes(200, 2));
  }
  auto other = r.find("/other");
  CHECK(other.has_value());
  if (other)
    CHECK(*other == expected_bytes(300, 3));

  // No orphaned payload: with one 200-byte and one 300-byte file the data
  // region holds exactly those two, padded to 64 bytes each.
  int seen = 0;
  r.for_each_file(
      [&](uvfs::reader::iter_entry)
      {
        seen++;
        return true;
      });
  CHECK_EQ(seen, 2);
}

UVFS_TEST("path/replace_keeps_registration_order_stable")
{
  scratch_dir dir{"dup-order"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.set_duplicate_policy(uvfs::on_duplicate::replace);
  for (int i = 0; i < 50; i++)
    w.add_file("/f" + std::to_string(i), dir.make_file("f" + std::to_string(i), 8, i + 1));
  // re-register a few, last one wins
  for (int i : {3, 17, 42})
    w.add_file("/f" + std::to_string(i), dir.make_file("z" + std::to_string(i), 24, 900 + i));
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{50});
  for (int i : {3, 17, 42})
  {
    auto got = r.find("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(*got == expected_bytes(24, 900 + i));
  }
  for (int i : {0, 1, 2, 4, 49})
  {
    auto got = r.find("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(*got == expected_bytes(8, i + 1));
  }
}

UVFS_TEST("path/reader_rejects_duplicate_entries_in_index")
{
  // Hand-build the duplicate the writer now refuses to produce, by copying an
  // archive's single index entry over its neighbour.
  scratch_dir dir{"dup-index"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/aa", dir.make_file("a", 32, 1));
  w.add_file("/bb", dir.make_file("b", 32, 2));
  w.commit(arc);

  std::string bytes;
  {
    std::ifstream f(arc, std::ios::binary);
    bytes.assign(
        std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  // Both entries are static_size(20) + 3 path bytes -> rounded to 24 each.
  const std::size_t index_start = 64;
  const std::size_t stride = 24;
  // Make the second entry's path identical to the first's.
  bytes[index_start + stride + 20] = bytes[index_start + 20];
  bytes[index_start + stride + 21] = bytes[index_start + 21];
  bytes[index_start + stride + 22] = bytes[index_start + 22];

  const auto bad = dir / "bad.uvfs";
  {
    std::ofstream f(bad, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  CHECK_THROWS(uvfs::reader{bad});
}
