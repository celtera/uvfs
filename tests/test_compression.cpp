#include "archive_bytes.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <random>
#include <string>
#include <vector>

using namespace uvfs::test;

#if defined(UVFS_HAS_ZSTD)

namespace
{
//! Highly compressible: long runs and a small alphabet, like text or config.
auto compressible(std::size_t n, uint64_t seed = 1) -> std::string
{
  static const char* words[]
      = {"alpha ", "beta ", "gamma ", "delta ", "epsilon ", "zeta "};
  std::string s;
  std::mt19937_64 rng{seed};
  while (s.size() < n)
    s += words[rng() % 6];
  s.resize(n);
  return s;
}

//! Stands in for already-compressed media: audio, video, JPEG. zstd cannot do
//! anything with it, which is the case the automatic policy exists for.
auto incompressible(std::size_t n, uint64_t seed = 2) -> std::string
{
  std::string s(n, '\0');
  std::mt19937_64 rng{seed};
  for (auto& c : s)
    c = static_cast<char>(rng() & 0xff);
  return s;
}

auto write_blob(const scratch_dir& dir, std::string_view leaf, const std::string& data)
    -> std::string
{
  return dir.make_text(leaf, data);
}
} // namespace

UVFS_TEST("compression/roundtrips_every_payload")
{
  scratch_dir dir{"zroundtrip"};
  const auto arc = dir / "out.uvfs";

  std::vector<std::pair<std::string, std::string>> want;
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);

  // sizes around the interesting boundaries, plus both kinds of content
  const std::size_t sizes[] = {0, 1, 63, 64, 65, 1000, 65536, 300000};
  int i = 0;
  for (auto sz : sizes)
  {
    for (int kind = 0; kind < 2; kind++)
    {
      const auto data = kind ? incompressible(sz, sz + 1) : compressible(sz, sz + 1);
      const auto key = "/f" + std::to_string(i++);
      want.emplace_back(key, data);
      w.add_file(key, write_blob(dir, "s" + std::to_string(i), data));
    }
  }
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), want.size());
  for (auto& [key, data] : want)
  {
    auto got = r.read(key);
    CHECK(got.has_value());
    if (got)
      CHECK(std::string(got->begin(), got->end()) == data);
  }
}

UVFS_TEST("compression/automatic_declines_on_incompressible_data")
{
  // The point of the automatic policy: media that is already compressed keeps
  // its zero-copy pointer instead of paying for a decompression step that
  // buys nothing.
  scratch_dir dir{"zauto"};
  const auto arc = dir / "out.uvfs";

  const auto text = compressible(200000, 7);
  const auto media = incompressible(200000, 8);

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  w.set_compression(cs);
  w.add_file("/doc.txt", write_blob(dir, "doc", text));
  w.add_file("/clip.wav", write_blob(dir, "clip", media));
  w.commit(arc);

  uvfs::reader r{arc};

  auto doc = r.stat("/doc.txt");
  CHECK(doc.has_value());
  if (doc)
  {
    CHECK(doc->storage == uvfs::stored_as::compressed);
    CHECK(doc->stored_size < doc->size);
    // find() has no verbatim bytes to hand out for a compressed entry.
    CHECK(!r.find("/doc.txt").has_value());
  }

  auto clip = r.stat("/clip.wav");
  CHECK(clip.has_value());
  if (clip)
  {
    CHECK(clip->storage == uvfs::stored_as::raw);
    CHECK_EQ(clip->stored_size, clip->size);
    // ...and it still hands out a pointer straight into the mapping.
    auto mapped = r.find("/clip.wav");
    CHECK(mapped.has_value());
    if (mapped)
    {
      CHECK(*mapped == media);
      CHECK_EQ(
          reinterpret_cast<std::uintptr_t>(mapped->data()) % 64u, std::uintptr_t{0});
    }
  }

  // read() works for both, regardless of how they are stored.
  auto a = r.read("/doc.txt");
  auto b = r.read("/clip.wav");
  CHECK(a.has_value());
  CHECK(b.has_value());
  if (a)
    CHECK(std::string(a->begin(), a->end()) == text);
  if (b)
    CHECK(std::string(b->begin(), b->end()) == media);
}

