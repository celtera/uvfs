#include "archive_bytes.hpp"
#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <random>
#include <vector>

using namespace uvfs::test;

UVFS_TEST("integrity/header_damage_is_always_caught")
{
  // The header hash is not optional, so this holds at every integrity level.
  scratch_dir dir{"hdrhash"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);

  // Byte 8 onwards is covered by the hash; the magic and version are checked
  // separately and have their own tests.
  for (std::size_t off : {hdr::flags, hdr::file_count, hdr::index_start,
                          hdr::table_capacity, hdr::index_hash})
  {
    auto patched = bytes;
    patched[off] = static_cast<char>(patched[off] ^ 0x01);
    const auto bad = dir / "bad.uvfs";
    spit(bad, patched);
    CHECK_THROWS(uvfs::reader{bad});
    CHECK_THROWS(uvfs::reader(bad, uvfs::integrity::header_only));
  }
}

UVFS_TEST("integrity/index_damage_needs_the_index_check")
{
  // A flipped bit inside a name is structurally plausible: every offset still
  // points where it should, so nothing but the hash can notice.
  scratch_dir dir{"idxhash"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());

  const auto name_byte
      = static_cast<std::size_t>(h.index_start + h.names_offset() + 1);
  auto patched = bytes;
  patched[name_byte] = static_cast<char>(patched[name_byte] ^ 0x20);
  const auto bad = dir / "bad.uvfs";
  spit(bad, patched);

  // header_only opens it: the geometry is intact and the damage is inside a
  // name. This is exactly the 44% of corruptions that used to pass silently.
  CHECK_NOTHROW(uvfs::reader(bad, uvfs::integrity::header_only));
  // ...and the index check is what catches it.
  CHECK_THROWS(uvfs::reader(bad, uvfs::integrity::index));
}

