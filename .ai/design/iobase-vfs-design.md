# IOBase (vn::io) core capabilities: VFS and Stream

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: `Vfs` / `Stream` / the error model / paths / backends / MountVfs; migration and gates.
> **Sources**: `src/base/iobase/sdk/vine/io/**`, `tests/test_iobase`.
> **Backlog**: `../backlog/`

## 1. Overview

### 1.1 Problem and background

The old VFS answered only `bool`, had no `stat`, could not read or write partially, and its write permission was unpredictable; opening a zip kept the whole archive in memory;
`DirectoryVfs` rejected `..` by substring only, so an absolute segment could escape the root. The requirements are: distinguishable errors, a lazy zip, streaming read and write, and several backends under one tree.

### 1.2 Goals

- `IoError` + `Result<T>`: a business failure travels in the return value; an exception is only for a programming error.
- Path validation converges on `normalizeVfsPath`: no working directory, absolute spellings refused, `..` cancelled segment by segment.
- The `Vfs` family: `DirectoryVfs` / `ZipArchive` / `MountVfs`; a lazy zip index + an overlay + an atomic commit.
- Read streams / push sinks / write sources: a large payload does not have to sit in memory as one piece.
- Not thread-safe at the method level and no internal lock; a shared object is synchronised by the caller.

### 1.3 Non-goals

- No `VfsPath` value type; no core `IStream` hierarchy.
- No `MemoryVfs` (`ZipArchive()` with no source tree already covers it).
- No TAR/GZIP distinction, no cache, no HTTP/ACL/encryption/deduplication.
- No push writing (a libzip source is pull).

### 1.4 One-paragraph conclusion

`vn::io` distinguishes failure with `Result<IoError>`, addresses everything through a normalized virtual path, and covers directory / zip / mount with the three `Vfs` backends;
a large payload travels through `openRead`/`read(sink)`/`addFile(source)`, and the medium is touched only by `commit`/`saveAs`.

## 2. Design Points

### 2.1 Error model

- `IoError`：`NotFound` / `AlreadyExists` / `PermissionDenied` / `NotADirectory` / `IsADirectory` /
  `InvalidPath` / `ReadOnly` / `IoFailure` / `Unsupported` / `InvalidData` / `Closed` / `OutOfRange` /
  `CapacityExceeded` / `NotEmpty`。
- `Result<T>`: implicitly constructs from a success value or an `IoError`; `ok()`, `error()`, `value()`, `take()`.
- No `and_then`/`map` combinators; `T=void` uses `IoError` directly.
- A value rather than an exception; exceptions are still used for `bad_alloc` and for misuse (`invalid_argument`).

### 2.2 Path validation

`detail::normalizeVfsPath(path, out)` returns `IoError`:

- An empty path = the root; empty segments, repeated separators, a trailing `/` and `.` are folded away.
- A leading `/` is refused (a virtual path has no working directory).
- `\`, an embedded `\0` and a `:` segment are refused (drive letter / UNC / URI).
- `..` cancels a segment; cancelling past the root → `InvalidPath`.
- The ZIP archive entry accepts an old package that writes `\` as the directory separator; it maps to a `/` virtual path, a path outside the root is refused, a duplicate name invalidates the archive, and an unmodified entry keeps its original name when repackaging.
- A non-text entry name is decoded as “valid UTF-8 → local code page → byte-for-byte same-value code points”, and the archive's original bytes are written back unchanged when repackaging.

### 2.3 Backend family

```
Vfs
 ├── DirectoryVfs   a real directory; a write lands at once, commit() = Ok
 ├── ZipArchive     ZIP: a lazy index + a change overlay + commit/saveAs/toBytes
 └── MountVfs       several backends under one tree (longest prefix + priority + read-only)