UVFS_TEST("compression/min_gain_threshold_is_respected")
{
  scratch_dir dir{"zgain"};
  // Content that compresses by a real but modest amount: three quarters of it
  // is incompressible noise, the rest is a repeated block. Sprinkling single
  // bytes through noise would not compress at all, which would test nothing.
  std::string data;
  {
    const auto noise = incompressible(75000, 11);
    const auto filler = compressible(25000, 12);
    for (std::size_t i = 0; i < 25; i++)
    {
      data += noise.substr(i * 3000, 3000);
      data += filler.substr(i * 1000, 1000);
    }
  }

  auto build = [&](int gain, const char* name)
  {
    const auto arc = dir / name;
    uvfs::writer w;
    uvfs::compression_settings cs;
    cs.method = uvfs::compression::automatic;
    cs.min_gain_percent = gain;
    w.set_compression(cs);
    w.add_file("/a", write_blob(dir, "a", data));
    w.commit(arc);
    uvfs::reader r{arc};
    return r.stat("/a")->storage;
  };

  // Demanding a 90% saving must make it store instead.
  CHECK(build(90, "strict.uvfs") == uvfs::stored_as::raw);
  // Accepting any saving at all must make it compress.
  CHECK(build(0, "lax.uvfs") == uvfs::stored_as::compressed);
}

