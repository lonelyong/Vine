# imageio: the public codec surface for image containers

> **Status**: current
> **Last verified**: 2026-10-27
> **Scope**: the public API of `src/loaders/imageio`: the container vocabulary, the capability table, decoding, encoding and failure classification.
> Not the data shape of `imaging::Image` / `PixelFormat` (see `imaging-design.md`) , and not VFS / Stream (see `iobase-vfs-design.md`).
> **Sources**: `src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp`, `tests/test_imageio`.
> **Backlog**: see 9.2.

## 1. Overview

### 1.1 Problem and background

The codec has one channel only: stb's memory API (`STBI_NO_STDIO`), while this module does the file I/O with `std::filesystem` + `fstream`.
Once ported, three problems on the public surface showed up:

1. **failure is an exception**, while `meshio` / `brepio` on the same layer have already unified on `Result` / `IoError`: three modules on one layer, two failure channels,
   and a caller has to remember both conventions. The only consumer (the cube map in `DemoScene.cpp`) uses `try/catch` only to print the reason and skip.
2. **four copies of one fact**: `formatName()` / `formatFromPath()` / `canRead()` / `canWrite()` each had its own `switch`,
   so adding a container meant four edits, and the copies can drift (the "can read" table lists it, the extension table forgets it).
3. **the file dialog had no single source**: `meshio` has `supportedExtensions()`, `imageio` had none, so any dialog had to hand-write its own extension list.

### 1.2 Goals

- A container is an enum and its capability comes from **one table**: the name, the extensions, readability and writability all come from it, and the dialog's extension list is generated from it too.
- Failure is a value: a read answers `Result<T>`, a write answers `IoError`, the same vocabulary as `meshio` / `brepio`.
- One set of entry points: reading the public header tells you what the module can do; the module is **stateless**.

### 1.3 Non-goals

- No `ImageLoader` / `ImageEncoder` classes (there are no options and no cache).
- No `std::istream` overload, no VFS entry point and no `createSource()`: see 2.4 and `D2`–`D4` .
- No mip-chain generation and no JPEG writing (writing is lossy and needs a quality parameter, and this API deliberately has no place for one).

### 1.4 One-paragraph conclusion

**A container is an enum and its capability is one table; a read goes through `Result`, a write through `IoError`; the four codec entry points are stateless free functions, and there is no second concept.**

## 2. Design Points

### 2.1 The format is a type, the capability comes from one table

- **What it is**: the `ImageFileFormat` enum; `ImageFormatInfo{ extensions, can_read, can_write }`; `formatInfo()` reads the table;
  `formatName()` / `formatFromPath()` / `canRead()` / `canWrite()` / `supportedExtensions()` are all queries on that table.
- **Why**: `G1`, `G6` — one home for one fact; the same names and shapes as `meshio`'s `formatInfo()` / `supportedExtensions()`.
- **Boundary**: the table is the read / write contract — a container with `can_write == false` cannot be written (JPEG); an extension outside the table answers `Unknown`.
- **Difference from meshio**: `meshio`'s table has no `can_read` / `can_write` ("the enum is the contract": every value in the table is readable and writable);
  an image is not like that (JPEG is read-only), so the table keeps two booleans. This is a **domain difference**, not a style difference.

### 2.2 Failure is a value

- **What it is**: `decodeImage()` / `loadImage()` return `vn::io::Result<intrusive_ptr<Image>>`;
  `encodeImage()` returns `Result<std::vector<std::byte>>`; `saveImage()` returns `IoError` (`Ok` means success).
- **Why**: `G4`, `G6` — one return value states the outcome and the reason; the same rule as `iobase`, `meshio` and `brepio`.
- **Classification** (`R1`): a layout that cannot carry the image → `Unsupported`; bytes that are not a readable image, or an empty file → `InvalidData`;
  a missing file → `NotFound`; a read that fails, a write that fails, no encoder output → `IoFailure`; a length beyond the decoder's addressable range → `CapacityExceeded`.
- **Cost**: `Result` carries the category only and drops stb's `stbi_failure_reason()` text —  the same cost as `meshio` giving up assimp's error text,
  and the caller knows the path and the format anyway (`D5`).

### 2.3 Stateless: four free functions, not a Loader / Exporter

- **What it is**: decoding is `decodeImage(bytes, layout)` / `loadImage(path, layout)` ; encoding is `encodeImage(image, format)` /
  `saveImage(path, image, format)`。
- **Why**: `G8` — decoding is a pure function and encoding has no policy; `meshio` needs an instance because it has options (a scaling policy) and
  a content-fingerprint cache. Copying the class shape would only make every caller build an object before calling one method.
- **Boundary**: the pixel layout the caller wants is a parameter (`PixelFormat`), because "the decoded RGB8 has no 24-bit texture format" is the caller's problem;
  the container is sniffed from the bytes and the extension is only a hint.

### 2.4 Codec only: no stream, no VFS, no createSource

- **Stream**: stb is a memory API, so "decode from a stream" necessarily reads it all into memory first, and `decodeImage(span)` is where that ends; an `istream` overload
  would only drain it for the caller, while giving the library one more path ("the stream broke halfway") to own — the reverse of `G7`.
