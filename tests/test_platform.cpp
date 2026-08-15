#include "framework.hpp"
#include "platform.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace uvfs::test;
namespace pf = uvfs::platform;

namespace
{
auto contents_of(const std::string& path) -> std::string
{
  auto f = pf::file::open_read(path.c_str());
  std::string out(static_cast<std::size_t>(f.size()), '\0');
  const auto n = f.read_at(out.data(), static_cast<int64_t>(out.size()), 0);
  out.resize(static_cast<std::size_t>(n));
  return out;
}
} // namespace

UVFS_TEST("platform/backend_reports_itself")
{
  // Not an assertion about which backend, just that one was selected and says
  // so. A build that silently fell through to nothing would show up here.
  std::printf(
      "    (backend=%s, shared-writable-mapping=%s, kernel-copy=%s, page=%lld)\n",
      pf::backend_name,
      pf::has_shared_writable_mapping ? "yes" : "no",
      pf::has_kernel_copy ? "yes" : "no",
      static_cast<long long>(pf::page_size()));
  CHECK(pf::backend_name != nullptr);
  CHECK(pf::page_size() >= 512);
  CHECK(pf::process_id() != 0);
}

UVFS_TEST("platform/open_and_size")
{
  scratch_dir dir{"pf-open"};
  const auto p = dir.make_file("a.bin", 1234, 5);
  auto f = pf::file::open_read(p.c_str());
  CHECK(f.valid());
  CHECK_EQ(f.size(), int64_t{1234});

  // A file that is not there must be reported, not returned as an empty one.
  CHECK_THROWS((void)pf::file::open_read((dir / "missing.bin").c_str()));
}

UVFS_TEST("platform/positioned_read_covers_the_whole_file")
{
  scratch_dir dir{"pf-read"};
  const std::size_t n = 100000;
  const auto p = dir.make_file("a.bin", n, 11);
  const auto want = expected_bytes(n, 11);

  auto f = pf::file::open_read(p.c_str());

  // Whole file in one call.
  std::string all(n, '\0');
  CHECK_EQ(f.read_at(all.data(), static_cast<int64_t>(n), 0), static_cast<int64_t>(n));
  CHECK(all == want);

  // Every offset and length combination that matters at the boundaries.
  const int64_t offsets[] = {0, 1, 4095, 4096, 4097, 65535, 99999};
  for (auto off : offsets)
  {
    const int64_t len = std::min<int64_t>(4096, static_cast<int64_t>(n) - off);
    std::string part(static_cast<std::size_t>(len), '\0');
    CHECK_EQ(f.read_at(part.data(), len, off), len);
    CHECK(
        part
        == want.substr(static_cast<std::size_t>(off), static_cast<std::size_t>(len)));
  }

  // Reading past the end returns what is there, not an error.
  std::string tail(4096, '\0');
  const auto got = f.read_at(tail.data(), 4096, static_cast<int64_t>(n) - 10);
  CHECK_EQ(got, int64_t{10});
}

UVFS_TEST("platform/positioned_write_and_resize")
{
  scratch_dir dir{"pf-write"};
  const auto p = dir / "out.bin";
  {
    auto f = pf::file::create_write(p.c_str());
    f.resize(4096);
    CHECK_EQ(f.size(), int64_t{4096});

    const std::string a(100, 'A');
    const std::string b(100, 'B');
    f.write_at(a.data(), 100, 0);
    f.write_at(b.data(), 100, 2000);
    f.flush();
  }
  const auto got = contents_of(p);
  CHECK_EQ(got.size(), std::size_t{4096});
  CHECK(got.substr(0, 100) == std::string(100, 'A'));
  CHECK(got.substr(2000, 100) == std::string(100, 'B'));
  CHECK(got.substr(100, 100) == std::string(100, '\0'));

  // Shrinking must actually shorten the file.
  {
    auto f = pf::file::create_write(p.c_str());
    f.resize(4096);
    f.resize(64);
    CHECK_EQ(f.size(), int64_t{64});
  }
  CHECK_EQ(std::filesystem::file_size(p), std::uintmax_t{64});
}

