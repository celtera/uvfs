#include "framework.hpp"

#include <uvfs/reader.hpp>
#include <uvfs/writer.hpp>

#include <utility>
#include <vector>

using namespace uvfs::test;

UVFS_TEST("api/moved_from_reader_is_usable_not_undefined")
{
  // Move leaves the pimpl null. Every accessor dereferenced it, and the cheap
  // ones are noexcept, so a moved-from reader was a segfault waiting for its
  // first method call rather than a merely useless object. Standard practice
  // is that a moved-from object is valid but unspecified -- valid means you
  // can still call things on it.
  scratch_dir dir{"moved"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  w.add_file("/a", dir.make_file("a", 128, 3));
  w.commit(arc);

  uvfs::reader source{arc};
  CHECK_EQ(source.size(), std::size_t{1});

  uvfs::reader taken = std::move(source);
  CHECK_EQ(taken.size(), std::size_t{1});

  // The moved-from object behaves as an empty archive.
  CHECK_EQ(source.size(), std::size_t{0});
  CHECK_EQ(source.count(), int64_t{0});
  CHECK(source.empty());
  CHECK(!source.has_content_hashes());
  CHECK(!source.find("/a").has_value());
  CHECK(!source.stat("/a").has_value());
  CHECK(!source.read("/a").has_value());
  CHECK_THROWS((void)source.at(0));
  CHECK(source.verify().empty());

  int visited = 0;
  source.for_each_file(
      [&](uvfs::reader::iter_entry)
      {
        visited++;
        return true;
      });
  CHECK_EQ(visited, 0);

  std::vector<char> buf(16);
  CHECK(!source.read_into("/a", buf.data(), std::ssize(buf)).has_value());

  // Move-assignment leaves the source in the same state.
  uvfs::reader other{arc};
  source = std::move(other);
  CHECK_EQ(source.size(), std::size_t{1});
  CHECK_EQ(other.size(), std::size_t{0});
  CHECK(!other.find("/a").has_value());
}

UVFS_TEST("api/readers_can_be_returned_and_stored")
{
  scratch_dir dir{"container"};
  const auto arc = dir / "out.uvfs";
  uvfs::writer w;
  for (int i = 0; i < 5; i++)
    w.add_file("/f" + std::to_string(i), dir.make_file("f" + std::to_string(i), 64, i));
  w.commit(arc);

  auto make = [&] { return uvfs::reader{arc}; };
  std::vector<uvfs::reader> readers;
  for (int i = 0; i < 3; i++)
    readers.push_back(make());
  // Growing the vector moves the readers; every one must still work.
  for (auto& r : readers)
  {
    CHECK_EQ(r.size(), std::size_t{5});
    CHECK(r.find("/f3").has_value());
  }
}
