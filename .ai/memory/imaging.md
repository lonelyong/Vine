# imaging notes

> Module positioning, the rationale and the rejected alternatives: `.ai/design/imaging-design.md`.
> This file records **conclusions + criteria** only, so the next session can pick up directly.

## Facts

- Directory `src/base/imaging/`, short name **`Imaging`** → alias `vn::Imaging`, macros
  `VN_IMAGING_API` / `VN_IMAGING_LIB`, namespace `vn::imaging`, headers `<vine/imaging/...>`.
- Depends on **`vn::Core` and `vn::Global` only** (a leaf, the same level as `geometry`).
- Registration points: `add_subdirectory(imaging)` in `src/base/CMakeLists.txt` (after `geometry`) and
  `add_subdirectory(test_imaging)` in `tests/CMakeLists.txt`.
- **A new file needs `cmake -S . -B build`**: `vn_add_library` uses `file(GLOB_RECURSE ...)` **without**
  `CONFIGURE_DEPENDS`.

## The division that must hold

> **CPU pixels / image data → `imaging`; GPU resources (format / mip / sampler / usage) → `graphics`.**
> Ask yourself: *"does this make sense headless, with no renderer and no Qt?"* If yes → `imaging`.

## 2026-09-12, first batch: the module, `PixelFormat` and `Image`

**Background**: the SDK had **no object** for pixel data - a material texture was just a path string
(`Material::textureFile() -> String`), `loaders/` decoded no pixels at all, and the whole library had no
readback path. `ImageRef.hpp`'s own comment admitted the hole ("a material texture is a file PATH").

**Decisions taken**:
- `Image` (CPU pixels) does **not** go into `graphics` (it would make `meshio` **depend on the
  renderer**; `graphics → Geometry` shows vertex data sits **below** graphics, and pixel data must not
  sit above it); it does **not** go into `window` (a forced name, and renaming means touching the short
  name, the directory, the macro and the alias in four places - miss one and `dllimport` blows up on
  Windows); instead a **new leaf module** `imaging`.
- `Texture` stays in `graphics` and **covers 2D + Cube only** for now; no 1D/3D/array.
- `Image` also **cannot** be a `QImage` (graphics is Qt-free; `QImage` lives in `Qt6::Gui`); the Qt bridge
  stays in `appfw`.

**Landed**:
- `PixelFormat`: 18 real formats + `Unknown`; queries are **free functions** (`channelCount` /
  `bytesPerPixel` / `isDepthFormat` / `isSrgbFormat` / `formatName`), so a new format teaches one place;
  the implementation is a `switch` **covering every enumerator** (no `default:`), so `-Wswitch` forces a
  new format to be handled; an out-of-range cast reads as `Unknown`.
- `Image`: `Object` + `RefCounted`; one allocation holds the whole mip chain (`mip_offsets_` has N+1
  entries, the last a sentinel); mip level N is `max(1, w>>N) x max(1, h>>N)`; pixels are
  **zero-initialised**; accessors **clamp** an out-of-range mip (`noexcept`: a size query is not where a
  bug is discovered); the constructor **throws `std::invalid_argument`** for a non-positive size, a
  zero-sized pixel format or an out-of-range mip count.
- **A cube map is six `Image`s**; `Image` carries no face axis (adding one would make every ordinary 2D
  caller carry the `faces = 1` special case).

**Criteria**: `test_imaging` **19 tests / 3 suites** green (newly created); a full `ninja` with **zero
errors / zero warnings**; `test_graphics` and `test_vsg` **no regression** (a pure new leaf module, no
existing source touched).

**Not done (deliberately postponed)**: **the backend still does not consume `Texture`** (no face / mip
upload, no sampler, no descriptor set); no image decoding or readback path; `RenderTarget`'s own
`ColorFormat`/`DepthFormat` **duplicate** `imaging::PixelFormat` and should merge into it (a public API
change, so its own batch).

## 2026-09-12, second batch: `graphics::Texture` (2D + Cube) and the `Material` migration

**Prerequisite fact**: `Material::textureFile()`'s path string had **zero calls** in `src/` and one test
reference - a dead API, exactly the hole `ImageRef.hpp`'s comment described (*"a material texture is a
file PATH"*).

**Landed**:
- A new `graphics::Texture` (`Object` + `RefCounted`, the same family as `Material`/`ImageRef`):
  `Shape{D2,Cube}`, `layerCount()` (D2=1 / Cube=6), `width/height/format/mipCount`,
  `setLayer(layer, intrusive_ptr<const imaging::Image>)` / `layer(index)` / `hasLayer(layer)` /
  `isComplete()`.
- **The same pattern as `RenderTarget`**: the SDK type is a **logical description**, holds no GPU object,
  and a backend materialises it.
- A source image must agree with the description on **all of format / size / mipCount** (a mismatch is
  refused **at the call site**, not left for the backend to discover during upload).
- Reading an out-of-range face is a **query** (answer "there is no such face", return null); writing one
  throws `std::out_of_range`.
- The mip cap **reuses** `imaging::Image::mipCapacity` instead of restating the rule.
- `graphics` now **depends PUBLICly on `vn::Imaging`**.
- `Material`: `textureFile()`/`setTextureFile()` deleted (the dead API) and replaced by
  `texture()`/`setTexture(intrusive_ptr<Texture>)`.

**Criteria**: `test_graphics` **191 → 200** (+9 = 8 `TextureTest` cases + 1 `MaterialTest`);
a full `ninja` with zero errors / zero warnings; `vsg_selftest_evidence.sh` → PASS (**45 lines
byte-identical**, the backend untouched); `gfx_lavapipe_check.sh` → PASS (0 VUID);
`check_diagnostic_formats.py` → 0 suspicious.

> Reconciliation note (2026-09-25): two items in this file's "Not done (deliberately postponed)" above are
> **out of date** - **"the backend still does not consume `Texture`"** (the rewritten backend does: face
> 0 / six Cube layers, the mip chain, an anisotropic sampler, the sampling descriptor set) and **"no
> readback path"** (the backend layer has off-screen `readColorBuffer`/`readDepthBuffer`, used by
> `test_vsg`'s pixel phase); what is still true is **mip generation** (decoding produces one level),
> **an `Image`-level wrapper and window readback**, and **the `PixelFormat` merge**. For the current
> state see the 2026-09-25 reconciliation in `.ai/design/imaging-design.md` §7.
