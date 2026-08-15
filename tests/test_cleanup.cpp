#include "framework.hpp"
#include "scope_guard.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace uvfs::test;

namespace
{
auto temp_files_in(const std::filesystem::path& dir) -> int
{
  int n = 0;
  std::error_code ec;
  for (auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.path().filename().string().rfind(".uvfs-tmp-", 0) == 0)
      n++;
  return n;
}
} // namespace

UVFS_TEST("cleanup/guard_runs_on_every_exception_type")
{
  // The whole point is that cleanup does not depend on someone having listed
  // the right exception type in a catch clause. commit() caught commit_error
  // and std::runtime_error; std::bad_alloc and std::length_error walked
  // straight past both.
  struct oddball
  {
  };

  int ran = 0;
  const auto body = [&](int which)
  {
    uvfs::scope_guard guard{[&] { ran++; }};
    switch (which)
    {
      case 0: throw std::runtime_error{"runtime"};
      case 1: throw std::bad_alloc{};
      case 2: throw std::length_error{"length"};
      case 3: throw std::invalid_argument{"invalid"};
      case 4: throw oddball{};
      default: break;
    }
  };

  for (int which = 0; which < 5; which++)
  {
    try
    {
      body(which);
    }
    catch (...)
    {
    }
  }
  CHECK_EQ(ran, 5);

  // ...and on an ordinary return.
  body(99);
  CHECK_EQ(ran, 6);

  // Dismissing suppresses it, which is how success is signalled.
  {
    uvfs::scope_guard guard{[&] { ran++; }};
    guard.dismiss();
  }
  CHECK_EQ(ran, 6);
}

UVFS_TEST("cleanup/failed_rename_leaves_nothing_behind")
{
  // The output path is a directory, so everything succeeds until rename.
  scratch_dir dir{"renamefail"};
  std::filesystem::create_directories(dir.path / "occupied");
  const auto src = dir.make_file("a.bin", 4096, 1);

  uvfs::writer w;
  w.add_file("/a", src);
  CHECK_THROWS(w.commit((dir.path / "occupied").string()));
  CHECK_EQ(temp_files_in(dir.path), 0);
}

UVFS_TEST("cleanup/failed_commit_leaves_nothing_behind")
{
  scratch_dir dir{"copyfail"};
  const auto keep = dir.make_file("keep.bin", 8192, 1);
  const auto gone = dir.make_file("gone.bin", 8192, 2);

  uvfs::writer w;
  w.add_file("/keep", keep);
  w.add_file("/gone", gone);
  std::filesystem::remove(gone);
  CHECK_THROWS(w.commit((dir.path / "out.uvfs").string()));
  CHECK_EQ(temp_files_in(dir.path), 0);
  CHECK(!std::filesystem::exists(dir.path / "out.uvfs"));
}

UVFS_TEST("cleanup/successful_commit_leaves_nothing_behind")
{
  scratch_dir dir{"okcommit"};
  uvfs::writer w;
  for (int i = 0; i < 20; i++)
    w.add_file("/f" + std::to_string(i), dir.make_file("f" + std::to_string(i), 512, i));
  const auto arc = (dir.path / "out.uvfs").string();
  w.commit(arc);
  CHECK_EQ(temp_files_in(dir.path), 0);
  CHECK(std::filesystem::exists(arc));
  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{20});
}

#if defined(UVFS_HAS_ZSTD)
UVFS_TEST("cleanup/failed_compressed_commit_leaves_nothing_behind")
{
  scratch_dir dir{"zcopyfail"};
  std::string text;
  while (text.size() < 20000)
    text += "padding padding padding ";
  const auto keep = dir.make_text("keep.txt", text);
  const auto gone = dir.make_text("gone.txt", text);

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  w.add_file("/keep", keep);
  w.add_file("/gone", gone);
  std::filesystem::remove(gone);
  CHECK_THROWS(w.commit((dir.path / "out.uvfs").string()));
  CHECK_EQ(temp_files_in(dir.path), 0);
}
#endif