UVFS_TEST("platform/reserve_reports_honestly")
{
  scratch_dir dir{"pf-reserve"};
  const auto p = dir / "res.bin";
  auto f = pf::file::create_write(p.c_str());
  f.resize(1 << 20);
  // Either the platform reserves, or it says it cannot. Both are fine; what is
  // not fine is claiming success without the space, which is what the previous
  // implementation did by discarding posix_fallocate's return value.
  const bool ok = f.reserve(0, 1 << 20);
  std::printf("    (reserve supported: %s)\n", ok ? "yes" : "no");
  CHECK(f.size() >= (1 << 20));
}

UVFS_TEST("platform/read_only_mapping_sees_the_file")
{
  scratch_dir dir{"pf-map"};
  const std::size_t n = 70000;
  const auto p = dir.make_file("m.bin", n, 3);
  const auto want = expected_bytes(n, 3);

  auto f = pf::file::open_read(p.c_str());
  auto m = pf::mapping::read_only(f, static_cast<int64_t>(n));
  CHECK_EQ(m.size(), static_cast<int64_t>(n));
  CHECK(m.data() != nullptr);
  if (m.data())
    CHECK(std::string(m.data(), n) == want);

  m.advise_random();
  m.advise_sequential();

  // A zero-length mapping is legal and yields nothing to read.
  auto empty = pf::mapping::read_only(f, 0);
  CHECK_EQ(empty.size(), int64_t{0});
}

UVFS_TEST("platform/writable_mapping_reaches_the_file")
{
  if constexpr (!pf::has_shared_writable_mapping)
  {
    std::printf("    (no shared writable mapping on this backend; skipping)\n");
    return;
  }
  else
  {
    scratch_dir dir{"pf-mapw"};
    const auto p = dir / "w.bin";
    const int64_t n = 40000;
    {
      auto f = pf::file::create_write(p.c_str());
      f.resize(n);
      auto m = pf::mapping::read_write(f, n);
      CHECK(m.data() != nullptr);
      if (m.data())
      {
        std::memset(m.data(), 'Z', static_cast<std::size_t>(n));
        std::memcpy(m.data(), "header", 6);
      }
      m.flush(n);
      m.reset();
      f.flush();
    }
    const auto got = contents_of(p);
    CHECK_EQ(got.size(), static_cast<std::size_t>(n));
    CHECK(got.substr(0, 6) == "header");
    CHECK(got.back() == 'Z');
  }
}

UVFS_TEST("platform/copy_into_matches_the_source")
{
  scratch_dir dir{"pf-copy"};
  // Sizes on both sides of the kernel-copy threshold, which is where the
  // backends diverge most.
  const std::size_t sizes[] = {0, 1, 4096, 262143, 262144, 262145, 1u << 20};
  int i = 0;
  for (auto n : sizes)
  {
    const auto src_path = dir.make_file("src" + std::to_string(i), n, 100 + i);
    const auto dst_path = dir / ("dst" + std::to_string(i));
    const auto want = expected_bytes(n, 100 + i);
    const int64_t offset = 512; // not at the start, to catch offset mistakes

    {
      auto src = pf::file::open_read(src_path.c_str());
      auto dst = pf::file::create_write(dst_path.c_str());
      dst.resize(offset + static_cast<int64_t>(n));
      if constexpr (pf::has_shared_writable_mapping)
      {
        auto m = pf::mapping::read_write(dst, offset + static_cast<int64_t>(n));
        pf::copy_file_into(
            src,
            dst,
            offset,
            m.data() ? m.data() + offset : nullptr,
            static_cast<int64_t>(n));
        m.flush(offset + static_cast<int64_t>(n));
      }
      else
      {
        pf::copy_file_into(src, dst, offset, nullptr, static_cast<int64_t>(n));
      }
      dst.flush();
    }

    const auto got = contents_of(dst_path);
    CHECK_EQ(got.size(), static_cast<std::size_t>(offset) + n);
    CHECK(got.substr(static_cast<std::size_t>(offset)) == want);
    i++;
  }
}

