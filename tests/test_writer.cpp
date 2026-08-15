#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <sys/stat.h>

#include <random>
#include <vector>

using namespace uvfs::test;

// R5: a file removed between add_file() and commit() used to throw inside a
// worker thread, which calls std::terminate. Archiving a live tree -- a build
// directory, a working copy -- hit this routinely.
UVFS_TEST("writer/vanished_file_is_reported_not_fatal")
{
  scratch_dir dir{"vanish"};
  const auto keep = dir.make_file("keep.bin", 100);
  const auto gone = dir.make_file("vanish.bin", 100);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/keep", keep);
  w.add_file("/vanish", gone);
  std::filesystem::remove(gone);

  bool threw = false;
  std::size_t reported = 0;
  try
  {
    w.commit(arc);
  }
  catch (const uvfs::commit_error& e)
  {
    threw = true;
    reported = e.files.size();
  }
  CHECK(threw);
  CHECK_EQ(reported, std::size_t{1});
  // Nothing partial must be left behind.
  CHECK(!std::filesystem::exists(arc));
}

UVFS_TEST("writer/vanished_file_can_be_skipped")
{
  scratch_dir dir{"vanish-skip"};
  const auto keep = dir.make_file("keep.bin", 100);
  const auto gone = dir.make_file("vanish.bin", 100);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.set_unreadable_policy(uvfs::on_unreadable::skip);
  w.add_file("/keep", keep);
  w.add_file("/vanish", gone);
  std::filesystem::remove(gone);

  CHECK_NOTHROW(w.commit(arc));
  CHECK_EQ(w.skipped().size(), std::size_t{1});
  if (!w.skipped().empty())
    CHECK(w.skipped().front().path_in_archive == "/vanish");

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{1});
  CHECK(r.find("/keep").has_value());
  CHECK(!r.find("/vanish").has_value());
}

// R6: an unreadable file used to abort the process the same way.
UVFS_TEST("writer/unreadable_file_is_reported_not_fatal")
{
  scratch_dir dir{"unreadable"};
  const auto ok = dir.make_file("ok.bin", 50);
  const auto secret = dir.make_file("secret.bin", 50);
  std::filesystem::permissions(secret, std::filesystem::perms::none);
  const auto arc = dir / "out.uvfs";

  // Running as root defeats permission checks entirely; skip rather than
  // report a false failure.
  if (::access(secret.c_str(), R_OK) == 0)
  {
    std::printf("    (running as root, permission check not meaningful)\n");
    return;
  }

  uvfs::writer w;
  w.add_file("/ok", ok);
  w.add_file("/secret", secret);
  CHECK_THROWS(w.commit(arc));
  CHECK(!std::filesystem::exists(arc));

  uvfs::writer w2;
  w2.set_unreadable_policy(uvfs::on_unreadable::skip);
  w2.add_file("/ok", ok);
  w2.add_file("/secret", secret);
  CHECK_NOTHROW(w2.commit(arc));
  CHECK_EQ(w2.skipped().size(), std::size_t{1});

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{1});
}

UVFS_TEST("writer/directory_input_is_not_archived")
{
  scratch_dir dir{"dirinput"};
  std::filesystem::create_directories(dir.path / "subdir");
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.set_unreadable_policy(uvfs::on_unreadable::skip);
  w.add_file("/subdir", (dir.path / "subdir").string());
  CHECK_NOTHROW(w.commit(arc));
  CHECK_EQ(w.skipped().size(), std::size_t{1});
  if (!w.skipped().empty())
    CHECK(w.skipped().front().reason.find("regular") != std::string::npos);
}

UVFS_TEST("writer/missing_file_is_not_stored_as_empty")
{
  // stat() used to be called without checking its result, so a missing file
  // silently became a zero-length entry.
  scratch_dir dir{"missing-src"};
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/nope", (dir.path / "does-not-exist").string());
  CHECK_THROWS(w.commit(arc));
}

// R3: add_file() took a string_view but handed .data() to stat(), reading past
// the end of the view whenever it was a prefix of a longer buffer.
UVFS_TEST("writer/string_view_arguments_are_not_over_read")
{
  scratch_dir dir{"sv"};
  const auto real = dir.make_file("real.bin", 100, 5);
  dir.make_file("real.bin.DECOY", 4096, 6);
  const auto arc = dir / "out.uvfs";

  // A view of ".../real.bin" backed by the longer ".../real.bin.DECOY".
  const std::string backing = real + ".DECOY";
  const std::string_view sv{backing.data(), real.size()};
  CHECK_EQ(std::string{sv}, real);

  uvfs::writer w;
  w.add_file("/a", sv);
  CHECK_NOTHROW(w.commit(arc));

  uvfs::reader r{arc};
  auto got = r.find("/a");
  CHECK(got.has_value());
  if (got)
  {
    // 100, not 4096: the view must be honoured exactly.
    CHECK_EQ(got->size(), std::size_t{100});
    CHECK(*got == expected_bytes(100, 5));
  }
}

UVFS_TEST("writer/archive_path_view_is_not_over_read")
{
  scratch_dir dir{"sv2"};
  const auto src = dir.make_file("a.bin", 20);
  const auto arc = dir / "out.uvfs";

  const std::string backing = "/short/and/then/some/more";
  const std::string_view name{backing.data(), 6}; // "/short"

  uvfs::writer w;
  w.add_file(name, src);
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK(r.find("/short").has_value());
  CHECK(!r.find(backing).has_value());
}

