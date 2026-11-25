# the imaging module: CPU pixel data

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the CPU pixel formats, `Image` and the mip chain; no textures / samplers / devices.
> **Sources**: `src/base/imaging/sdk/vine/imaging/**`, `tests/test_imaging`.
> **Backlog**: `../backlog/`

## 1. Overview

### 1.1 Problem and background

The SDK has no object for pixel data: a material texture is only a path string, nothing is decoded, nothing is read back, and `ImageRef` is only a binding handle.
`imaging` answers that gap on the **data side**: what an image looks like in memory.

### 1.2 Goals

- `PixelFormat`: the per-pixel byte layout plus the query functions.
- `Image`: one 2D image plus an optional mip chain, one allocation, one copy, one release.
- Usable at the same time by a loader (decodes a file), the rendering layer (read-back result) and a renderer-free tool.

### 1.3 Non-goals

- It knows nothing about textures, samplers, devices or renderers.
- No `QImage` bridge (the Qt-side conversion stays in appfw).
- No 1D / 3D / array; today only 2D and Cube (Cube = six `Image`s).
- No mip generator (decoding produces level 0 only; generation is a separate decision).

### 1.4 One-paragraph conclusion

`imaging` is the base layer's **CPU pixel leaf module**: `PixelFormat` is the single mapping point of the format facts, `Image` is a 2D image plus its mip chain;
GPU resources (textures / samplers / devices) stay in `graphics`, and the two sides meet only through `IntrusivePtr<Image>`.

## 2. Design Points

### 2.1 Architectural placement

```
                ┌────────────────────────────┐
                │ imaging (leaf, CPU pixels) │
                │  PixelFormat / Image       │
                │  depends on Core + Global   │
                └─────────────┬──────────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
┌───────▼────────┐  ┌────────▼────────┐  ┌────────▼────────┐
│ graphics       │  │ meshio          │  │ offscreen tools │
│ builds Texture │  │ decodes file    │  │ urdf2vine, etc. │
│ readback→Image │  │   → Image       │  │ (no renderer)   │
└────────────────┘  └─────────────────┘  └─────────────────┘
```

**The key: dependencies point only upward.** `imaging` sits at the same level as `geometry`; anyone may depend on it, it depends on nobody.

### 2.2 The division of labour with graphics

> **CPU pixels / image data → `imaging`**
> **GPU resources (format / mip / sampler / usage) → `graphics`**

The test question: **“does this thing mean anything in a headless environment with no renderer and no Qt?”** Yes → `imaging`; no → `graphics`.

### 2.3 Why `Image` does not live in `graphics`

Putting `Image` into `graphics` is “cheapest today, most painful in three months”:

1. **the `meshio → graphics` layer inversion**: assimp's `aiTexture::pcData` naturally produces decoded pixels.
2. **renderer-free tools cannot use it**: `urdf2vine` deliberately does not link graphics/Qt.
3. **the layering logic is inverted**: vertex data `Geometry` sits below `graphics`, and pixel data is the same kind of thing.

In addition: `Image` also **cannot** be `QImage` — `graphics` is Qt-free; the Qt-side conversion bridge stays in appfw.

### 2.4 Why `Texture` stays in `graphics`

A texture is a GPU resource: its only consumers are `Material` (sampling), `RenderTarget` (attachment), `ImageRef` (identity) and the backend (creation).
A module below `graphics` cannot meaningfully talk about a GPU texture.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | `PixelFormat` | 18 real formats + `Unknown`; the queries go through free functions | landed |
| F2 | `Image` | a 2D image + mip chain, one allocation | landed |
| F3 | mip layout | a compact 2D grid per level, an offset table + a sentinel | landed |
| F4 | format queries | `channelCount` / `bytesPerPixel` / `isDepthFormat` / `isSrgbFormat` / `formatName` | landed |

### 3.1.1 F1 `PixelFormat`

