# ADR-2026-11-19 iobase bounds a whole-content read

- Status: Accepted
- Date: 2026-11-19
- Related: `.ai/design/iobase-vfs-design.md` (2.3, 5.3 N7)

## Background

A ZIP directory states the size of every entry, and libzip hands that number out as a `zip_uint64_t`. Three read paths
believed it:

- `ZipArchive::readStored` allocated `entry.info.size` bytes before reading a single byte.
- `Zip::readEntry` (file and memory overloads) did the same with `zip_stat.size`.
- `Vfs::addFile(path, fragments|source)` on a buffering backend assembled the content in one vector with no bound.

`Zip::decompress` had a 1 GiB limit and was the only path that did. So a 1 KiB archive announcing a 2^60-byte member
turned a read into an allocation request, and the failure a caller saw was a `std::bad_alloc` escaping a function that
reports failures as values (G4, G10).

## Options

1. **Trust the directory, as libzip does.** Cost: a malformed or hostile archive chooses the process's allocation size.
2. **Decompress and check the real size after the fact.** Cost: the allocation that has to be refused is exactly the one
   that happens first.
3. **Give every read a configurable limit.** Cost: a setting every caller has to reason about, for a bound only the
   untrusted path needs (G7, G8).
4. **One bound for every "the whole thing at once" call, streaming as the escape hatch.**

## Decision

Chose **4**, with a single constant:

- `detail::kMaxWholeContentBytes` (1 GiB, the limit `Zip::decompress` already enforced) is the bound for
  `Zip::decompress`, `Zip::readEntry`, `ZipArchive::readStored`, `ZipArchive::contentOf` (a pulled generator) and the
  buffering `Vfs::addFile` overloads.
- Passing the bound answers `IoError::CapacityExceeded`, a failure the caller can act on: the entry is not damaged, it
  is too big for one buffer.
- `openRead()`, a sink, `saveAs` and a staging file stay unbounded: the bound limits one call's memory, never what a
  tree can store or hand out.
- The CRC is folded in chunks (`detail::crc32Of`), because the same "a 32-bit length is not a size" mistake silently
  truncated the checksum of any entry past 4 GiB and turned a correct entry into `IoFailure`.

## Consequences

- An archive's stated size can no longer choose a process's allocation; a hostile or broken archive fails the read it
  belongs to.
- A legitimate entry larger than 1 GiB has to be read through `openRead()` - documented on `Vfs::read`,
  `Zip::readEntry` and `ZipArchive::read`, and stated as `N7` in the design page.
- `Zip::compress`/`decompress` no longer pass a size through `uLong`, so a buffer larger than 4 GiB is compressed (and
  decompressed) whole on every platform instead of being silently cut on Windows.
- `ZipArchive::read` of a generated entry keeps what it pulled: a source that stated no length cannot be pulled twice,
  so reading an entry and then saving the archive used to write it out empty.

## Evidence

- `test_iobase` is green, including `IoBaseTest.CompressRoundTrip`, `IoBaseTest.DecompressRejectsGarbage` and
  `IoBaseTest.ZipReadEntryVerifiesTheChecksum` over the streamed codec.
- `ZipArchiveTest.ReadingAGeneratedEntryKeepsItForTheSave` - a generated entry read before the save still holds its
  content afterwards.
