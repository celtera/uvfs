#if defined(__linux__) && !defined(__EMSCRIPTEN__)

// Helper for the disk-full tests: builds one archive into a directory that is
// deliberately too small, in its own process and mount namespace, so a SIGBUS
// is observable rather than fatal to the test suite.
//
//   enospc_commit <none|always> <outdir> <count> <kb-each>
#include <uvfs/writer.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

auto main(int argc, char** argv) -> int
{
  if (argc < 5)
    return 2;
  const std::string mode = argv[1];
  const std::string outdir = argv[2];
  const int count = std::atoi(argv[3]);
  const int kb = std::atoi(argv[4]);

  // Inputs live outside the small filesystem; only the archive goes into it.
  const std::string indir = "/tmp/enospc-inputs";
  std::filesystem::create_directories(indir);

  std::vector<std::string> paths;
  std::mt19937_64 rng{99};
  std::string blob(static_cast<std::size_t>(kb) * 1024, '\0');
  for (auto& c : blob)
    c = static_cast<char>(rng() & 0xff); // incompressible
  for (int i = 0; i < count; i++)
  {
    const auto p = indir + "/f" + std::to_string(i);
    if (!std::filesystem::exists(p))
    {
      FILE* f = std::fopen(p.c_str(), "wb");
      std::fwrite(blob.data(), 1, blob.size(), f);
      std::fclose(f);
    }
    paths.push_back(p);
  }

  uvfs::writer w;
  try
  {
    // Inside the handler: a build without zstd refuses compression here, and
    // letting that escape main() would look like a crash to the caller.
    if (mode == "always")
    {
      uvfs::compression_settings cs;
      cs.method = uvfs::compression::always;
      w.set_compression(cs);
    }
    for (int i = 0; i < count; i++)
      w.add_file("/f" + std::to_string(i), paths[static_cast<std::size_t>(i)]);

    w.commit(outdir + "/out.uvfs");
    std::printf("committed\n");
    return 0;
  }
  catch (const std::exception& e)
  {
    std::printf("error: %s\n", e.what());
    return 10;
  }
}

#endif // linux only
