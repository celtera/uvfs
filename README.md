# uvfs

A microscopic archive format for data you want to *point at* rather than unpack.

Opening a uvfs archive is one `mmap` and one header check. Looking a file up is
one hash and about one cache miss. If the file is stored uncompressed, what you
get back is a 64-byte-aligned pointer straight into the mapping — no copy, no
allocation, and the index is shared through the page cache with every other
process that opened the same archive.

```cpp
uvfs::reader r{"assets.uvfs"};

if (auto bytes = r.find("/audio/kick.wav"))
    play(bytes->data(), bytes->size());   // points into the mapping
```

Opening a 44,691-file archive takes **9 µs** and adds nothing to the heap.

## Why not just use a zip

Mostly you should. uvfs exists for one specific shape of problem: a read-mostly
blob of assets, opened often, where you want a pointer rather than a buffer.

The concrete thing uvfs does better is where the payload offsets live. ZIP's
central directory records the offset of each *local header*, and each local
header carries its own independently-sized extra field — so a random-access ZIP
reader has to touch a byte near every entry, scattered across the whole archive,
before it can answer the first question. Measured on the same 466 MB corpus:

| reader                       | open, warm | open, cold | resident after open |
| ---------------------------- | ---------: | ---------: | ------------------: |
| uvfs                         |     `9 µs` |   `124 µs` |                `0`  |
| zip (store), lazy offsets    |  `1174 µs` |  `2122 µs` |            `5.8 MB` |
| zip (store), eager offsets   | `11098 µs` |`365321 µs` |             `344 MB`|

What uvfs does *not* do: no permissions, no timestamps, no ownership, no
symlinks, no directories, no updates in place. It stores bytes under names. If
you need a real archive format, use tar or zip; if you need a read-only
filesystem, use EROFS or SquashFS.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Requires C++20. zstd is optional — without it, uvfs still reads and writes
uncompressed archives and refuses compressed entries with a message saying why.

| option                  | default | meaning                                  |
| ----------------------- | ------- | ---------------------------------------- |
| `UVFS_WITH_ZSTD`        | `ON`    | compression support                      |
| `UVFS_BUILD_TESTS`      | top-level | the test suite                         |
| `UVFS_BUILD_TOOLS`      | top-level | the `uvfs` command line tool           |
| `UVFS_BUILD_BENCHMARKS` | `OFF`   | `uvfs_bench`                             |
| `UVFS_BUILD_FUZZERS`    | `OFF`   | libFuzzer target for the reader (clang)  |
| `UVFS_SANITIZE`         | *empty* | e.g. `address,undefined`                 |

## Command line

```sh
uvfs create assets.uvfs --compress --strip=/data /data
uvfs list    assets.uvfs
uvfs verify  assets.uvfs
uvfs extract assets.uvfs ./out
```

## Compression

Compression is a property of each *entry*, not of the archive, because real
asset data is mixed: a pile of already-compressed audio and video next to a pile
of very compressible JSON and text.

`compression::automatic` compresses, then keeps the result only if it saved at
least `min_gain_percent` (default 10%). Anything that does not compress falls
back to being stored — which means it keeps its zero-copy pointer, and reading
it costs nothing. That is the setting you want for asset packs.

```cpp
uvfs::writer w;
uvfs::compression_settings cs;
cs.method = uvfs::compression::automatic;
cs.dictionary_size = 110 * 1024;   // optional, helps many small files
w.set_compression(cs);
```

Measured on an asset-pack corpus — 620 MB of incompressible media plus 2,000
small JSON files:

| policy      |     size | media kept zero-copy | read everything back |
| ----------- | -------: | -------------------: | -------------------: |
| `none`      | `653 MB` |          `190 / 190` |             `0.04 s` |
| `automatic` | `650 MB` |          `190 / 190` |             `0.04 s` |

Every media file stays a pointer; every JSON file shrinks. On text-heavy data
the picture is the opposite — `/usr/include`, 44,691 files, 465.9 MB:

| policy               |     size | of raw |
| -------------------- | -------: | -----: |
| `none`               | `472 MB` | 101.3% |
| `automatic -3`       | `101 MB` |  21.6% |
| `automatic -9`       |  `93 MB` |  19.9% |
| `automatic -9` +dict |  `79 MB` |  17.0% |

