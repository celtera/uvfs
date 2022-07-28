#include <sys/stat.h>
#include <uvfs/writer.hpp>

#include <filesystem>

auto main(int argc, char** argv) -> int
{
  if (argc < 2)
    return 1;

  const auto* out = argv[1];

  uvfs::writer wr;
  for (int i = 2; i < argc; i++)
  {
    std::filesystem::path p = argv[i];

    if (std::filesystem::is_regular_file(p))
    {
      std::string_view path = argv[i];
      wr.add_file(path, path);
    }
  }

  wr.commit(out);
}