Zip                 one-shot compress/decompress/entries/readEntry
```

- The three ways to open a `ZipArchive`: `open(..., ReadOnly)` / `open(..., ReadWrite)` / the default constructor (an in-memory tree with no source).
- The source `zip_t` is read-only throughout; the destination `zip_t` is opened only by `commit`/`saveAs`.
- `commit()`: a temporary file + an atomic replace + an index rebuild; `saveAs(path|ostream)` / `toBytes()` write elsewhere.
- An unmodified entry is carried over by `zip_source_zip_file`; peak memory = one entry.
- The read path computes its own CRC against the directory record; a mismatch → `IoFailure`.

### 2.4 Streams and zero copy

- `Stream.hpp`: `Fragment` / `DataSource` / `DataSink` / `kUnknownSize`, with no Vfs name in it.
- `Vfs.hpp`: `VfsEntrySource` (a `DataSource` subclass).
- `Adapters.hpp`：`IstreamSource` / `OstreamSink` / `Conversion<In,Out>`。
- Read: `openRead(path)` returns a `VfsEntrySource`; `read(path, DataSink&)` is the push variant (the base class chunks at 64 KiB).
- Write: `addFile(path, span)` (one copy) / `span<Fragment>` (none) / `shared_ptr<DataSource>` (none; pulled at save) /
  `real_path` (none; read at save).
- `kUnknownSize` is allowed: libzip goes zip64 and backfills the local header after writing; the destination must be seekable.
- One source instance attaches to one entry only; the callback runs on the saving thread.

### 2.5 MountVfs

- The longest prefix wins; equal prefixes order by descending priority; a write routes to the first writable backend.
- Enumeration merges and de-duplicates, and fills in the intermediate directories (with `/data/a` and `/data/b` mounted, listing `/data` shows a and b).
- A cross-backend `rename` = read the source → write the destination → remove the source; a failure keeps the source.
- A single-backend hit is forwarded directly; a backend mounted at several points gets one `commit()` call; a read-only mount is skipped.

### 2.6 Concurrency

- Not thread-safe at the method level and no internal lock; a shared object, or two objects sharing storage, is synchronised by the caller.
- Objects that share nothing (buffer / handle / storage) may run concurrently.
- `DirectoryVfs` has no file lock and the last writer wins; `ZipArchive::commit()` uses a fixed temporary name, so a concurrent commit on one file is not supported.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | error model | `IoError` + `Result<T>` | landed |
| F2 | path validation | `normalizeVfsPath` | landed |
| F3 | file information | `stat` / `list` / `VfsEntryInfo` | landed |
| F4 | read-only declaration | `isReadOnly()`; a write answers `ReadOnly` at once | landed |
| F5 | directory / file operations | create/rename/remove/removeAll | landed |
| F6 | read streams / push | `openRead` / `read(sink)` / `copy` | landed |
| F7 | write sources | `addFile(span/fragments/source/real)` | landed |
| F8 | backend family | Directory/Zip/Mount | landed |
| F9 | memory stream | `reserve` + a capacity limit | landed |

### 3.1.1 F1 error model

- **Behaviour**: a business failure returns `IoError`; `Result<T>` carries a value or an error.
- **References**: C1, R1, T1, N1.

### 3.1.2 F2 path validation

- **Behaviour**: `normalizeVfsPath` folds legal segments and refuses illegal ones.
- **References**: C2, R2, T2, P2, P4.

### 3.1.3 F3 file information

- **Behaviour**: `stat(path)` returns a `VfsEntryInfo` (a directory has `size=0`, `kind` is only File/Directory, `crc` is 0 when unrecorded); `list(dir)` returns full paths that can be fed straight back.
- **References**: C3, R3, T3, P5.

### 3.1.4 F4 read-only declaration

- **Behaviour**: `isReadOnly()`; a write or change checks it on the first line and answers `ReadOnly` at once.
- **References**: C4, R4, T4.

### 3.1.5 F5 directory / file operations

- **Behaviour**: `createDirectory(s)` / `rename` / `remove` / `removeAll`.
- **References**: C5, R5, T5, P6.

### 3.1.6 F6 read streams / push

- **Behaviour**: `openRead` returns a `VfsEntrySource`; `read(path, sink)` pushes in chunks; `copy` carries across trees.
- **References**: C6, R6, T6, P7.

### 3.1.7 F7 write sources

- **Behaviour**: four `addFile` overloads; a `Fragment` is borrowed, a `DataSource` is pulled, a real file is read late.
- **References**: C7, R7, T7, P8.

### 3.1.8 F8 backend family

- **Behaviour**: `DirectoryVfs` / `ZipArchive` / `MountVfs`.
- **References**: C8, R8, T8, P3.

### 3.1.9 F9 memory stream

- **Behaviour**: `reserve` + `setCapacityLimit` of `MemoryStreamBuf` / `ChunkedMemoryStreamBuf`.
- **References**: C9, R9, T9, P9.

## 4. Public Interface

### 4.1 `Vfs`

| Interface | Contract | Notes |
| --- | --- | --- |
| `stat` / `list` / `kindOf` | file information | `Result` |
| `isReadOnly` | the read-only declaration | a write checks it first |
| `createDirectory(s)` / `rename` / `remove` / `removeAll` | directory / file operations | `IoError` |
| `openRead` / `read(sink)` / `copy` | reading | a stream may outlive the VFS |
| `addFile(span/fragments/source/real)` | write sources | borrowed / pulled |
| `commit` / `saveAs` / `toBytes` | reaching the medium | the destination decides |

### 4.2 Errors / information / streams

| Interface | Contract | Notes |
| --- | --- | --- |
| `IoError` / `Result<T>` | a value, not an exception | `ioErrorName()` |
| `VfsEntryInfo` | path/kind/size/crc/name | a directory has size=0 |
| `DataSource` / `DataSink` / `Fragment` | the byte-stream vocabulary | no Vfs name in it |
| `MountVfs` | several backends under one tree | longest prefix / priority |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | a business failure travels in the return value; an exception is only for a programming error. |
| C2 | a virtual path has no working directory; a leading `/`/`\`/`:` and a `..` past the root are refused. |
| C3 | `stat`/`list` describe an existing entry only; an entry that exists but is neither file nor directory answers `NotFound`, and absence is carried by `Result`'s error code. |
| C4 | a write on a read-only backend answers `ReadOnly` at once and does not touch the backend. |
| C5 | removing a non-empty directory answers `NotEmpty`; a failed cross-backend rename keeps the source. |
| C6 | a read stream may outlive the VFS; a push sink that refuses is returned as it is. |
| C7 | borrowed data lives until the medium is reached; `DataSource::size()` must be accurate. |
| C8 | a lazy zip index + overlay; the medium is touched only by `commit`/`saveAs`. |
| C9 | the memory-stream limit constrains growth only; a failed `reserve` leaves the buffer unchanged. |
| C10 | a backslash in a ZIP entry name is read as a separator at the archive boundary; an external VFS path still accepts `/` only. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | `ioErrorName()` is readable; diagnostics and tests use it. |
| R2 | a backend re-validates an absolute path before it lands. |
| R3 | `list` returns full paths that can be fed straight back. |
| R4 | read-only mount points and read-only backends both take part in the read-only decision. |
| R5 | path validation comes first; a failure keeps the source. |
| R6 | a read stream may answer a short read (0 = EOF); `error()` explains the 0 bytes. |
| R7 | one source attaches to one entry; the callback runs on the saving thread. |
| R8 | `commit()` uses a temporary file + an atomic replace + an index rebuild. |
| R9 | even a single-character write obeys the capacity limit. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | `NotFound` looks at `file_type` first and `error_code` second. |
| N2 | a non-text entry name may be decoded as UTF-8 → local code page → byte code point; repackaging writes the original bytes back. |
| N3 | the zip read path computes its own CRC; a mismatch → `IoFailure`. |
| N4 | not thread-safe at the method level; a shared object is synchronised by the caller. |
| N5 | after separator normalization, a duplicate name or a path outside the root refuses to open the archive; the original stored name is used only to repack an unmodified entry losslessly. |

## 6. Caveats and Traps

| # | Trap | How it fails | How to detect / prevent it |
| --- | --- | --- | --- |
| P1 | `error_code` decides `NotFound` | MSVC sets `ec` even when the path does not exist, so "not found" is misreported as `IoFailure`. | `stat`/`list` look at `file_type::not_found` first; write per N1 and pin it with a cross-platform case. |
| P2 | an absolute segment escapes | `C:/…` as the right operand of a join replaces the left one. | `normalizeVfsPath` refuses a `:` segment and a leading `/`; path validation keeps one entry point. |
| P3 | zip CRC | libzip's read path does not verify the CRC, so a corrupt entry quietly yields wrong bytes. | compute `crc32()` yourself against the directory record; a mismatch is `IoFailure`; `readEntries` takes `ZIP_STAT_CRC` and compares at the end of the stream. |
| P4 | a substring test for `..` | rejecting `..` by substring kills `a..b` and can still miss a real escape. | cancel segment by segment; `normalizeVfsPath` is the only entry (`C2`). |
| P5 | `VfsEntryInfo::path` taken for a name | using the full path as a name builds a doubled path. | names go through `name()` (`R3`). |
| P6 | `rename` overwriting a name | overwrite semantics follow the backend, and `DirectoryVfs` differs from the zip overlay. | `stat` first when "do not overwrite" is wanted; a failed cross-backend rename keeps the source (`C5`). |
| P7 | seeking in a compressed entry | every seek decompresses that entry from the start. | read and write sequentially; for repeated seeks, decompress into a memory stream first. |
| P8 | `kUnknownSize` | the zip64 backfill fails when the destination is not seekable. | write an unknown length to a seekable destination only, otherwise buffer first (`C7`). |
| P9 | `release()` / `clearData()` | assuming clearing the content also resets the limit. | the limit is independent of the content; the move constructor / assignment carry it (`C9`). |

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | a `VfsPath` value type | the gain of compile-time protection against a raw string does not pay for changing every API; validation was the gap. | — |
| D2 | a core `IStream` hierarchy | `Result<VfsStream>` + `intrusive_ptr<Buffer>` is already enough. | — |
| D3 | a `MemoryVfs` type | `ZipArchive()` with no source tree + `toBytes()` already covers it; it cannot be lazy. | — |
| D4 | one name `save(path)` | writing back to itself and saving elsewhere are different targets; split into `commit`/`saveAs`. | — |
| D5 | push writing `openWrite()` | the libzip source is pull; push could only buffer or spool. | — |
| D6 | `toBytes()`/`saveAs(ostream&)` for a large package | the peak is about 2× the package; a large package goes through `saveAs(path)`/`commit`. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | error model / `Result` | `test_iobase` |
| T2 | path validation / an absolute segment / `..` | `test_iobase` |
| T3 | `stat`/`list`/`kindOf` | `test_iobase` |
| T4 | a read-only refusal | `ReadOnlyBackendRefusesEveryChange` |
| T5 | directory / file operations | `test_iobase` |
| T6 | lazy zip / CRC / streams | `ZipArchiveOpensWithoutReadingContent` |
| T7 | `addFile`/`copy`/`kUnknownSize` | `test_iobase` |
| T8 | MountVfs merging / cross-backend rename | `MountVfsTest` |
| T9 | the memory-stream limit / reserve | `MemoryStreamTest` |
| T10 | ZIP backslash compatibility, a path escape and a collision after normalization | `ZipArchiveTest.ReadsWindowsSeparatorsAsArchivePathSeparators`, `RejectsArchiveNamesThatCollideAfterSeparatorNormalization`, `RejectsWindowsSeparatorPathTraversalInArchiveNames` |

## 9. Status and Evolution

### 9.1 Current Status

The error model, path validation, stat/list, read-only, directory operations, streams/sources, ZipArchive, MountVfs and the memory-stream limit are implemented.

### 9.2 Known Gaps and Backlog

- TAR/GZIP distinction, cache, HTTP/ACL/encryption/deduplication: not done.
- `saveAs(ostream&)`/`toBytes()` are not usable for a large package; the 2× peak of `toBytes()` can be removed when a real consumer appears.
- a concurrent `commit()` on one file is not supported (a fixed temporary name).
- the core `IStream` layering is not done; the trigger for moving `Stream.hpp` down to core is the first consumer that does not depend on iobase.

### 9.3 Compatibility and Migration

The public surface breaks with the old VFS: `IMemoryVfs` → `Vfs`; there is no `ZipMemoryVfs`; `save(path)` → `saveAs`/`toBytes`;
there is no `bool` family. New code uses the §4 interfaces only.

## 10. References

### 10.1 Code

- `src/base/iobase/sdk/vine/io/Vfs.hpp`
- `src/base/iobase/sdk/vine/io/Stream.hpp`
- `src/base/iobase/sdk/vine/io/Adapters.hpp`
- `src/base/iobase/sdk/vine/io/MountVfs.hpp`
- `src/base/core/sdk/vine/MemoryStream.hpp`
- `tests/test_iobase`

### 10.2 Documents and ADRs

- `robotics-io-design.md`
- `appfw-document-model.md`
- `../backlog/`

### 10.3 External Sources

- the semantics of libzip's `zip_source_*` / `zip_file_*`.
- the cross-platform differences of `std::filesystem`'s `file_type` / `error_code`.