UVFS_TEST("integrity/payload_damage_is_caught_by_content_hashes")
{
  scratch_dir dir{"payhash"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());

  auto patched = bytes;
  const auto payload_byte = static_cast<std::size_t>(h.data_start + 3);
  patched[payload_byte] = static_cast<char>(patched[payload_byte] ^ 0xff);
  const auto bad = dir / "bad.uvfs";
  spit(bad, patched);

  // Damage inside a payload leaves every offset valid, so opening succeeds
  // even with the index check: the index itself is fine.
  CHECK_NOTHROW(uvfs::reader(bad, uvfs::integrity::index));

  // verify() finds it...
  {
    uvfs::reader r{bad};
    CHECK(r.has_content_hashes());
    const auto damaged = r.verify();
    CHECK_EQ(damaged.size(), std::size_t{1});
  }
  // ...and under integrity::full, reading it reports rather than returns it.
  {
    uvfs::reader r{bad, uvfs::integrity::full};
    bool threw = false;
    try
    {
      for (int64_t i = 0; i < r.count(); i++)
        (void)r.find(r.at(i).path);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    CHECK(threw);
  }
}

UVFS_TEST("integrity/intact_archive_passes_every_level")
{
  scratch_dir dir{"clean"};
  const auto arc = build_sample(dir);
  for (auto level : {uvfs::integrity::header_only, uvfs::integrity::index,
                     uvfs::integrity::full})
  {
    CHECK_NOTHROW({
      uvfs::reader r{arc, level};
      for (int64_t i = 0; i < r.count(); i++)
        CHECK(r.find(r.at(i).path).has_value());
      CHECK(r.verify().empty());
    });
  }
}

UVFS_TEST("integrity/content_hashes_can_be_turned_off")
{
  scratch_dir dir{"nohash"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.set_content_hashes(false);
  w.add_file("/a", dir.make_file("a", 100, 1));
  w.commit(arc);

  uvfs::reader r{arc};
  CHECK(!r.has_content_hashes());
  CHECK(r.verify().empty()); // nothing to check against
  CHECK(r.find("/a").has_value());

  // Asking for full checking on an archive that cannot support it is an error
  // rather than a silently weaker guarantee.
  CHECK_THROWS(uvfs::reader(arc, uvfs::integrity::full));

  // The index hash is written regardless.
  CHECK_NOTHROW(uvfs::reader(arc, uvfs::integrity::index));
}

UVFS_TEST("integrity/duplicate_entry_forged_into_the_index_is_caught")
{
  // v1 noticed duplicates because it walked the index at open. v2 does not,
  // so this is the check that replaces it: forging a duplicate necessarily
  // changes the index, and the index hash covers the index.
  scratch_dir dir{"dupidx"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/aa", dir.make_file("a", 32, 1));
  w.add_file("/bb", dir.make_file("b", 32, 2));
  w.commit(arc);

  auto bytes = slurp(arc);
  const auto e0 = entry_offset(bytes, 0);
  const auto e1 = entry_offset(bytes, 1);
  // Point the second entry's name at the first entry's name.
  const auto n0 = uvfs::load<uint32_t>(bytes.data() + e0 + ent::name_offset);
  const auto s0 = uvfs::load<uint16_t>(bytes.data() + e0 + ent::name_size);
  std::memcpy(bytes.data() + e1 + ent::name_offset, &n0, sizeof n0);
  std::memcpy(bytes.data() + e1 + ent::name_size, &s0, sizeof s0);

  const auto bad = dir / "bad.uvfs";
  spit(bad, bytes);
  CHECK_THROWS(uvfs::reader(bad, uvfs::integrity::index));
}

UVFS_TEST("integrity/mutation_sweep_with_index_checking")
{
  // The review measured 44% of single-byte corruptions being accepted as
  // valid. With the index hash on, anything that lands in the header or the
  // index has to be refused; only payload bytes can slip through to the
  // content hashes.
  scratch_dir dir{"sweep"};
  const auto arc = build_sample(dir);
  const auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  const auto index_end = static_cast<std::size_t>(h.index_start + h.index_size);

  std::mt19937_64 rng{99};
  const auto bad = dir / "mutant.uvfs";
  int structural_accepted = 0, structural_total = 0;

  for (int iter = 0; iter < 3000; iter++)
  {
    auto patched = bytes;
    // Only touch the header and index, where corruption must be detectable.
    const auto off = rng() % index_end;
    patched[off] = static_cast<char>(patched[off] ^ (1u << (rng() % 8)));
    if (patched == bytes)
      continue;
    spit(bad, patched);
    structural_total++;

    try
    {
      uvfs::reader r{bad, uvfs::integrity::index};
      std::size_t sink = 0;
      r.for_each_file(
          [&](uvfs::reader::iter_entry e)
          {
            sink += e.path.size() + e.data.size();
            return true;
          });
      structural_accepted++;
    }
    catch (const std::exception&)
    {
    }
  }
  std::printf(
      "    (%d/%d header+index corruptions accepted)\n", structural_accepted,
      structural_total);
  CHECK_EQ(structural_accepted, 0);
}

UVFS_TEST("integrity/lookup_terminates_when_the_table_has_no_empty_slot")
{
  // Found by the fuzzer. validate() checks the hash table's *capacity* against
  // the file count, but nothing checks its contents. Linear probing stopped
  // only at an empty slot, so a table corrupted to be entirely full made
  // find() of an absent key loop forever -- a hang, not a crash, which is why
  // the mutation tests never caught it: they all terminate.
  scratch_dir dir{"fulltable"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/aa", dir.make_file("a", 32, 1));
  w.add_file("/bb", dir.make_file("b", 32, 2));
  w.commit(arc);

  auto bytes = slurp(arc);
  const auto h = uvfs::header::load_from(bytes.data());
  auto* const table = bytes.data() + h.index_start + h.table_offset();
  // Fill every slot with a value that is not the empty sentinel and whose
  // fingerprint will not match, so no probe ever succeeds or stops.
  for (int64_t i = 0; i < h.table_capacity; i++)
  {
    const uint64_t occupied = 0x1234'5678'0000'0000ull;
    std::memcpy(table + i * 8, &occupied, sizeof occupied);
  }
  const auto bad = dir / "bad.uvfs";
  spit(bad, bytes);

  uvfs::reader r{bad, uvfs::integrity::header_only};
  // If the bound is missing this never returns and the test suite hangs.
  CHECK(!r.find("/definitely/not/present").has_value());
  CHECK(!r.find("/aa").has_value());
  CHECK(!r.stat("").has_value());

  // A table with no empty slot that *does* still hold a real entry must find
  // it: the bound must not cut a legitimate probe short.
  auto bytes2 = slurp(arc);
  auto* const table2 = bytes2.data() + h.index_start + h.table_offset();
  // Preserve a genuinely occupied slot, not slot 0 -- with 8 slots and 2
  // entries, slot 0 is very likely empty, and keeping an empty sentinel there
  // would stop the probe early and test nothing.
  int64_t real_slot = -1;
  uint64_t keep = 0;
  for (int64_t i = 0; i < h.table_capacity; i++)
  {
    const auto v = uvfs::load<uint64_t>(table2 + i * 8);
    if (v != uvfs::empty_slot)
    {
      real_slot = i;
      keep = v;
      break;
    }
  }
  CHECK(real_slot >= 0);
  for (int64_t i = 0; i < h.table_capacity; i++)
  {
    const uint64_t occupied = 0x1234'5678'0000'0000ull;
    std::memcpy(table2 + i * 8, &occupied, sizeof occupied);
  }
  std::memcpy(table2 + real_slot * 8, &keep, sizeof keep);
  const auto bad2 = dir / "bad2.uvfs";
  spit(bad2, bytes2);
  uvfs::reader r2{bad2, uvfs::integrity::header_only};
  int found = 0;
  for (int64_t i = 0; i < r2.count(); i++)
    found += r2.stat(r2.at(i).path).has_value();
  CHECK(found >= 1);
}
