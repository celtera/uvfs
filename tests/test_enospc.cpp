#include "framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace uvfs::test;

namespace
{
struct run_result
{
  int exit_code{};
  int signal{};
  std::string output;
};

//! Runs one commit into a tmpfs of the given size, inside a private mount
//! namespace, so the destination filesystem fills up part way through.
auto commit_into_small_fs(const char* mode, int fs_mb, int count, int kb) -> run_result
{
  const std::string script
      = std::string{"set -e; "} + "MNT=/tmp/uvfs-smallfs-$$; mkdir -p $MNT; "
        + "mount -t tmpfs -o size=" + std::to_string(fs_mb) + "m tmpfs $MNT; "
        + UVFS_ENOSPC_HELPER + " " + mode + " $MNT " + std::to_string(count) + " "
        + std::to_string(kb);
  const std::string cmd = "unshare -Urm sh -c '" + script + "' 2>&1";

  run_result r;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p)
    return r;
  char buf[512];
  while (std::fgets(buf, sizeof buf, p))
    r.output += buf;
  const int status = pclose(p);
  if (WIFSIGNALED(status))
    r.signal = WTERMSIG(status);
  else
  {
    r.exit_code = WEXITSTATUS(status);
    // popen goes through /bin/sh, which turns a signal death into 128+N.
    if (r.exit_code > 128)
    {
      r.signal = r.exit_code - 128;
      r.exit_code = 0;
    }
  }
  return r;
}

//! "always" only means anything where the build can actually compress.
auto compression_modes() -> std::vector<const char*>
{
#if defined(UVFS_HAS_ZSTD)
  return {"none", "always"};
#else
  return {"none"};
#endif
}

//! Requires unshare(1) and permission to create user + mount namespaces, so
//! it is Linux-only and not available in every container either. A probe that
//! actually commits is the only reliable test.
auto namespaces_available() -> bool
{
  const auto r = commit_into_small_fs("none", 64, 1, 1);
  return r.signal == 0 && r.exit_code == 0;
}
} // namespace

UVFS_TEST("enospc/running_out_of_space_is_an_error_not_a_crash")
{
  if (!namespaces_available())
  {
    std::printf("    (user namespaces unavailable, skipping)\n");
    return;
  }

  // Writing payloads through a mapping of a sparse file means the kernel has
  // no way to report a full filesystem: the store fails at page-fault time and
  // the process gets SIGBUS. Blocks have to be reserved before they are
  // written through.
  for (auto* mode : compression_modes())
  {
    const auto r = commit_into_small_fs(mode, 4, 2000, 20);
    std::printf(
        "    %-7s signal=%d exit=%d :: %s",
        mode,
        r.signal,
        r.exit_code,
        r.output.empty() ? "(no output)\n" : r.output.c_str());

    CHECK_EQ(r.signal, 0);
    // It must fail -- 40 MB of payload cannot fit in 4 MB -- and say so.
    CHECK_EQ(r.exit_code, 10);
    const bool blames_space = r.output.find("space") != std::string::npos
                              || r.output.find("No space") != std::string::npos;
    // The old uncompressed path reported "read failed: Bad address" and named
    // an input file, which sends anyone debugging it in the wrong direction.
    const bool blames_an_input = r.output.find("read failed") != std::string::npos;
    CHECK(blames_space);
    CHECK(!blames_an_input);
  }
}

UVFS_TEST("enospc/an_archive_that_fits_still_commits")
{
  if (!namespaces_available())
  {
    std::printf("    (user namespaces unavailable, skipping)\n");
    return;
  }
  // Same filesystem size, comfortably enough room: reserving space must not
  // turn a workable commit into a failure.
  for (auto* mode : compression_modes())
  {
    const auto r = commit_into_small_fs(mode, 64, 200, 20);
    std::printf("    %-7s exit=%d %s", mode, r.exit_code, r.output.c_str());
    CHECK_EQ(r.signal, 0);
    CHECK_EQ(r.exit_code, 0);
  }
}
