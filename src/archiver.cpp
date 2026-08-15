// uvfs: command line archiver.
#include <uvfs/path.hpp>
#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <string_view>

namespace fs = std::filesystem;

namespace
{
void usage()
{
  std::fprintf(
      stderr,
      "usage:\n"
      "  uvfs create <archive> [options] <path>...\n"
      "  uvfs list   <archive>\n"
      "  uvfs verify <archive>\n"
      "  uvfs extract <archive> <output-dir>\n"
      "\n"
      "create options:\n"
      "  --compress[=auto|always|none]  default none; auto keeps raw bytes\n"
      "                                 when compression does not pay\n"
      "  --level=N                      zstd level (default 3)\n"
      "  --dict=BYTES                   train a shared dictionary of this size\n"
      "  --threads=N                    0 = automatic\n"
      "  --no-hashes                    omit per-entry content hashes\n"
      "  --skip-unreadable              leave unreadable inputs out\n"
      "  --strip=PREFIX                 remove PREFIX from stored paths\n");
}

//! Turns a filesystem path into an archive path: '/'-separated, with the
//! requested prefix removed and any leading separators dropped.
auto to_archive_path(const fs::path& p, std::string_view strip) -> std::string
{
  std::string s = p.generic_string();
  if (!strip.empty() && s.rfind(strip, 0) == 0)
    s.erase(0, strip.size());
  while (!s.empty() && s.front() == '/')
    s.erase(0, 1);
  return "/" + s;
}

auto human(int64_t bytes) -> std::string
{
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4)
  {
    v /= 1024.0;
    u++;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
  return buf;
}

auto parse_after(std::string_view arg, std::string_view flag) -> std::string_view
{
  if (arg.rfind(flag, 0) != 0)
    return {};
  return arg.substr(flag.size());
}

auto cmd_create(int argc, char** argv) -> int
{
  const std::string archive = argv[2];
  uvfs::writer w;
  uvfs::compression_settings cs;
  std::string strip;
  std::vector<std::string> roots;

  for (int i = 3; i < argc; i++)
  {
    const std::string_view a = argv[i];
    if (a == "--compress" || a == "--compress=auto")
      cs.method = uvfs::compression::automatic;
    else if (a == "--compress=always")
      cs.method = uvfs::compression::always;
    else if (a == "--compress=none")
      cs.method = uvfs::compression::none;
    else if (auto lvl = parse_after(a, "--level="); !lvl.empty())
      cs.level = std::stoi(std::string{lvl});
    else if (auto dict = parse_after(a, "--dict="); !dict.empty())
      cs.dictionary_size = std::stoll(std::string{dict});
    else if (auto thr = parse_after(a, "--threads="); !thr.empty())
      w.set_thread_count(std::stoi(std::string{thr}));
    else if (a == "--no-hashes")
      w.set_content_hashes(false);
    else if (a == "--skip-unreadable")
      w.set_unreadable_policy(uvfs::on_unreadable::skip);
    else if (auto pre = parse_after(a, "--strip="); !pre.empty())
      strip = std::string{pre};
    else if (a.rfind("--", 0) == 0)
    {
      std::fprintf(stderr, "uvfs: unknown option %.*s\n", (int)a.size(), a.data());
      return 2;
    }
    else
      roots.emplace_back(a);
  }

  if (roots.empty())
  {
    usage();
    return 2;
  }
  w.set_compression(cs);

  int64_t count = 0, bytes = 0;
  const auto add = [&](const fs::path& p)
  {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec)
      return;
    w.add_file(to_archive_path(p, strip), p.string());
    count++;
    bytes += static_cast<int64_t>(size);
  };

  for (const auto& root : roots)
  {
    std::error_code ec;
    if (fs::is_directory(root, ec))
    {
      // Symlinked directories are not followed: doing so double-counts files
      // reachable by more than one path, and can loop.
      for (fs::recursive_directory_iterator
               it{root, fs::directory_options::skip_permission_denied, ec},
           end;
           it != end;
           it.increment(ec))
      {
        if (ec)
          break;
        if (it->is_regular_file(ec) && !it->is_symlink(ec))
          add(it->path());
      }
    }
    else
    {
      add(root);
    }
  }

  try
  {
    w.commit(archive);
  }
  catch (const uvfs::commit_error& e)
  {
    std::fprintf(stderr, "uvfs: %s\n", e.what());
    for (const auto& f : e.files)
      std::fprintf(stderr, "  %s: %s\n", f.path_in_system.c_str(), f.reason.c_str());
    return 1;
  }