Large payloads are decided from a 256 KB sample rather than by compressing the
whole thing, and payloads too big to stage are streamed, so a multi-gigabyte
video is neither compressed pointlessly nor held in memory.

Output is byte-for-byte reproducible: compression runs in parallel, but offsets
are assigned in sorted order.

The same file added under several names, or reached through hard links, is
stored once and pointed at by every entry that uses it. The format never
required payload offsets to be distinct, so this costs a hash lookup at write
time and nothing at all at read time.

## Integrity

The threat model is *corruption*, not an adversary: bad disks, truncated
downloads, half-written files. So the hash is [XXH3](https://xxhash.com/) —
non-cryptographic, and measured here at 14.3 GB/s.

Where the hashes sit matters more than which hash it is, because hashing
everything at open would throw away the one thing the format is for:

| level                    | what it checks                        | cost on a 466 MB archive |
| ------------------------ | ------------------------------------- | -----------------------: |
| `header_only` *(default)*| the 128-byte header, and the dictionary if present |      `3 µs` |
| `index`                  | \+ entries, hash table, names          |                 `175 µs` |
| `full`                   | \+ each payload as it is read          |    proportional to reads |
| `verify()`               | every payload, on demand               |                 `32.7 ms`|

The dictionary is not on that ladder: it is checked at open whatever the level,
because every entry that uses it depends on it, and a damaged dictionary
changes what those entries decode to without touching a single stored byte that
any other checksum covers.

Memory safety never depends on any of this: index entries are bounds-checked
where they are used, at every level. The levels buy detection of corruption that
is *structurally plausible* — a flipped bit inside a name, an offset that still
lands in the data region but on the wrong payload.

## Format

Little-endian. All offsets are absolute unless stated.

```
header   128 bytes
index    entries, sorted by name, 32-byte stride
         hash table, power of two, open addressing
         content hashes, 8 bytes per entry     (optional)
         name blob
dict     zstd dictionary                       (optional)
data     payloads
```

### Header

| offset | size | field                                              |
| -----: | ---: | -------------------------------------------------- |
|    `0` |  `8` | `"UVFS\0\0\0\2"` — magic, 3 reserved, version      |
|    `8` |  `4` | flags: `1` content hashes, `2` dictionary          |
|   `16` |  `8` | total file size, must equal the file on disk       |
|   `24` |  `8` | file count                                         |
|   `32` |  `8` | index start / `40` index size                      |
|   `48` |  `8` | data start / `56` data size                        |
|   `64` |  `8` | hash table capacity (power of two, or 0)           |
|   `72` |  `8` | name blob size                                     |
|   `80` |  `8` | dictionary start / `88` dictionary size            |
|   `96` |  `8` | XXH3 of the index region                           |
|  `120` |  `8` | XXH3 of bytes `[0, 120)`                           |

### Entry — 32 bytes, fixed stride

| offset | size | field                                          |
| -----: | ---: | ---------------------------------------------- |
|    `0` |  `8` | payload offset, relative to data start         |
|    `8` |  `8` | stored size — bytes occupied in the archive    |
|   `16` |  `8` | original size — bytes after decompression      |
|   `24` |  `4` | name offset into the blob                      |
|   `28` |  `2` | name length                                    |
|   `30` |  `1` | codec: `0` store, `1` zstd, `2` zstd+dictionary|
|   `31` |  `1` | reserved                                       |

Entries are sorted by name, so the array is binary searchable and index order
*is* extraction order. The table stores `(fingerprint << 32) | entry index`,
with `~0` for empty; the fingerprint lets a colliding probe be rejected without
touching the entry or the name blob.

Stored payloads are 64-byte aligned so they can be handed to SIMD, DMA or audio
code directly. Compressed payloads are 8-byte aligned — they get copied out
anyway.

### Paths

A path is UTF-8, `/`-separated, at most 65535 bytes, with no empty components,
no `.` or `..`, no NUL and no backslash. A leading `/` is allowed and kept
verbatim; these are lookup keys, not filesystem paths.

uvfs never resolves them, but anything extracting an archive joins them onto an
output directory, so the rule is enforced at write time and exposed as
`uvfs::is_safe_archive_path()` for extractors to apply themselves.

## Performance

Threadripper PRO 7965WX, NVMe, GCC 13, `/usr/include` — 44,691 files, 465.9 MB.

| operation                                | uvfs        |
| ---------------------------------------- | ----------: |
| open (any archive size)                  |      `9 µs` |
| lookup, steady state                     |     `39 ns` |
| open + read 10 files                     |     `66 µs` |
| build, uncompressed                      |    `0.42 s` |
| build, `automatic -3`                    |    `0.23 s` |
| heap used by the index                   |         `0` |

Opening a 50,000-entry archive costs no more than opening a 100-entry one —
there is no per-entry work at open, which is the whole point of the layout.
`open` measures the same 2.1–2.4 µs on a 2,190-file archive and a 200,000-file
one. Loading an archive that carries a compression dictionary costs ~10 µs
instead, because zstd has to build its decoding tables.

Overhead depends heavily on how big the files are. Per entry uvfs spends 32
bytes of index, 8 bytes of hash table, 8 bytes of content hash, the path
itself, and up to 63 bytes of alignment padding:

| corpus                          | payload  | archive  | overhead |
| ------------------------------- | -------: | -------: | -------: |
| asset pack, 2,190 files (~300 KB each) | `652.9 MB` | `653.2 MB` |  `+0.0%` |
| `/usr/include`, 44,691 files (~10 KB)  | `465.9 MB` | `472.0 MB` |  `+1.3%` |
| 200,000 files of ~207 bytes            |  `41.5 MB` |  `72.2 MB` | `+73.9%` |

At a couple of hundred bytes per file the per-entry cost dominates and uvfs is
a poor fit — though even there it beats `zip -0` (80.7 MB) and `tar` (204.8 MB)
on the same corpus, since both spend more per entry than uvfs does. If your
files are that small, pack them into fewer, larger ones.

Writing is bound by per-file syscalls rather than CPU, so the copy phase caps
itself at 12 threads (48 threads costs 4.6× the system time for no gain).
Compression is CPU-bound and uses every thread available.

## Limits

- 2³¹−2 files per archive.
- 65535 bytes per path, and 4 GiB of archive paths in total (a 32-bit offset
  per entry into one name blob). Both are enforced at write time.
- Linux, macOS, FreeBSD, Windows, Emscripten and generic POSIX are supported.
  OpenBSD and NetBSD build on the generic path.
- Little-endian only, checked at compile time.
- No metadata: no mode, mtime, ownership, symlinks or directories.
- Payloads are shared between entries that name the same file (same inode) or
  the same file added twice, but identical *content* under different inodes is
  stored more than once. Content dedup would have saved 4.2% on `/usr/include`
  in exchange for hashing every input before laying the archive out, which
  doubles read I/O; that trade is not worth making by default.
- Per-entry overhead is ~50 bytes plus alignment padding, which is significant
  for files of only a few hundred bytes (see above).
- Archives are written whole; there is no append or update in place.

## Supported platforms

Built and tested in CI on every push:

| | |
| --- | --- |
| Linux x86_64 / arm64 | gcc 12–16, clang 17–21, libstdc++ and libc++ |
| libc | glibc and musl (Alpine), x86_64 and arm64 |
| macOS | arm64 (latest) and x86_64 (13) |
| BSD | FreeBSD, OpenBSD, NetBSD |
| Windows | MSVC x86_64 and arm64, MinGW (run under wine) |
| WebAssembly | Emscripten, run under node |
| Standards | C++20, C++23, C++26 |
| Sanitizers | ASan, UBSan, TSan, valgrind, `_GLIBCXX_DEBUG` |

Plus, on every push: clang-tidy and cppcheck as errors, clang-format, a
libFuzzer run over the reader, byte-for-byte reproducibility, a command line
round trip across every compression setting, installed-package and
`add_subdirectory` consumption, a build with zstd disabled, and a check that
the little-endian guard actually fires on s390x rather than silently writing
byte-swapped archives.

Each platform gets its own fastest primitives rather than a common denominator:
`copy_file_range` on Linux and FreeBSD, `F_PREALLOCATE` and `F_FULLFSYNC` on
macOS, `CreateFileMapping` and positioned `ReadFile` on Windows. Emscripten has
no writable shared mapping at all, so archives are staged in memory and written
out; the writer does not know the difference.

## Licence

See `LICENSE`.