UVFS_TEST("writer/commit_is_atomic")
{
  // A failed commit must not damage an archive that is already there.
  scratch_dir dir{"atomic"};
  const auto good = dir.make_file("good.bin", 128, 3);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w1;
  w1.add_file("/good", good);
  w1.commit(arc);
  const auto size_before = std::filesystem::file_size(arc);

  uvfs::writer w2;
  w2.add_file("/good", good);
  w2.add_file("/missing", (dir.path / "nope").string());
  CHECK_THROWS(w2.commit(arc));

  CHECK(std::filesystem::exists(arc));
  CHECK_EQ(std::filesystem::file_size(arc), size_before);
  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{1});
  auto got = r.find("/good");
  CHECK(got.has_value());
  if (got)
    CHECK(*got == expected_bytes(128, 3));
}

UVFS_TEST("writer/no_temporary_files_are_left_behind")
{
  scratch_dir dir{"tmp"};
  const auto src = dir.make_file("a.bin", 64);
  const auto arc = dir / "out.uvfs";

  uvfs::writer w;
  w.add_file("/a", src);
  w.commit(arc);

  int leftovers = 0;
  for (auto& e : std::filesystem::directory_iterator(dir.path))
    if (e.path().filename().string().rfind(".uvfs-tmp-", 0) == 0)
      leftovers++;
  CHECK_EQ(leftovers, 0);
}

UVFS_TEST("writer/many_threads_many_files")
{
  // Exercises the worker pool with more files than threads and with a mix of
  // empty and non-empty payloads.
  scratch_dir dir{"pool"};
  uvfs::writer w;
  for (int i = 0; i < 500; i++)
  {
    const auto leaf = "f" + std::to_string(i);
    w.add_file(
        "/" + leaf,
        dir.make_file(leaf, static_cast<std::size_t>(i % 13) * 100,
                      static_cast<uint64_t>(i + 1)));
  }
  const auto arc = dir / "out.uvfs";
  CHECK_NOTHROW(w.commit(arc));

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{500});
  for (int i = 0; i < 500; i++)
  {
    auto got = r.find("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(
          *got
          == expected_bytes(
              static_cast<std::size_t>(i % 13) * 100, static_cast<uint64_t>(i + 1)));
  }
}

#if defined(UVFS_HAS_ZSTD)
UVFS_TEST("writer/large_unreadable_input_is_reported_like_any_other")
{
  // Payloads bigger than the staging batch take a separate streaming path, and
  // that path opened the source file outside any try/catch. A permission error
  // there escaped as a bare runtime_error instead of commit_error, so the
  // caller lost the per-file list, and the blanket "append the output path"
  // handler made the message name the archive rather than the input at fault.
  scratch_dir dir{"bigunreadable"};
  const auto arc = dir / "out.uvfs";

  // Larger than compression_batch_bytes (64 MiB) so it streams.
  const std::size_t big = 70u * 1024 * 1024;
  std::string text;
  text.reserve(big);
  while (text.size() < big)
    text += "streaming compressible payload for the permission test. ";
  text.resize(big);
  const auto secret = dir.make_text("secret.bin", text);
  const auto ok = dir.make_text("fine.bin", "hello");
  std::filesystem::permissions(secret, std::filesystem::perms::none);

  if (::access(secret.c_str(), R_OK) == 0)
  {
    std::printf("    (running as root, permission check not meaningful)\n");
    return;
  }

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  w.add_file("/fine", ok);
  w.add_file("/secret", secret);

  bool got_commit_error = false;
  std::string message;
  std::vector<uvfs::skipped_file> files;
  try
  {
    w.commit(arc);
  }
  catch (const uvfs::commit_error& e)
  {
    got_commit_error = true;
    message = e.what();
    files = e.files;
  }
  catch (const std::exception& e)
  {
    message = e.what();
  }

  // It must arrive as commit_error, carrying which input failed.
  CHECK(got_commit_error);
  CHECK(!files.empty());
  if (!files.empty())
    CHECK(files.front().path_in_archive == "/secret");
  // ...and the message must name the input, not the archive being written.
  CHECK(message.find("secret.bin") != std::string::npos);
  CHECK(!std::filesystem::exists(arc));
}

UVFS_TEST("writer/large_input_that_vanishes_is_reported")
{
  scratch_dir dir{"bigvanish"};
  const auto arc = dir / "out.uvfs";
  const std::size_t big = 70u * 1024 * 1024;
  std::string blob;
  blob.reserve(big);
  std::mt19937_64 rng{4};
  while (blob.size() < big)
    blob.push_back(static_cast<char>(rng() & 0xff)); // incompressible
  const auto gone = dir.make_text("gone.bin", blob);

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  w.set_compression(cs);
  w.add_file("/gone", gone);
  std::filesystem::remove(gone);

  bool got_commit_error = false;
  try
  {
    w.commit(arc);
  }
  catch (const uvfs::commit_error&)
  {
    got_commit_error = true;
  }
  catch (const std::exception&)
  {
  }
  CHECK(got_commit_error);
  CHECK(!std::filesystem::exists(arc));
}
#endif