UVFS_TEST("platform/copy_into_without_a_mapping")
{
  // The path a backend takes when it has no writable mapping to read into.
  // Exercised on every platform so it cannot rot on the ones that do.
  scratch_dir dir{"pf-copy-nomap"};
  const std::size_t n = 300000;
  const auto src_path = dir.make_file("s.bin", n, 42);
  const auto dst_path = dir / "d.bin";
  {
    auto src = pf::file::open_read(src_path.c_str());
    auto dst = pf::file::create_write(dst_path.c_str());
    dst.resize(static_cast<int64_t>(n) + 128);
    pf::copy_file_into(src, dst, 128, nullptr, static_cast<int64_t>(n));
    dst.flush();
  }
  const auto got = contents_of(dst_path);
  CHECK(got.substr(128) == expected_bytes(n, 42));
}

UVFS_TEST("platform/stat_identifies_the_file")
{
  scratch_dir dir{"pf-stat"};
  const auto p = dir.make_file("a.bin", 777, 1);

  const auto s = pf::stat_path(p.c_str());
  CHECK(s.exists);
  CHECK(s.regular);
  CHECK_EQ(s.size, int64_t{777});

  // Two names for the same file must agree on identity, which is what payload
  // sharing depends on.
  const auto link = dir / "link.bin";
  std::error_code ec;
  std::filesystem::create_hard_link(p, link, ec);
  if (!ec)
  {
    const auto t = pf::stat_path(link.c_str());
    CHECK(t.exists);
    CHECK_EQ(t.device, s.device);
    CHECK_EQ(t.inode, s.inode);
  }

  // A different file must not collide.
  const auto other = dir.make_file("b.bin", 777, 2);
  const auto o = pf::stat_path(other.c_str());
  CHECK(o.inode != s.inode || o.device != s.device);

  // Directories are not regular files.
  const auto sub = dir.path / "sub";
  std::filesystem::create_directories(sub);
  const auto d = pf::stat_path(sub.string().c_str());
  CHECK(d.exists);
  CHECK(!d.regular);

  // Absent paths report absent rather than throwing.
  const auto missing = pf::stat_path((dir / "nope").c_str());
  CHECK(!missing.exists);
}

UVFS_TEST("platform/readable_reflects_permissions")
{
  scratch_dir dir{"pf-perm"};
  const auto p = dir.make_file("a.bin", 10, 1);
  CHECK(pf::readable(p.c_str()));
  CHECK(!pf::readable((dir / "nope").c_str()));
}

UVFS_TEST("platform/rename_replaces_atomically")
{
  scratch_dir dir{"pf-rename"};
  const auto a = dir.make_file("new.bin", 200, 1);
  const auto b = dir.make_file("existing.bin", 50, 2);

  // POSIX rename overwrites; Windows' rename does not, which is why the
  // platform layer has its own call rather than using std::rename.
  CHECK(pf::rename_replace(a.c_str(), b.c_str()));
  CHECK(!std::filesystem::exists(a));
  CHECK_EQ(std::filesystem::file_size(b), std::uintmax_t{200});
  CHECK(contents_of(b) == expected_bytes(200, 1));

  // Renaming something that is not there fails rather than inventing a file.
  CHECK(!pf::rename_replace((dir / "ghost").c_str(), (dir / "target").c_str()));
}

UVFS_TEST("platform/remove_is_quiet_about_missing_files")
{
  scratch_dir dir{"pf-remove"};
  const auto p = dir.make_file("a.bin", 10, 1);
  pf::remove_quietly(p.c_str());
  CHECK(!std::filesystem::exists(p));
  // Removing it again must not throw or crash.
  pf::remove_quietly(p.c_str());
  pf::remove_quietly((dir / "never-existed").c_str());
}

UVFS_TEST("platform/errors_have_readable_text")
{
  const auto s = pf::last_error();
  CHECK(!s.empty());
  bool threw = false;
  std::string msg;
  try
  {
    (void)pf::file::open_read("/definitely/not/a/real/path/uvfs");
  }
  catch (const std::exception& e)
  {
    threw = true;
    msg = e.what();
  }
  CHECK(threw);
  // The message must say something more useful than a bare number.
  CHECK(msg.size() > 20);
  CHECK(msg.find("could not open") != std::string::npos);
}