- **Behaviour**: `enum class PixelFormat : std::uint8_t`, 18 real formats + `Unknown`; a cast from outside the range is treated as `Unknown`.
- **Constraints** (C1): the queries go through free functions, not branches over the enum; the `switch` covers every enumerator and uses no `default:`.
- **Requirements** (R1): `Unknown` is the only value that is “not a format”, a default-constructed one is detectable as wrong and does not silently degrade into the first real format.
- **Acceptance** (T1): the PixelFormat cases in `test_imaging`.
- **Caveats** (N1): `D24UnormS8Uint` reports `channelCount` 2 (depth and stencil both count as channels).

### 3.1.2 F2 `Image`

- **Behaviour**: `Object` + `RefCounted<Image>`; `Image(width, height, format, mip_count = 1)`;
  `mipCapacity(width, height)`.
- **Constraints** (C2): a non-positive size, a format whose pixel size is 0 and a mip count outside `[1, mipCapacity]` all raise
  `std::invalid_argument`; the pixels are zero-initialised.
- **Requirements** (R2): one allocation, one copy, one release; the mip accessor clamps an out-of-range level to the last one instead of failing.
- **Acceptance** (T2): the Image / layout cases in `test_imaging`.
- **Caveats** (N2): an out-of-range request is rejected in the constructor; a size query is not where a caller's bug is found.

### 3.1.3 F3 mip layout

- **Behaviour**: the base level is `width x height`; mip N is `max(1, width >> N) x max(1, height >> N)`;
  `mip_offsets_` holds N+1 offsets (the last one is the sentinel = the total size).
- **Constraints** (C3): a compact 2D grid per level, no padding between rows; the whole chain is one allocation.
- **Requirements** (R3): the size of a level = the difference of two offsets; no separate size table.
- **Acceptance** (T3): the mip cases in `test_imaging`.
- **Caveats** (N3): the upload side already works per level (`makeMipmapLayout`); what is missing is only generating the levels beyond level 0.

### 3.1.4 F4 format queries

- **Behaviour**: `channelCount` / `bytesPerPixel` / `isDepthFormat` / `isSrgbFormat` / `formatName`.
- **Constraints** (C4): `formatName` returns a static literal and never null; `bytesPerPixel` of a packed format reports the whole texel size.
- **Requirements** (R4): adding a format = teaching one place (`PixelFormat.cpp`), not every call site.
- **Acceptance** (T4): the format query cases in `test_imaging`.
- **Caveats** (N4): `-Wswitch` forces a new format to be handled, a guard a lookup table does not give.

## 4. Public Interface

### 4.1 `PixelFormat.hpp`

| Interface | Contract | Notes |
| --- | --- | --- |
| `enum class PixelFormat` | 18 real formats + `Unknown` | a cast from outside the range is treated as `Unknown` |
| `channelCount(PixelFormat)` | channel count | depth and stencil count as 2 |
| `bytesPerPixel(PixelFormat)` | bytes per pixel | a packed format reports the texel size |
| `isDepthFormat` / `isSrgbFormat` | format classification | `noexcept` |
| `formatName` | the static name | never null |

### 4.2 `Image.hpp`

| Interface | Contract | Notes |
| --- | --- | --- |
| `Image(width, height, format, mip_count)` | constructs and zero-initialises | an illegal argument throws `invalid_argument` |
| `mipCapacity(width, height)` | the maximum mip count | static |
| `width()` / `height()` / `format()` / `mipCount()` | queries | — |
| `mipData(level)` / `mipByteSize(level)` | access | clamps an out-of-range level |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | the queries go through free functions; the `switch` covers every enumerator and uses no `default:`. |
| C2 | an illegal size / format / mip count is rejected in the constructor; the pixels are zero-initialised. |
| C3 | a compact 2D grid per level, no padding between rows; the whole chain is one allocation. |
| C4 | `formatName` returns a static literal; `bytesPerPixel` reports the texel size. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | a default-constructed `Unknown` is detectable and does not silently degrade. |
| R2 | the mip accessors clamp an out-of-range level instead of failing; a size query does not find a bug. |
| R3 | the layering points only upward: `imaging` does not depend on `graphics`/Qt. |
| R4 | adding a format changes `PixelFormat.cpp` in one place only. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | `Image` is 2D; Cube = six `Image`s and adds no face axis. |
| N2 | the last value of `mip_offsets_` is the total-size sentinel. |
| N3 | the pixel data is all zero at construction, so an unfilled part leaks no memory. |