  for (const auto& f : w.skipped())
    std::fprintf(
        stderr, "uvfs: skipped %s (%s)\n", f.path_in_system.c_str(), f.reason.c_str());

  std::error_code ec;
  const auto on_disk = static_cast<int64_t>(fs::file_size(archive, ec));
  std::printf(
      "%s: %lld files, %s of content in %s (%.1f%%)\n",
      archive.c_str(),
      static_cast<long long>(count),
      human(bytes).c_str(),
      human(on_disk).c_str(),
      bytes ? 100.0 * static_cast<double>(on_disk) / static_cast<double>(bytes) : 0.0);
  return 0;
}

auto cmd_list(const std::string& archive) -> int
{
  uvfs::reader r{archive};
  std::printf("%12s %12s  %-9s %s\n", "size", "stored", "storage", "path");
  for (int64_t i = 0; i < r.count(); i++)
  {
    const auto e = r.at(i);
    std::printf(
        "%12lld %12lld  %-9s %.*s\n",
        static_cast<long long>(e.size),
        static_cast<long long>(e.stored_size),
        e.storage == uvfs::stored_as::raw ? "stored" : "zstd",
        static_cast<int>(e.path.size()),
        e.path.data());
  }
  std::printf("%lld entries\n", static_cast<long long>(r.count()));
  return 0;
}

auto cmd_verify(const std::string& archive) -> int
{
  // integrity::index checks the header and the whole index up front; verify()
  // then reads every payload and checks its content hash.
  uvfs::reader r{archive, uvfs::integrity::index};
  if (!r.has_content_hashes())
  {
    std::printf(
        "%s: header and index intact (no content hashes stored)\n", archive.c_str());
    return 0;
  }
  const auto damaged = r.verify();
  if (damaged.empty())
  {
    std::printf(
        "%s: header, index and all %lld payloads intact\n",
        archive.c_str(),
        static_cast<long long>(r.count()));
    return 0;
  }
  std::fprintf(stderr, "%s: %zu damaged payload(s)\n", archive.c_str(), damaged.size());
  for (const auto& d : damaged)
    std::fprintf(stderr, "  %s\n", d.c_str());
  return 1;
}

auto cmd_extract(const std::string& archive, const std::string& outdir) -> int
{
  uvfs::reader r{archive, uvfs::integrity::index};
  std::vector<char> buf;
  int64_t written = 0;
  for (int64_t i = 0; i < r.count(); i++)
  {
    const auto e = r.at(i);
    // Archive paths are validated at write time, but an extractor should not
    // take that on trust: it is joining these names onto a real directory.
    if (!uvfs::is_safe_archive_path(e.path))
    {
      std::fprintf(
          stderr,
          "uvfs: refusing unsafe path %.*s\n",
          static_cast<int>(e.path.size()),
          e.path.data());
      return 1;
    }
    std::string rel{e.path};
    while (!rel.empty() && rel.front() == '/')
      rel.erase(0, 1);

    const fs::path target = fs::path{outdir} / rel;
    fs::create_directories(target.parent_path());

    // read() validates the size against the frame before allocating; sizing
    // buf from e.size here would reintroduce the unbounded allocation.
    auto payload = r.read(e.path);
    if (!payload)
      continue;
    buf = std::move(*payload);
    const int64_t n = std::ssize(buf);
    // path::c_str() is wchar_t* on Windows, so go through string().
    const auto target_utf8 = target.string();
    FILE* f = std::fopen(target_utf8.c_str(), "wb");
    if (!f)
    {
      std::fprintf(stderr, "uvfs: could not write %s\n", target_utf8.c_str());
      return 1;
    }
    if (n > 0)
      std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), f);
    std::fclose(f);
    written++;
  }
  std::printf(
      "extracted %lld files to %s\n", static_cast<long long>(written), outdir.c_str());
  return 0;
}

} // namespace

auto main(int argc, char** argv) -> int
{
  // An inner try rather than a function-try-block: the latter is valid C++ but
  // several static analysers cannot parse it, and this costs nothing.
  try
  {
    if (argc < 3)
    {
      usage();
      return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "create")
      return cmd_create(argc, argv);
    if (cmd == "list")
      return cmd_list(argv[2]);
    if (cmd == "verify")
      return cmd_verify(argv[2]);
    if (cmd == "extract")
    {
      if (argc < 4)
      {
        usage();
        return 2;
      }
      return cmd_extract(argv[2], argv[3]);
    }
    usage();
    return 2;
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "uvfs: %s\n", e.what());
    return 1;
  }
}
