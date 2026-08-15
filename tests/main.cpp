#include "framework.hpp"

#include <algorithm>

auto main(int argc, char** argv) -> int
{
  using namespace uvfs::test;
  std::string filter = argc > 1 ? argv[1] : "";

  auto& all = registry();
  std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return a.name < b.name; });

  int ran = 0;
  for (auto& c : all)
  {
    if (!filter.empty() && c.name.find(filter) == std::string::npos)
      continue;
    ran++;
    const int before = failures();
    std::printf("  %s\n", c.name.c_str());
    try
    {
      c.fn();
    }
    catch (const std::exception& e)
    {
      failures()++;
      std::printf("    FAIL uncaught exception: %s\n", e.what());
    }
    catch (...)
    {
      failures()++;
      std::printf("    FAIL uncaught non-standard exception\n");
    }
    if (failures() != before)
      std::printf("    ^^ %s FAILED\n", c.name.c_str());
  }

  std::printf("\n%d test(s), %d check(s), %d failure(s)\n", ran, checks(), failures());
  return failures() == 0 ? 0 : 1;
}