- **VFS**: `Vfs::read()` / `Vfs::addFile()` combined with `decodeImage()` / `encodeImage()` is two lines; a VFS entry point would
  drag the `Vfs` type into the public surface for one combination (`G5`). The failure vocabulary (`IoError`) is a different matter: it is vocabulary, not implementation.
- **`createSource()`**: `encodeImage()` already produces the complete bytes and `Vfs::addFile(path, span)` takes them directly; deferred encoding saves one
  encoding buffer of a few dozen milliseconds, at the price of a source that must hold `intrusive_ptr<const Image>` and a lifetime contract (`G8`, `G10`).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | container vocabulary and capability table | the enum + `formatInfo()` + the four queries + `supportedExtensions()` | landed |
| F2 | in-memory decoding | `decodeImage(bytes, layout)` → `Result<Image>` | landed |
| F3 | file decoding | `loadImage(path, layout)` → `Result<Image>` | landed |
| F4 | in-memory encoding | `encodeImage(image, format)` → `Result<vector<byte>>` | landed |
| F5 | file encoding | `saveImage(path, image, format)` → `IoError` | landed |

### 3.1.1 F1 container vocabulary and the capability table

- **Behaviour**: `ImageFileFormat` (`Png` / `Jpeg` / `Bmp` / `Tga` / `Unknown`) ; the table answers the extensions, readability and writability.
- **Constraints / requirements / acceptance**: `C1`, `R2`, `T1`, `T2`.
- **Caveats**: JPEG has two extensions (`.jpg` / `.jpeg`); `Unknown` has an empty extension list and both capabilities false.

### 3.1.2 F2 / F3 decoding

- **Behaviour**: the container is sniffed from the bytes; the layout is the caller's choice; the result is always one mip; a BGRA layout gets R/B swapped after decoding.
- **Constraints / requirements / acceptance**: `C2`, `C3`, `R1`, `T3`, `T4`.
- **Caveats**: a one-channel file is Rec.601 **luma**, not the source's red channel; a two-channel layout (gray + alpha) is refused (`C4`, `P1`).

### 3.1.3 F4 / F5 encoding

- **Behaviour**: only the base mip is written; levels beyond the base are not supported; BGRA is encoded from a temporary R/B-swapped copy, so the caller's pixels are untouched.
- **Constraints / requirements / acceptance**: `C5`, `R1`, `T5`, `T6`.
- **Caveats**: BMP has only the 24-bit RGB and 32-bit RGBA paths, and a one-channel image is expanded into a gray three-channel one (`P2`).

## 4. Public Interface

```cpp
// vn::imageio —— the format vocabulary (src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp)
enum class ImageFileFormat : std::uint8_t { Unknown = 0, Png, Jpeg, Bmp, Tga };
struct ImageFormatInfo {
    std::span<const char* const> extensions;  // lower case, leading dot; Unknown → empty
    bool                         can_read;
    bool                         can_write;
};
const ImageFormatInfo& formatInfo(ImageFileFormat) noexcept;
const char*            formatName(ImageFileFormat) noexcept;          // "PNG"
ImageFileFormat        formatFromPath(const std::filesystem::path&);  // by extension, a hint only
bool                   canRead(ImageFileFormat) noexcept;
bool                   canWrite(ImageFileFormat) noexcept;
const std::set<vn::String>& supportedExtensions();                    // generated from the table
```

| Interface | Contract | Failure |
|---|---|---|
| `Result<intrusive_ptr<Image>> decodeImage(span<const byte>, PixelFormat)` | container sniffing; the layout is the caller's; one mip | `Unsupported` (no shape for the layout) / `InvalidData` / `CapacityExceeded` |
| `Result<intrusive_ptr<Image>> loadImage(path, PixelFormat)` | reads the whole file, then decodes | `NotFound` / `IoFailure` / whatever decoding fails with |
| `Result<vector<byte>> encodeImage(const Image&, ImageFileFormat)` | writes the base mip only | `Unsupported` / `InvalidData` / `IoFailure` |
| `IoError saveImage(path, const Image&, ImageFileFormat)` | the format comes from the parameter, never guessed from the extension | whatever encoding fails with / `IoFailure` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- `C1` The enum and the table are the only format vocabulary: `supportedExtensions()`, `formatFromPath()` and both read and write take it from there, and no second hand-written copy is allowed.
- `C2` The decoded container is decided by the bytes and the extension takes no part; a lying file name does not change the result.
- `C3` A decode result is always one mip: a container stores one image, and the chain is a different thing.
- `C4` A two-channel layout is refused: two channels in a file are gray + alpha, while `Rg8Unorm` is red + green.
- `C5` Encoding writes the base mip only; JPEG is not writable (`can_write == false`).

### 5.2 Requirements

- `R1` A failure must be classified (the five kinds in 2.2), not merely answered with an empty value or an exception.
- `R2` The public header uses base-module types only (`Imaging`, `IOBase`, `Core`); stb does not leak out (PRIVATE link).
- `R3` The public entry points are stateless: two calls with the same arguments produce the same bytes (`T6`).