## 6. Caveats and Traps

### 6.1 `Unknown` is not the first real format

- How it fails: the default-constructed value is taken for `R8G8B8A8` and quietly paints black or the wrong colour.
- How to detect it: `formatName(Unknown)` returns `"Unknown"`; the constructor rejects a format whose pixel size is 0.
- How to prevent it: the `switch` covers every enumerator and uses no `default:`.

### 6.2 The mip offset sentinel

- How it fails: one offset too few is stored and `mipByteSize()` of the last level reads out of range.
- How to detect it: `mip_offsets_.size() == mip_count + 1`.
- How to prevent it: the size is always the difference of two offsets, never a separate size table.

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | put `Image` into `graphics` | it inverts the `meshio → graphics` layering; renderer-free tools cannot use it. | — |
| D2 | `Image` = `QImage` | `graphics` is Qt-free; the Qt bridge stays in appfw. | — |
| D3 | rename `window` to `display` and collect `Image` there | the name is forced; the rename drags the short name / directory / macro / alias along. | — |
| D4 | 1D / 3D / array | over-engineering; today only 2D and Cube exist. | — |
| D5 | `Image` with a face axis | every 2D caller carries a `faces=1` special case; grouping a Cube is the renderer's job. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | PixelFormat queries / Unknown | `test_imaging` |
| T2 | Image construction / zero-init / illegal arguments | `test_imaging` |
| T3 | mip layout / offsets / clamping | `test_imaging` |
| T4 | the Texture / Material migration has no regression | `test_graphics` (642), `test_vk` (186) |

## 9. Status and Evolution

### 9.1 Current Status

`PixelFormat` / `Image` are in place; `graphics::Texture` (2D + Cube) and `Material::textureFile()` → `Texture*`
are migrated; `ImageIO` decoding is implemented (public surface in `imageio-design.md`).

### 9.2 Known Gaps and Backlog

- **no mip generation**: decoding produces one mip; the upload side already works per level, what is missing is the step that expands level 0 into a chain.
- **no `imaging::Image`-level read-back wrapper**: the backends already have `readColorImage()/readDepthImage()`, and window read-back goes through an X11 script.
- `RenderTarget`'s `ColorFormat`/`DepthFormat` duplicate `PixelFormat` and should be folded into the latter eventually (a public API change, its own batch).
- `Texture3D`/`2DArray` cannot be expressed (`Image` is strictly two-dimensional).

### 9.3 Compatibility and Migration

`Material::textureFile()` has been migrated to `Texture*`; the old path string has no call site left in `src/`. New code hands CPU pixels over through
`IntrusivePtr<Image>`; the GPU side uses only `Texture`.

## 10. References

### 10.1 Code

- `src/base/imaging/sdk/vine/imaging/PixelFormat.hpp`
- `src/base/imaging/sdk/vine/imaging/Image.hpp`
- `src/base/imaging/src/PixelFormat.cpp`
- `tests/test_imaging`

### 10.2 Documents and ADRs

- `.ai/memory/imaging.md`
- `.ai/design/graphics-rendering.md`
- `imageio-design.md`
- `../backlog/`

### 10.3 External Sources

- `loaders/imageio` (`vn::ImageIO`, stb support).
- the GPU-side contract of `graphics::Texture` / `Material`.
