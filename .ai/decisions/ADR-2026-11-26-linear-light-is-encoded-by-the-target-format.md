# ADR-2026-11-26: linear light becomes an encoded pixel at the swap-chain write, by the target's format

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-rendering.md` (§2.14, F23), `RenderTarget.hpp` (`ColorFormat`), `TargetObjects.cpp`
  (format table), `WindowTarget.cpp` (swap-chain candidate list), `BuiltinShaders.hpp`
  (`fullscreenCopyProgram`), `docs/guides/graphics.md` (§6 display)

## Background

Every shading stage produces **linear** light: the material values are linear, the BRDF is linear, and an emissive
term may exceed 1. Nothing in the module stated where that value becomes an encoded pixel, so three facts were true at
once and each was written down somewhere different:

- an 8-bit OFFSCREEN attachment is a `*_UNORM` format, i.e. it stores the linear value quantised to 256 steps;
- the SWAP CHAIN uses an `*_SRGB` format, whose stores the hardware encodes;
- the full-screen copy is one `texture()` with no transform, and no stage in the tree applies a tone curve or an
  exposure.

The consequence is the one a reader cannot see from any single header: a lit value that passes an offscreen `RGBA8`
loses most of its precision where the eye is most sensitive (linear 0.5 is about 188 in sRGB), clips above 1 before the
window, and a readback of that target differs from what the window shows by the transfer function - so a
pixel-comparison test against a readback proves something other than what the user sees.

## Evidence

| Fact | Where |
|---|---|
| Shading writes linear values, unbounded above 1 (emissive) | `vine_material.glsl`, `builtin_forward.frag`, `VineMaterialBlock`'s note |
| Offscreen 8-bit formats map to `*_UNORM` | `TargetObjects.cpp` format table |
| The swap chain selects an sRGB format | `WindowTarget.cpp` candidate list |
| The copy program is a plain `texture()` | `builtin_fullscreen_copy.frag` |
| No tone mapper, no exposure | `BuiltinShaders.hpp`: a program that wants one "is a different program" |
| A colour readback returns the stored bytes | `ReadbackStaging.cpp` |

## Decision

1. **The transfer function is a property of the TARGET FORMAT** - the concrete device format a backend picks for the
   `ColorFormat` a target states (`RenderTarget.hpp` already calls `RGBA8` a projection over the `*_UNORM` and `*_SRGB`
   spelling) - not of the shading, the material or the pass. Two targets whose device formats differ hold different
   kinds of value.
2. **The encode happens at the swap-chain write, and only there.** A presenting target carries an sRGB format, so the
   hardware applies the transfer when the linear fragment is stored: that store is the last step of the display
   pipeline. No shader and no pass in the engine encodes.
3. **An offscreen 8-bit target is a DATA format**: a coverage mask, a weight, an occlusion factor, the exact bytes a
   test compares. A lit picture that a human will look at, and that must survive an offscreen hop, is stored in
   `RGBA16F` (or 32F) until the swap-chain write - which is what the Deferred preset already does with its G-buffer.
4. **Tone mapping and exposure are not part of the engine.** The transfer function is not a tone curve: a scene whose
   lit values exceed 1 clips at the swap-chain write. A host that wants a look authors a full-screen program
   (`fullscreenCopyProgram`'s note says so), and the pass names it.
5. **A readback answers the stored bytes** - what the target holds, not what the window shows. The difference between
   the two is the format's transfer, which is why a pixel test on an 8-bit offscreen target is a test of the data.

## Rejected options

| Option | Why not |
|---|---|
| Encode in the full-screen copy, into a `*_UNORM` swap chain | The encode would then be every copy program's duty, and a host's copy that forgot it would present a dark picture; the format the window already has does it in one place, in hardware. |
| Add sRGB spellings to `ColorFormat` now | No pass asks for one (`G8`); the presenting target already states the fact through its format, and the enum would double the vocabulary for a decision the target carries. Reopen when a pass must present an encoded picture through an offscreen target. |
| Make the 8-bit offscreen format sRGB so "it looks right" | Storing a LIGHTING intermediate as sRGB makes every arithmetic read of it (a blend, a filter, a downsample) operate on encoded values - the error this decision exists to avoid. |
| Add a tone-mapping stage to the engine | A tone curve is an authored look, not a renderer fact (`G8`); the engine has no exposure input and no consumer for one. |
| Document nothing and let the formats speak | They do not: `RGBA8` names a width, not a transfer, and the module shipped for months with three readers of the value disagreeing. |

## Consequences

- `RenderTarget.hpp`'s `ColorFormat` note states the transfer rule, and the design page's display section states the
  order (linear shading -> optional float intermediate -> swap-chain encode).
- A host that reads a target back and compares it with the window must apply the transfer itself; a host that wants the
  window's picture exactly reads pixels from the presenting target, not from an offscreen one.
- An 8-bit offscreen lit picture still bands in its dark half. The fix is the format (16F), not a shader: the engine
  does not silently promote a target's format.
