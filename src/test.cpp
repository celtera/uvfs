#include <sys/resource.h>
#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <filesystem>
#include <functional>
#include <iostream>

#include <string_view>

template <typename Func>
void for_all_files(std::string_view root, Func f)
try
{
  namespace fs = std::filesystem;
  using iterator = fs::recursive_directory_iterator;
#if defined(_WIN32)
  constexpr auto options = fs::directory_options::skip_permission_denied;
#else
  constexpr auto options = fs::directory_options::follow_directory_symlink
                           | fs::directory_options::skip_permission_denied;
#endif
  for (auto it = iterator{root, options}, end = iterator{}; it != end; ++it)
  {
    try
    {
      const auto& path = it->path();
#if defined(_WIN32)
      std::string path_str = path.generic_string();
#else
      std::string_view path_str = path.native();
#endif
      if (path_str.empty())
        continue;
      auto last_slash = path_str.find_last_of('/');
      if (last_slash == std::string_view::npos || last_slash == path_str.length() - 1)
        continue;
      if (path_str[last_slash + 1] == '.')
      {
        it.disable_recursion_pending();
        continue;
      }

      if (fs::is_regular_file(path))
        f(path_str);
    }
    catch (...)
    {
      continue;
    }
  }
}
catch (...)
{
}

auto main() -> int
{
  uvfs::writer wr;

  for_all_files("/usr/include", [&](auto p) { wr.add_file(p, p); });

  wr.commit("/tmp/foo.uvfs");

  auto t0 = std::chrono::high_resolution_clock::now();
  uvfs::reader f{"/tmp/foo.uvfs"};
  auto t1 = std::chrono::high_resolution_clock::now();

  std::vector<std::string_view> paths;
  paths.reserve(f.size());

  f.for_each_file([&] (uvfs::reader::iter_entry entry) -> bool{
    paths.push_back(entry.path);
    return true;
  });

  std::sort(paths.begin(), paths.end());

  auto t2 = std::chrono::high_resolution_clock::now();
  for(auto& p : paths)
  {
    if(!f.find(p))
      return 1;
  }
  auto t3 = std::chrono::high_resolution_clock::now();
  std::cerr
      << "Load time: " << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << "us \n"
      << "Find time: " << std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count() << "us \n"
      ;

  return 0;
}