### 5.3 Invariants

- `N1` `Result`'s `*` / `->` are undefined behaviour on failure; check `ok()` first.
- `N2` Every decode allocates a new `Image` owned by the caller; there is no cache and no shared instance.
- `N3` An 8-bit colour layout = 1 / 3 / 4 channels; floating-point and depth layouts are outside what can be encoded.

## 6. Caveats and Traps

| # | How it fails | How to detect it | How to prevent it |
|---|---|---|---|
| P1 | a one-channel image is taken for the source's red channel | a gray image and red-channel sampling differ | the comment on `C4`; `ASingleChannelIsLumaAndNotTheSourcesRed` |
| P2 | assuming BMP can store two channels | the second channel reads back as `0xFF` | the `canWrite` table entry plus BMP's RGB/RGBA-only paths |
| P3 | measuring luminance semantics on JPEG | the luminance is a step or two off | luminance cases run on lossless containers only (PNG) |
| P4 | decoding a buffer larger than `INT_MAX` | the length is truncated to `int` | `CapacityExceeded` is returned before the decoder is entered |
| P5 | the file dialog writes its own extensions | the dialog does not accept a new format | use `supportedExtensions()` (`C1`) |

## 7. Rejected Options

| # | Option | Why not | ADR |
|---|---|---|---|
| D1 | copy the `Loader` / `Exporter` class shape | `G8`: no options and no cache, so a class adds lifetime only | — |
| D2 | `load(std::istream&)` / `save(std::ostream&)` | `G7`: stb is a memory API, the overload only drains for the caller | — |
| D3 | `loadImage(Vfs&, path)` / a `DataSource` entry point | `G5`/`G7`: `Vfs::read()` plus `decodeImage()` is two lines, and adding it pulls `Vfs` into the public surface | — |
| D4 | `createSource()` (deferred encoding into a package) | `G8`/`G10`: it saves one encoding buffer at the price of a source holding an `Image` and a lifetime contract | — |
| D5 | keep exceptions as the failure channel | `G4`/`G6`: one layer with two failure conventions, so a caller cannot act on the category | — |
| D6 | keep the four hand-written `switch`es | `G1`: four copies of one fact, and adding a format means four edits | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | the table is self-consistent value by value: every extension of `formatInfo()` is recognised back by `formatFromPath()` and appears in `supportedExtensions()` | `ImageFileFormatTest.TheTableStatesEachFormatsExtensionsAndCapabilities` |
| T2 | the extension list is generated from the table (5 entries: png / jpg / jpeg / bmp / tga) | `ImageFileFormatTest.TheExtensionListIsDerivedFromTheTable` |
| T3 | every writable container × every 8-bit layout round-trips byte for byte | `ImageEncodeTest.EveryWritableContainerRoundTripsEveryLayoutExactly` |
| T4 | failure classification: a missing file `NotFound`, an empty file / garbage bytes `InvalidData`, an unwritable container `Unsupported`, an unwritable path `IoFailure` | `ImageDecodeTest.ReportsAMissingFileAsNotFound` and others |
| T5 | a two-channel or floating-point layout is refused (`Unsupported`), never silently converted | `ImageDecodeTest.RefusesTwoChannelLayouts...`, `RefusesALayoutAContainerCannotHold` |
| T6 | the same input encodes to the same bytes twice; only the base mip is written | `ImageEncodeTest.OnlyTheBaseLevelIsWritten` |

## 9. Status and Evolution

### 9.1 Current Status

The public surface is implemented as this page states; the 28 cases in `tests/test_imageio` all pass. The only consumer is `demo_plugin`'s cube map (six JPEG faces).

### 9.2 Known Gaps

- **no mip generation**: decoding produces the base level only, and the chain is generated on the upload side (see `imaging-design.md` 9.2).
- **JPEG writing**: it needs a quality parameter, which is "a new parameter" rather than "opening a table entry"; add it as a `WriteOptions` form when a consumer appears.
- **the VFS / package entry point**: the caller composes it per `D3`; reassess when "read a large image inside a package without loading it" or "export a package in bulk" appears (nothing is pre-built while `../../.ai/backlog/` has no matching entry).

### 9.3 Compatibility and Migration

A breaking change on the public surface: the four codec entry points went from "throw / return a bare value" to `Result` / `IoError`, and `formatInfo()` and
`supportedExtensions()` were added. Every call site in the repository (`demo_plugin` and `tests/test_imageio`) is migrated.

## 10. References

- Code: `src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp`, `src/loaders/imageio/src/ImageFormat.cpp`,
  `src/loaders/imageio/src/ImageCodec.cpp`。
- Vocabulary: `src/base/iobase/sdk/vine/io/IoError.hpp` (`Result` / `IoError`).
- Same-layer comparison: `meshio-brepio-design.md` (the same failure semantics and capability table; the reason for the shape difference is in 2.3 / 2.4).
- Data shape: `imaging-design.md`; VFS: `iobase-vfs-design.md`.
