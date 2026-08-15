#pragma once
// A very small test framework: no dependencies, registers tests through static
// initialisers, and reports every failure rather than stopping at the first.
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <string_view>

namespace uvfs::test
{

struct case_t
{
  std::string name;
  void (*fn)();
};

inline auto registry() -> std::vector<case_t>&
{
  static std::vector<case_t> r;
  return r;
}

inline auto failures() -> int&
{
  static int f = 0;
  return f;
}

inline auto checks() -> int&
{
  static int c = 0;
  return c;
}

struct registrar
{
  registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void
report(bool ok, const char* file, int line, const char* expr, const std::string& detail)
{
  checks()++;
  if (ok)
    return;
  failures()++;
  std::printf("    FAIL %s:%d\n         %s\n", file, line, expr);
  if (!detail.empty())
    std::printf("         %s\n", detail.c_str());
}

template <typename T>
auto show(const T& v) -> std::string
{
  if constexpr (std::is_convertible_v<T, std::string_view>)
    return '"' + std::string(std::string_view(v)) + '"';
  else if constexpr (std::is_same_v<T, bool>)
    return v ? "true" : "false";
  else
    return std::to_string(v);
}

// A directory that is created empty and removed when the test ends.
struct scratch_dir
{
  std::filesystem::path path;

  explicit scratch_dir(std::string_view label)
  {
    static std::atomic_int counter{0};
    path = std::filesystem::temp_directory_path()
           / ("uvfs-test-" + std::string(label) + "-"
              + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~scratch_dir()
  {
    std::error_code ec;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
    for (auto& e : std::filesystem::recursive_directory_iterator(path, ec))
      std::filesystem::permissions(
          e.path(),
          std::filesystem::perms::owner_all,
          std::filesystem::perm_options::add,
          ec);
    std::filesystem::remove_all(path, ec);
  }
  scratch_dir(const scratch_dir&) = delete;
  auto operator=(const scratch_dir&) -> scratch_dir& = delete;

  [[nodiscard]] auto operator/(std::string_view leaf) const -> std::string
  {
    return (path / leaf).string();
  }

  // Creates a file of `n` deterministic pseudo-random bytes.
  auto
  make_file(std::string_view leaf, std::size_t n, uint64_t seed = 1) const -> std::string
  {
    auto p = path / leaf;
    std::filesystem::create_directories(p.parent_path());
    std::string data(n, '\0');
    std::mt19937_64 rng{seed};
    for (auto& c : data)
      c = static_cast<char>(rng() & 0xff);
    const auto p_utf8 = p.string();
    FILE* f = std::fopen(p_utf8.c_str(), "wb");
    if (n)
      std::fwrite(data.data(), 1, n, f);
    std::fclose(f);
    return p_utf8;
  }

  auto make_text(std::string_view leaf, std::string_view content) const -> std::string
  {
    auto p = path / leaf;
    std::filesystem::create_directories(p.parent_path());
    const auto p_utf8 = p.string();
    FILE* f = std::fopen(p_utf8.c_str(), "wb");
    if (!content.empty())
      std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return p.string();
  }
};

//! Resident set size in KB, or -1 where it cannot be read.
//!
//! Only /proc gives a *current* figure; getrusage reports the peak, which is
//! monotonic and so useless for comparing one operation against another.
//! Tests that need this skip themselves where it is unavailable rather than
//! quietly asserting against zero.
inline auto current_rss_kb() -> long
{
#if defined(_WIN32)
  return -1; // no /proc; the tests that need this skip themselves
#else
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f)
    return -1;
  long total = 0, resident = 0;
  const bool ok = std::fscanf(f, "%ld %ld", &total, &resident) == 2;
  std::fclose(f);
  if (!ok)
    return -1;
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

inline auto have_current_rss() -> bool
{
  return current_rss_kb() >= 0;
}

inline auto expected_bytes(std::size_t n, uint64_t seed = 1) -> std::string
{
  std::string data(n, '\0');
  std::mt19937_64 rng{seed};
  for (auto& c : data)
    c = static_cast<char>(rng() & 0xff);
  return data;
}

} // namespace uvfs::test

#define UVFS_CAT_(a, b) a##b
#define UVFS_CAT(a, b) UVFS_CAT_(a, b)

#define UVFS_TEST(name)                                                    \
  static void UVFS_CAT(uvfs_test_fn_, __LINE__)();                         \
  static const ::uvfs::test::registrar UVFS_CAT(uvfs_test_reg_, __LINE__){ \
      name, &UVFS_CAT(uvfs_test_fn_, __LINE__)};                           \
  static void UVFS_CAT(uvfs_test_fn_, __LINE__)()

#define CHECK(...) \
  ::uvfs::test::report(!!(__VA_ARGS__), __FILE__, __LINE__, #__VA_ARGS__, {})

#define CHECK_EQ(a, b)                                   \
  do                                                     \
  {                                                      \
    const auto& uvfs_a_ = (a);                           \
    const auto& uvfs_b_ = (b);                           \
    ::uvfs::test::report(                                \
        uvfs_a_ == uvfs_b_,                              \
        __FILE__,                                        \
        __LINE__,                                        \
        #a " == " #b,                                    \
        "got " + ::uvfs::test::show(uvfs_a_) + ", want " \
            + ::uvfs::test::show(uvfs_b_));              \
  } while (0)

#define CHECK_THROWS(...)                              \
  do                                                   \
  {                                                    \
    bool uvfs_threw_ = false;                          \
    try                                                \
    {                                                  \
      __VA_ARGS__;                                     \
    }                                                  \
    catch (const std::exception&)                      \
    {                                                  \
      uvfs_threw_ = true;                              \
    }                                                  \
    ::uvfs::test::report(                              \
        uvfs_threw_,                                   \
        __FILE__,                                      \
        __LINE__,                                      \
        #__VA_ARGS__ " throws",                        \
        uvfs_threw_ ? "" : "no exception was thrown"); \
  } while (0)

#define CHECK_NOTHROW(...)                               \
  do                                                     \
  {                                                      \
    std::string uvfs_err_;                               \
    try                                                  \
    {                                                    \
      __VA_ARGS__;                                       \
    }                                                    \
    catch (const std::exception& e)                      \
    {                                                    \
      uvfs_err_ = e.what();                              \
    }                                                    \
    ::uvfs::test::report(                                \
        uvfs_err_.empty(),                               \
        __FILE__,                                        \
        __LINE__,                                        \
        #__VA_ARGS__ " does not throw",                  \
        uvfs_err_.empty() ? "" : "threw: " + uvfs_err_); \
  } while (0)
