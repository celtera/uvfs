// uvfs benchmark: measures the things the format claims to be good at.
//
//   build       how long it takes to produce an archive
//   open        time to a usable reader -- the number v2 exists to make small
//   lookup      steady-state cost of resolving a path
//   sparse      open + read a handful of files, which is what most callers do
//   read-all    open + read every file
//
// Usage: uvfs_bench <file-list> <scratch-dir>
#include <fcntl.h>
#include <unistd.h>
#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace
{
using clk = std::chrono::steady_clock;

auto us(clk::time_point a, clk::time_point b) -> double
{
  return std::chrono::duration<double, std::micro>(b - a).count();
}

auto read_list(const std::string& p) -> std::vector<std::string>
{
  std::vector<std::string> v;
  std::ifstream f(p);
  std::string l;
  while (std::getline(f, l))
    if (!l.empty())
      v.push_back(l);
  return v;
}

//! Drops an archive from the page cache. Works unprivileged.
void evict(const std::string& p)
{
  const int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0)
    return;
  ::fsync(fd);
  ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  ::close(fd);
}

template <typename F>
auto best_of(int reps, F&& f) -> double
{
  double best = 1e18;
  for (int i = 0; i < reps; i++)
    best = std::min(best, f());
  return best;
}

struct config
{
  const char* name;
  uvfs::compression method;
  int level;
  int64_t dict;
};

} // namespace

auto main(int argc, char** argv) -> int
{
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: %s <file-list> <scratch-dir>\n", argv[0]);
    return 2;
  }
  const auto files = read_list(argv[1]);
  const std::string scratch = argv[2];
  std::filesystem::create_directories(scratch);

  int64_t payload = 0;
  for (const auto& f : files)
  {
    std::error_code ec;
    const auto s = std::filesystem::file_size(f, ec);
    if (!ec)
      payload += static_cast<int64_t>(s);
  }
  std::printf("corpus: %zu files, %.1f MB of payload\n\n", files.size(), payload / 1e6);

  const config configs[] = {
      {"store", uvfs::compression::none, 0, 0},
      {"zstd -3 auto", uvfs::compression::automatic, 3, 0},
      {"zstd -9 auto", uvfs::compression::automatic, 9, 0},
      {"zstd -9 +dict", uvfs::compression::automatic, 9, 110 * 1024},
  };

  std::printf(
      "%-14s %10s %8s %9s %9s %9s %10s %11s %9s\n",
      "layout",
      "size",
      "of raw",
      "build",
      "open",
      "lookup",
      "sparse-10",
      "read-all",
      "zero-copy");
  std::printf("%s\n", std::string(100, '-').c_str());

  for (const auto& c : configs)
  {
    const std::string arc = scratch + "/bench.uvfs";

    const double build = best_of(
        3,
        [&]
        {
          uvfs::writer w;
          if (c.method != uvfs::compression::none)
          {
            uvfs::compression_settings cs;
            cs.method = c.method;
            cs.level = c.level;
            cs.dictionary_size = c.dict;
            w.set_compression(cs);
          }
          for (const auto& f : files)
            w.add_file(f, f);
          const auto t0 = clk::now();
          w.commit(arc);
          return us(t0, clk::now());
        });

    const auto size = std::filesystem::file_size(arc);

    // Keys in a random order, so lookups do not walk the index in layout order.
    std::vector<std::string> keys = files;
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64{12345});

    const double open_us = best_of(
        7,
        [&]
        {
          const auto t0 = clk::now();
          uvfs::reader r{arc};
          const auto n = r.count();
          const auto t = us(t0, clk::now());
          return n >= 0 ? t : t;
        });

    uvfs::reader r{arc};
    int64_t zero_copy = 0;
    for (int64_t i = 0; i < r.count(); i++)
      zero_copy += (r.at(i).storage == uvfs::stored_as::raw) ? 1 : 0;

    // Warm the index, then measure steady-state lookups.
    for (const auto& k : keys)
      (void)r.stat(k);
    const double lookup_ns = best_of(
                                 3,
                                 [&]
                                 {
                                   const auto t0 = clk::now();
                                   std::size_t hits = 0;
                                   for (const auto& k : keys)
                                     hits += r.stat(k).has_value();
                                   const auto t = us(t0, clk::now());
                                   return hits ? t : t;
                                 })
                             * 1000.0 / static_cast<double>(keys.size());

    // Sparse: what a caller that opens an archive to read a few files pays.
    const double sparse = best_of(
        5,
        [&]
        {
          const auto t0 = clk::now();
          uvfs::reader rr{arc};
          std::vector<char> buf;
          for (int i = 0; i < 10 && i < static_cast<int>(keys.size()); i++)
          {
            const auto info = rr.stat(keys[static_cast<std::size_t>(i)]);
            if (!info)
              continue;
            buf.resize(static_cast<std::size_t>(info->size));
            (void)rr.read_into(info->path, buf.data(), std::ssize(buf));
          }
          return us(t0, clk::now());
        });

    // Everything, cold, which is the honest number for a fresh process.
    evict(arc);
    const auto t0 = clk::now();
    {
      uvfs::reader rr{arc};
      std::vector<char> buf;
      int64_t total = 0;
      for (int64_t i = 0; i < rr.count(); i++)
      {
        const auto info = rr.at(i);
        buf.resize(static_cast<std::size_t>(info.size));
        const auto n = rr.read_into(info.path, buf.data(), std::ssize(buf));
        total += n ? *n : 0;
      }
      if (total < 0)
        return 1;
    }
    const double read_all = us(t0, clk::now());

    std::printf(
        "%-14s %9.1fM %7.1f%% %8.0fms %8.1fus %7.1fns %9.0fus %10.0fms %6ld/%ld\n",
        c.name,
        size / 1e6,
        100.0 * static_cast<double>(size) / static_cast<double>(payload),
        build / 1000.0,
        open_us,
        lookup_ns,
        sparse,
        read_all / 1000.0,
        zero_copy,
        r.count());

    std::filesystem::remove(arc);
  }
  return 0;
}
