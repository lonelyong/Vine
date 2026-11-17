# imageio notes

> The placement rationale is in `.ai/design/imaging-design.md` (the reasoning of §3 "the iron rule dividing
> work from graphics" applies here too).
> The public surface (capability table, failure classification, rejected options) is in
> `.ai/design/imageio-design.md`.
> This file records **conclusions + criteria** only.

## Quick facts

- Directory `src/loaders/imageio/`, short name **`ImageIO`** → alias `vn::ImageIO`, macros `VN_IMAGEIO_API` /
  `VN_IMAGEIO_LIB`, namespace `vn::imageio`, header `<vine/imageio/ImageCodec.hpp>`.
- Depends on **PUBLIC `vn::Imaging` + `vn::IOBase`** (`Result` / `IoError` are in public signatures) +
  **PRIVATE `tp::stb`** (statically baked into the DLL, consumers never see stb).
- Registration points: `src/loaders/CMakeLists.txt`, `tests/CMakeLists.txt`, `third_party/CMakeLists.txt`
  (new `third_party/stb/CMakeLists.txt`, INTERFACE target `stb` / alias **`tp::stb`**).
- **A new file requires `cmake -S . -B build` afterwards** (`vn_add_library`'s glob has no `CONFIGURE_DEPENDS`).

## Why loaders and not imaging

- **The repository has answered the same question once already**: `base/geometry` holds the data
  (`Mesh`/`Shape`), `loaders/meshio` holds the code that reads it with assimp. Images are isomorphic:
  `base/imaging` is the data and `loaders/imageio` is **one** channel that fills it. The name aligns with
  `meshio`.
- **"The base layer must not touch third-party libraries" is a false proposition** (`base/crypto` links
  wolfssl `PRIVATE`), but the real dividing line is sharper: **crypto depends on wolfssl because "the hash is
  that library"; `Image` is not a decoder** — the same data must serve four origins: probe / readback /
  programmatic generation / file, and stb is only one of those channels.
- The cost is concrete: `imaging` is now a leaf depending on Core+Global only and compiles in seconds; with
  stb inside, **every** target that links `vn::Imaging` (a future `meshio`, the headless `urdf2vine`) would
  carry it.

## How stb was brought in (2026-09-12)

- **Vendored** into `third_party/stb/`: `stb_image.h` (v2.30, sha256 `594c2fe3…`), `stb_image_write.h` (v1.16,
  sha256 `cbd5f0ad…`). No FetchContent: upstream has no CMakeLists and no releases, FetchContent would pull the
  whole repository for two files and the target would still have to be written by hand.
- **The compile configuration lives in one place, `src/loaders/imageio/src/StbConfig.hpp`**, included by two
  TUs (`StbImageImpl.cpp` with `*_IMPLEMENTATION`, `ImageCodec.cpp` without). Both must agree, so there is one
  copy.
- `STBI_ONLY_{PNG,JPEG,BMP,TGA}` trims the codecs + `STBI_NO_STDIO`/`STBI_WRITE_NO_STDIO`: coding goes through
  the **memory API** and file I/O is ours, with `std::filesystem` + fstream (avoids Windows narrow-character
  paths and makes coding testable purely in memory).
- **stb compiles with zero warnings under the project's strict flags** — no `-w` suppression is needed, so
  none was added.

## Channel semantics (**a trap to know before using stb**)

stb's channel count does **not** mean R/G/B/A in order:

| Channel | stb's meaning |
| --- | --- |
| 1 | **Rec.601 luma** (`(r*77+g*150+b*29)>>8`), not the red channel |
| 2 | **grey + alpha** (`(3,2){dest[0]=compute_y(...); dest[1]=255;}` in `convert_format`) |
| 3 | RGB |
| 4 | RGBA |

→ So `imageio` **refuses 2 channels**: 2 channels in a file is "grey+alpha" while `Rg8Unorm` is "red+green";
answering it would be a lie. Better to give nothing.

**Another trap**: the JPEG decoder computes the 1-channel luma at `stb_image.h:3991/4001` from the **pre-clamp**
intermediate RGB, while `stbi__convert_format` uses the clamped bytes — so "recompute luma from the RGB8
result" and "the 1-channel decode" can differ by a step or two. **Test luma semantics on a lossless container
(PNG)**, not on JPEG.

## API (`sdk/vine/imageio/ImageCodec.hpp`)

```cpp
enum class ImageFileFormat { Unknown, Png, Jpeg, Bmp, Tga };
struct ImageFormatInfo { std::span<const char* const> extensions; bool can_read; bool can_write; };
const ImageFormatInfo& formatInfo(ImageFileFormat) noexcept;    // the table: extensions + capability, every query reads it
const char*     formatName(ImageFileFormat) noexcept;
ImageFileFormat formatFromPath(const std::filesystem::path&);   // by extension, a hint only
bool canRead(ImageFileFormat) noexcept;    // all four
bool canWrite(ImageFileFormat) noexcept;   // PNG/BMP/TGA; not JPEG (lossy and needs a quality parameter)
const std::set<vn::String>& supportedExtensions();   // generated from the table (png/jpg/jpeg/bmp/tga)

vn::io::Result<intrusive_ptr<imaging::Image>> decodeImage(std::span<const std::byte>, imaging::PixelFormat);
vn::io::Result<intrusive_ptr<imaging::Image>> loadImage(const std::filesystem::path&, imaging::PixelFormat);
vn::io::Result<std::vector<std::byte>>        encodeImage(const imaging::Image&, ImageFileFormat);
vn::io::IoError                               saveImage(const std::filesystem::path&, const imaging::Image&, ImageFileFormat);
```

- **The container is sniffed from the bytes, not the extension** (decode never looks at the extension — a
  lying filename still decodes correctly).
- **The caller chooses the `PixelFormat`** (1/3/4 channels + sRGB/BGRA variants), which solves "decoding is
  naturally RGB8 but the GPU has no 24-bit texture format". BGRA is implemented by swapping R/B after
  decoding (stb only produces RGBA).
- A decode result is **always 1 mip** (the container stores one image; a chain is another matter).
- Encode **writes the base mip only** (PNG/BMP/TGA cannot hold a chain).
- Failure **is a value** (2026-10-27, the same scheme as meshio/brepio): no matching layout form / container
  not writable → `Unsupported`; empty buffer, empty file, undecodable → `InvalidData`; file does not exist →
  `NotFound`; read/write failure, encoder produced nothing → `IoFailure`; buffer over `INT_MAX` →
  `CapacityExceeded`. No more exceptions.

## BMP capability boundary (**a trap**)

stb's BMP writer has only two paths (`stb_image_write.h:494`): `comp != 4` → 24-bit RGB, `comp == 4` → 32-bit
V4. `comp == 1/2` goes through `expand_mono`, which copies grey into RGB.
So **BMP can store 1 / 3 / 4 channels and cannot store 2** (with 2 channels the alpha is silently dropped).
Measured: a `BMP/Rg8Unorm` round-trip fails (the second channel always comes back `0xFF`). PNG and TGA support
1/2/3/4.
Note that `stbiw__write_pixels` strides correctly by `comp` (`data + (j*x+i)*comp`), so there is **no
out-of-bounds read**.

## First batch, 2026-09-12: `loaders/imageio` created

**Criteria**: `test_imageio` **26 tests / 3 suites** all pass (newly created); a full `ninja` has **zero
errors / zero warnings**; `test_imaging` / `test_graphics` (200) / `test_vsg` / `test_meshio` / `test_brepio`
show no regression; `vsg_selftest_evidence.sh` → PASS (45 lines byte-identical); `check_diagnostic_formats.py` →
0 suspicious.

**The tests use real material**: all 12 2048² JPEGs in `test_data/images/` were decoded.
`tests/test_imageio/CMakeLists.txt` uses `target_compile_definitions(... VN_TEST_DATA_DIR=...)` to point at
**the copy next to the binary** (`<build>/bin/test_data`) — **this was the repository's first test that reads
material from disk, and it settled the path convention by the way**. The convention changed later (2026-09-13):
the material directory carries its own staging rule (`test_data/CMakeLists.txt`, target `stage_test_data`:
`<build>/bin/test_data` at build time, `<prefix>/test_data` at install time, beside `bin/`/`lib/`), and **the
source-tree path is no longer compiled in** — it runs on Windows without installing and no longer depends on
an absolute path.
