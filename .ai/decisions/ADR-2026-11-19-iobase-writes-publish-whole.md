# ADR-2026-11-19 iobase writes publish whole

- Status: Accepted
- Date: 2026-11-19
- Related: `.ai/design/iobase-vfs-design.md` (2.3, 5.1 R10, 6 P12)

## Background

Three of the four write paths in `vn::io` could leave a file that nobody had asked for:

- `ZipArchive::saveAs(path)` opened the target with `ZIP_CREATE | ZIP_TRUNCATE`, so an existing package was destroyed
  the moment the first byte was written - and a save that failed halfway (a file-backed entry whose file had vanished,
  a full disk) left a truncated archive where a valid one had been.
- `DirectoryVfs::addFile(path, bytes)` opened the target with `std::ios::trunc` and wrote into it, so a failed write
  left a partial file under a real name.
- `DirectoryVfs::addFile(path, real_path)` copied straight onto the target, with the same outcome.
- Only the streaming `DirectoryVfs::addFile(path, source|fragments)` and `ZipArchive::commit()` staged first, so the
  same class promised four different durabilities depending on which overload a caller happened to use.

The staging file was also a fixed sibling name (`<target>.vine-tmp`), which meant two writers of one target staged into
the same file, and a killed process left something that `list()` reports and `addDirectory()` imports.

## Options

1. **Document that only `commit()` is atomic.** Cost: the caller has to know which of four overloads it is calling to
   know whether a failure can destroy its previous file - a promise that depends on an implementation detail (G4, G6).
2. **Stage in the target's directory under a fixed name**, as `commit()` did. Cost: concurrent writers corrupt each
   other's staging file, and a crash leaves an entry that looks like content (G10).
3. **Stage in the system temporary directory, then rename.** Cost: the rename crosses a file-system boundary on many
   setups, so the publish is not atomic - exactly what the staging exists to provide.
4. **Stage in a uniquely named sibling and rename it into place**, in every path that replaces a file.

## Decision

Chose **4**:

- `detail::temporaryPathFor(target)` (in `VfsInternal.hpp`) names the staging file `<target>.vine-tmp-<pid>-<n>`, unique
  per process and per call; the name is the one place the suffix exists.
- `DirectoryVfs::addFile` - all three spellings - and `ZipArchive::saveAs` write into that staging file and rename it
  onto the target at the end. `commit()` keeps its own staging (it must rename onto the source path) and renames only
  after `saveAs` has published its own.
- On a failed publish the staging file is removed, and on a failed `commit()` rename the source archive is re-opened
  (`reopenSource()`) so the object keeps serving the entries it copies from it instead of turning them unreadable.

Renaming over an existing file is atomic on POSIX and `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` on Windows, so a reader
of the target sees the old file or the new one.

## Consequences

- A failed save of any kind leaves the previous file whole, whatever overload the caller used, and no partial file ever
  appears under a real name.
- Two writers of one target no longer share a staging file; the last rename still wins, so concurrent commits of one
  file still have to be serialized by the caller (documented on `commit()`).
- An interrupted process can leave a `<target>.vine-tmp-<pid>-<n>` sibling; it is recognizable, never collides, and
  sweeping it is the caller's business (this module has no startup hook).
- A `commit()` that cannot rename now costs one reopen. That is the price of an archive that stays usable after a
  refusal; the alternative was an object whose every source-backed entry had silently become unreadable.

## Evidence

- `ZipArchiveTest.SaveAsKeepsAnExistingTargetWhenTheContentFails` - a save whose content fails leaves the previous
  package openable and whole.
- `AdaptersTest.DirectoryVfsPublishesNothingWhenAStreamingSourceFails` (existing) and the `addFile` target rules
  (`VfsCoreTest.AddFileRefusesEveryTargetThatIsNotFree`) pin the new paths; the full `test_iobase` suite is green.
