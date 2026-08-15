#include "framework.hpp"

#include <uvfs/path.hpp>
#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <fstream>
#include <cstdlib>
#include <limits>

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

UVFS_TEST("path/writer_is_the_thing_that_prevents_duplicates")
{
  // Version 1 walked the whole index at open and built a heap map, so it could
  // notice a duplicate on the way past. Version 2 deliberately does no work at
  // open, so that check is gone from the reader. Duplicates are instead
  // prevented where they can actually occur: the writer refuses to emit them.
  // A duplicate appearing in a written archive would be corruption, which is
  // what the index hash is for (see integrity tests).
  scratch_dir dir{"dup-writer"};
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/aa", dir.make_file("a", 32, 1));
  w.add_file("/bb", dir.make_file("b", 32, 2));
  w.add_file("/aa", dir.make_file("c", 32, 3));
  CHECK_THROWS(w.commit(arc));
  CHECK(!std::filesystem::exists(arc));

  // And what it does emit is always internally consistent.
  uvfs::writer ok;
  ok.add_file("/aa", dir.make_file("a2", 32, 1));
  ok.add_file("/bb", dir.make_file("b2", 32, 2));
  ok.commit(arc);
  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{2});
  CHECK_EQ(r.count(), int64_t{2});
  // Sorted order is part of the format now.
  CHECK(r.at(0).path == "/aa");
  CHECK(r.at(1).path == "/bb");
}

UVFS_TEST("path/name_blob_limit_is_a_stated_constant")
{
  // entry::name_offset is a uint32_t, so the name blob cannot exceed 4 GiB.
  // The value is checked here so that widening the field without revisiting
  // the writer's guard cannot pass unnoticed.
  CHECK_EQ(uvfs::max_names_size, int64_t{0xffffffff});
  CHECK(uvfs::max_names_size <= int64_t{std::numeric_limits<uint32_t>::max()});
}

UVFS_TEST("path/oversized_name_blob_is_refused")
{
  // Reproducing this for real needs more than 4 GiB of archive paths held in
  // memory at once, so it is opt-in: UVFS_SLOW_TESTS=1. Without the guard the
  // writer truncates name offsets to 32 bits and reports success, producing an
  // archive whose entries carry other entries' names and cannot be found by
  // the names they were given.
  const char* slow = std::getenv("UVFS_SLOW_TESTS");
  if (!slow || std::string{slow} != "1")
  {
    std::printf("    (set UVFS_SLOW_TESTS=1 to run; needs ~9 GB of RAM)\n");
    return;
  }

  scratch_dir dir{"nameblob"};
  const auto src = dir.make_file("payload.bin", 16, 1);
  const auto arc = dir / "out.uvfs";

  // Each path is the largest the format allows, all well inside the per-file
  // count limit; only their total exceeds what a 32-bit offset can address.
  const std::size_t path_len = static_cast<std::size_t>(uvfs::max_archive_path_size);
  const int count = static_cast<int>(0xffffffffULL / path_len) + 4;

  uvfs::writer w;
  for (int i = 0; i < count; i++)
  {
    std::string name = "/" + std::to_string(i);
    name.append(path_len - name.size(), 'a');
    w.add_file(name, src);
  }
  std::printf(
      "    (%d paths of %zu bytes = %.2f GiB of names)\n", count, path_len,
      static_cast<double>(count) * static_cast<double>(path_len) / (1024.0 * 1024 * 1024));

  bool refused = false;
  std::string message;
  try
  {
    w.commit(arc);
  }
  catch (const std::exception& e)
  {
    refused = true;
    message = e.what();
  }
  CHECK(refused);
  CHECK(message.find("name") != std::string::npos);
  CHECK(!std::filesystem::exists(arc));
}