UVFS_TEST("compression/tiny_payloads_are_stored")
{
  scratch_dir dir{"ztiny"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  cs.min_size = 64;
  w.set_compression(cs);
  w.add_file("/tiny", write_blob(dir, "t", compressible(32, 3)));
  w.add_file("/big", write_blob(dir, "b", compressible(50000, 4)));
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK(r.stat("/tiny")->storage == uvfs::stored_as::raw);
  CHECK(r.stat("/big")->storage == uvfs::stored_as::compressed);
}

UVFS_TEST("compression/shrinks_a_text_corpus")
{
  scratch_dir dir{"zshrink"};
  uvfs::writer plain, packed;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  packed.set_compression(cs);

  for (int i = 0; i < 300; i++)
  {
    const auto data = compressible(4000, static_cast<uint64_t>(i));
    const auto src = write_blob(dir, "s" + std::to_string(i), data);
    plain.add_file("/f" + std::to_string(i), src);
    packed.add_file("/f" + std::to_string(i), src);
  }
  const auto a = dir / "plain.uvfs";
  const auto b = dir / "packed.uvfs";
  plain.commit(a);
  packed.commit(b);

  const auto plain_size = std::filesystem::file_size(a);
  const auto packed_size = std::filesystem::file_size(b);
  std::printf(
      "    (plain %zu bytes, compressed %zu bytes)\n",
      static_cast<std::size_t>(plain_size),
      static_cast<std::size_t>(packed_size));
  CHECK(packed_size < plain_size / 2);

  // ...and it still reads back exactly.
  uvfs::reader r{b};
  for (int i = 0; i < 300; i++)
  {
    auto got = r.read("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(
          std::string(got->begin(), got->end())
          == compressible(4000, static_cast<uint64_t>(i)));
  }
}

UVFS_TEST("compression/dictionary_helps_many_small_files")
{
  scratch_dir dir{"zdict"};
  auto build = [&](int64_t dict_size, const char* name)
  {
    uvfs::writer w;
    uvfs::compression_settings cs;
    cs.method = uvfs::compression::always;
    cs.level = 9;
    cs.min_size = 0;
    cs.dictionary_size = dict_size;
    w.set_compression(cs);
    for (int i = 0; i < 2000; i++)
      w.add_file(
          "/f" + std::to_string(i),
          write_blob(
              dir,
              "s" + std::to_string(i),
              compressible(300, static_cast<uint64_t>(i))));
    const auto arc = dir / name;
    w.commit(arc);
    return arc;
  };

  const auto without = build(0, "nodict.uvfs");
  const auto with = build(16 * 1024, "dict.uvfs");
  const auto a = std::filesystem::file_size(without);
  const auto b = std::filesystem::file_size(with);
  std::printf(
      "    (no dictionary %zu bytes, with dictionary %zu bytes)\n",
      static_cast<std::size_t>(a),
      static_cast<std::size_t>(b));
  CHECK(b < a);

  // The dictionary lives in the archive, so reading needs nothing extra.
  uvfs::reader r{with};
  for (int i = 0; i < 2000; i += 97)
  {
    auto got = r.read("/f" + std::to_string(i));
    CHECK(got.has_value());
    if (got)
      CHECK(
          std::string(got->begin(), got->end())
          == compressible(300, static_cast<uint64_t>(i)));
  }
}

UVFS_TEST("compression/content_hashes_cover_stored_bytes")
{
  scratch_dir dir{"zhash"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  w.add_file("/a", write_blob(dir, "a", compressible(20000, 5)));
  w.commit(arc);

  {
    uvfs::reader r{arc};
    CHECK(r.has_content_hashes());
    CHECK(r.verify().empty());
  }

  // Damage a compressed payload; verify() must notice without decompressing.
  auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  bytes[static_cast<std::size_t>(h.data_start + 5)] ^= 0x40;
  const auto bad = dir / "bad.uvfs";
  spit(bad, bytes);

  uvfs::reader r{bad};
  CHECK_EQ(r.verify().size(), std::size_t{1});
  // And reading it fails loudly rather than returning wrong bytes.
  CHECK_THROWS((void)r.read("/a"));
}

UVFS_TEST("compression/output_is_reproducible")
{
  // Compression runs in parallel, but offsets are assigned in sorted order, so
  // two runs over the same inputs must produce identical bytes.
  scratch_dir dir{"zrepro"};
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;

  auto build = [&](const char* name)
  {
    uvfs::writer w;
    w.set_compression(cs);
    for (int i = 0; i < 400; i++)
      w.add_file(
          "/f" + std::to_string(i),
          write_blob(
              dir,
              "s" + std::to_string(i),
              (i % 3) ? compressible(3000, static_cast<uint64_t>(i))
                      : incompressible(3000, static_cast<uint64_t>(i))));
    const auto arc = dir / name;
    w.commit(arc);
    return slurp(arc);
  };

  const auto a = build("one.uvfs");
  const auto b = build("two.uvfs");
  CHECK_EQ(a.size(), b.size());
  CHECK(a == b);
}

UVFS_TEST("compression/large_payload_is_streamed")
{
  // Larger than the in-memory batch, so it takes the streaming path.
  scratch_dir dir{"zbig"};
  const auto arc = dir / "out.uvfs";
  const std::size_t big = 80u * 1024 * 1024;

  const auto data = compressible(big, 21);
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  w.set_compression(cs);
  w.add_file("/big.bin", write_blob(dir, "big", data));
  w.add_file("/small.bin", write_blob(dir, "small", compressible(1000, 22)));
  w.commit(arc);

  uvfs::reader r{arc};
  auto info = r.stat("/big.bin");
  CHECK(info.has_value());
  if (info)
  {
    CHECK_EQ(info->size, static_cast<int64_t>(big));
    CHECK(info->storage == uvfs::stored_as::compressed);
  }
  auto got = r.read("/big.bin");
  CHECK(got.has_value());
  if (got)
    CHECK(std::string(got->begin(), got->end()) == data);
  CHECK(r.read("/small.bin").has_value());
}

UVFS_TEST("compression/large_incompressible_payload_is_stored")
{
  scratch_dir dir{"zbigmedia"};
  const auto arc = dir / "out.uvfs";
  const std::size_t big = 80u * 1024 * 1024;
  const auto data = incompressible(big, 23);

  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  w.set_compression(cs);
  w.add_file("/movie.bin", write_blob(dir, "movie", data));
  w.commit(arc);

  uvfs::reader r{arc};
  auto info = r.stat("/movie.bin");
  CHECK(info.has_value());
  if (info)
  {
    CHECK(info->storage == uvfs::stored_as::raw);
    CHECK_EQ(info->stored_size, static_cast<int64_t>(big));
  }
  // Zero copy is preserved for exactly the data that most needs it.
  auto mapped = r.find("/movie.bin");
  CHECK(mapped.has_value());
  if (mapped)
    CHECK(*mapped == data);
}

UVFS_TEST("compression/archive_shrinks_to_fit")
{
  // The file is mapped at its uncompressed upper bound and truncated back
  // down; the published archive must not carry that slack around.
  scratch_dir dir{"ztrunc"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::always;
  w.set_compression(cs);
  std::size_t raw_total = 0;
  for (int i = 0; i < 100; i++)
  {
    const auto data = compressible(20000, static_cast<uint64_t>(i));
    raw_total += data.size();
    w.add_file("/f" + std::to_string(i), write_blob(dir, "s" + std::to_string(i), data));
  }
  w.commit(arc);

  const auto on_disk = std::filesystem::file_size(arc);
  CHECK(on_disk < raw_total / 4);

  uvfs::reader r{arc};
  CHECK_EQ(r.size(), std::size_t{100});
  CHECK(r.verify().empty());
}

#else // !UVFS_HAS_ZSTD

UVFS_TEST("compression/is_refused_without_zstd")
{
  uvfs::writer w;
  uvfs::compression_settings cs;
  cs.method = uvfs::compression::automatic;
  CHECK_THROWS(w.set_compression(cs));
}

#endif
