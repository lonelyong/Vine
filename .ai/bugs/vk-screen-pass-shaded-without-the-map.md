# The vk full-screen set binds only the shadow stand-in, so deferred lighting has no shadows

- **Status**: Fixed (2026-11-14, `gfx_backend_vk`; regression case `VkShadowTest.AScreenPassShadesWithTheMapItResolved`)
- **Found**: 2026-11-14
- **Module**: `src/plugins/gfx_backend_vk`
- **Files**: `src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp` (`Impl::makeScreenSet` / `recordScreen`),
  `src/plugins/gfx_backend_vk/src/internal/Device.cpp` (handing the pass's resolved `ShadowSource` to screen recording),
  reference behaviour: `src/plugins/gfx_backend_vsg/src/internal/ContentPass.cpp` (the screen path binds the map the pass resolved)

## Symptom and impact

`Vine --open demo` (deferred by default) had **no sun shadows** on vk, while the same scene had them on vsg. Shadows on the
content path (forward, G-buffer stage) were fine - only the **full-screen** deferred lighting program could not get the
shadow map. Before the fix the same-size render area of the two backends differed by 1.88% of pixels; after it the
difference is **0 pixels** (byte-identical).

## Root cause

The engine's deferred lighting program has a **shadowed variant** (`BuiltinShaders::deferredLightProgram(shadowed)`, which
inserts `VN_SHADOW_BINDINGS` into the text: the map at binding 7 plus `VineShadowBlock` at binding 8). Vulkan's full-screen
set gained those two binding slots earlier on 2026-11-14 (the shadowed pipeline could not be created without them,
VUID-07988), but `makeScreenSet` **always** wrote a white stand-in sampler/view and a zero `VineShadowBlock`:

- `recordScreen` did not receive the map the pass had resolved (`Device::executeFrame` already called `resolveShadow` for
  **every** pass, it just never passed the result into screen recording);
- so the shadowed lighting program on vk always read "white image, zero block" => `shadow term = 1` => no shadows, with
  **no diagnostic at all** (bindings legal, pipeline legal, pixels produced).

The same path on vsg binds the real depth image the pass resolved, and **refuses** the call when the program declares a map
but the pass cannot resolve a readable one.

## Fix

- `ContentDraw::recordScreen` takes a `ShadowSource` (the same resolution result the content path uses);
- `recordScreen` packs binding 8 with `buildShadowBlock(shadow, draw)` (an empty source means `params.x = 0`, the engine's
  "no shadow reaches this pass") and binds binding 7 to the **real map** with the shadow sampler when
  `shadow.view != VK_NULL_HANDLE`, and otherwise to the white stand-in as before;
- `Device::executeFrame` passes the pass's already-resolved `shadow` in.

## Verification

- New case `VkShadowTest.AScreenPassShadesWithTheMapItResolved`: a screen program samples binding 7 directly and writes
  depth. The left strip is 0.25 in the map (the content stage folds clip z = 0.5 into device depth) and the right side is
  the clear value 0; a white stand-in would read 255 in both. **Mutation proof**: changing binding 7 in the screen set back
  to the constant white stand-in turns the case red; restoring it turns it green.
- `test_vk` **72/72** (lavapipe, validation silent); `BackendParityTest` in `test_vsg` **12/12**.
- Application: the 332x247 render area is **byte-identical** between vk and vsg on the same scene (0 pixels differ, against
  1.88% before the fix).

## Design constraints

- **G4** (interface semantics): having the binding slot in `makeScreenSet` is not the same as binding the right image;
  "the map this pass resolved" has exactly one source (`Device::resolveShadow`) and the screen path must take the same
  answer.
- **G6** (uniform and consistent): one `FrameDescription` must draw the same picture on both backends; this was the shape
  difference "content path shadowed, full-screen path not".
- **G11** (debuggable): the symptom was "forward has shadows, deferred does not, zero diagnostics". The criterion should be
  able to ask "which image is bound at binding 7" directly, and the case turns that question into a number by sampling the
  map and writing depth.
