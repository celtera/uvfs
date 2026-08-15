// Resource limits and mount namespaces are POSIX concepts; these tests
// exercise how the writer behaves when the system refuses it memory or
// space, which is checked differently on Windows.
#if !defined(_WIN32)

// Helper for the out-of-memory tests: runs one commit under an address-space
// limit, in its own process, so that a std::terminate is observable as a
// signal rather than taking the test suite down with it.
//
//   oom_commit <limit-mb> <workdir> <file-count> <file-mb> <threads>
#include <sys/resource.h>
#include <uvfs/writer.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

auto main(int argc, char** argv) -> int
{
  if (argc < 6)
    return 2;
  const auto limit_mb = std::strtoll(argv[1], nullptr, 10);
  const std::string dir = argv[2];
  const int count = std::atoi(argv[3]);
  const int file_mb = std::atoi(argv[4]);
  const int threads = std::atoi(argv[5]);

  std::filesystem::create_directories(dir + "/in");

  // Incompressible, so every payload is staged and compressed in full.
  std::vector<std::string> paths;
  std::mt19937_64 rng{1234};
  std::string blob(static_cast<std::size_t>(file_mb) * 1024 * 1024, '\0');
  for (auto& c : blob)
    c = static_cast<char>(rng() & 0xff);
  for (int i = 0; i < count; i++)
  {
    const auto p = dir + "/in/f" + std::to_string(i);
    FILE* f = std::fopen(p.c_str(), "wb");
    std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    paths.push_back(p);
  }

  // Applied after the inputs exist so the limit constrains the commit only.
  const rlim_t bytes = static_cast<rlim_t>(limit_mb) * 1024 * 1024;
  struct rlimit rl
  {
    bytes, bytes
  };
  if (setrlimit(RLIMIT_AS, &rl) != 0)
    return 3;

  uvfs::writer w;
  w.set_thread_count(threads);
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  for (const auto& p : paths)
    w.add_file("/" + std::filesystem::path{p}.filename().string(), p);

  try
  {
    w.commit(dir + "/out.uvfs");
    std::printf("committed\n");
    return 0;
  }
  catch (const uvfs::commit_error& e)
  {
    std::printf("commit_error: %s\n", e.what());
    return 10;
  }
  catch (const std::exception& e)
  {
    std::printf("exception %s\n", e.what());
    return 11;
  }
}

#endif // !_WIN32
