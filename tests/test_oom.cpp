#include "framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace uvfs::test;

#if defined(UVFS_HAS_ZSTD)

namespace
{
struct run_result
{
  int exit_code{};
  int signal{};
  std::string output;
};

//! Runs the helper under an address-space limit and reports how it ended.
auto run_under_limit(
    const scratch_dir& dir,
    long long limit_mb,
    int count,
    int file_mb,
    int threads) -> run_result
{
  const std::string cmd = std::string{UVFS_OOM_HELPER} + " " + std::to_string(limit_mb)
                          + " " + dir.path.string() + " " + std::to_string(count) + " "
                          + std::to_string(file_mb) + " " + std::to_string(threads)
                          + " 2>&1";
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
    // popen runs the command through /bin/sh, which reports a child killed by
    // a signal as exit code 128+N rather than passing the signal through. A
    // std::terminate would otherwise look like an ordinary non-zero exit.
    if (r.exit_code > 128)
    {
      r.signal = r.exit_code - 128;
      r.exit_code = 0;
    }
  }
  return r;
}
} // namespace

UVFS_TEST("oom/compression_worker_never_terminates")
{
  // A worker thread that lets an exception escape calls std::terminate, and
  // the compression path had no try/catch at all -- unlike the store path,
  // whose comment claims failures are recorded rather than thrown. Running out
  // of memory while staging a batch is the easiest way to trigger it.
  //
  // The window where the allocation that fails is the *unprotected* one is
  // narrow and depends on the allocator, so this sweeps a range rather than
  // pinning one value. Every outcome is acceptable except dying on a signal:
  // committing is fine, reporting commit_error is fine, aborting is not.
  const long long limits[] = {900, 1000, 1050, 1100, 1150, 1200, 1250, 1400};
  int aborted = 0, reported = 0, succeeded = 0;
  for (auto limit : limits)
  {
    scratch_dir dir{"oom" + std::to_string(limit)};
    const auto r = run_under_limit(dir, limit, 16, 60, 8);
    if (r.signal != 0)
    {
      aborted++;
      std::printf(
          "    limit=%lldMB died on signal %d: %s\n",
          limit,
          r.signal,
          r.output.substr(0, 80).c_str());
    }
    else if (r.exit_code == 10 || r.exit_code == 11)
      reported++;
    else if (r.exit_code == 0)
      succeeded++;

    // Whatever happened, no temporary may be left behind.
    int leftovers = 0;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir.path, ec))
      if (e.path().filename().string().rfind(".uvfs-tmp-", 0) == 0)
        leftovers++;
    CHECK_EQ(leftovers, 0);
  }
  std::printf(
      "    (%d succeeded, %d reported an error, %d aborted)\n",
      succeeded,
      reported,
      aborted);
  CHECK_EQ(aborted, 0);
}
#endif
