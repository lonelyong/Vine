# ADR-2026-11-26: the host-program contract is the push range, and it is stated in the SDK

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/graphics-shader-params.md` (§2.8, §4.4, C9), `ShaderAbi.hpp` (`VinePushRange`),
  `Camera::projectionMatrix` (`src/viz/graphics/sdk/vine/graphics/Camera.hpp`), the vk backend's
  `ContentDraw::foldToDeviceClip` / `pushConstantsOf` and `ContentCaches::ensureContentPipeline`,
  `ShaderProgram.hpp` (a host may supply GLSL)

## Background

A host may supply a GLSL program for a content draw, and the program is handed two matrices per draw. What a host
author could ASSUME about them was stated in three places, none of them the host's:

- the layout (projection, then model-view) was the backend's PRIVATE `ContentAbi.hpp` struct;
- the fact that `projection` is already folded into the device's clip convention (Y down, reverse-Z) lived in
  `ContentDraw.cpp`'s comment, while the public `ShaderAbi.hpp` documented a `VineViewBlock::proj` as "view -> clip"
  with no such word - and `VineViewBlock` was bound by nobody, because the content path implements the view as the
  range;
- two design references pointed at a `buildViewBlock` that does not exist (`CameraSnapshot.hpp`,
  `StateMapping.hpp`), so the reader was told to look for a function that was never written.

`VineViewBlock` itself was the clearest symptom: 288 bytes of view data (view, inverse view, projection, view-proj,
camera position, time) declared in the public SDK, static-asserted, size-pinned by a test - and uploaded by no backend.
A shape nobody binds is worse than no shape: it promises a channel a host cannot receive and makes the ABI page
describe a block that has no producer.

## Evidence

| Fact | Where |
|---|---|
| A host supplies GLSL and receives no statement of what its stages are given | `ShaderProgram.hpp` |
| The range's layout, size and fill are one backend's | `ContentAbi.hpp` (`PushRangeConstants`), `ContentDraw::pushConstantsOf` |
| The projection is folded into the device convention at one boundary | `ContentDraw::foldToDeviceClip` (near -1 -> 1, far +1 -> 0, y negated) |
| Nothing uploads, declares or binds `VineViewBlock` | `grep -rn VineViewBlock src` reaches comments, the struct and its `static_assert`s |
| A full-screen draw receives no camera data at all | `ContentDraw::recordFullscreen` (no push range; the draw block is empty) |
| The device guarantees 128 bytes of push constant, not more | Vulkan's minimum `maxPushConstantsSize` |

## Decision

1. **The push range IS the host-program contract.** Its layout is `vn::graphics::VinePushRange` in the SDK:
   `projection` then `model_view`, both column-major `mat4`, 128 bytes, asserted in `ShaderAbi.hpp`.
2. **The two conventions are stated beside the layout**, in the header a host reads:
   - `projection` is the pass' projection **folded into the device's clip convention** by the backend at the one
     boundary it owns - it is not `Camera::projectionMatrix()`, which stays in the SDK's convention for the culling and
     the CPU-side queries;
   - `model_view` is `view * model` for this drawable, so the range carries no separate model matrix, no normal
     matrix, no camera position and no time;
   - a full-screen draw receives no range at all.
3. **`VineViewBlock` is deleted.** The engine declares no block it does not bind (`G8`, `G12`); the view's one spelling
   is the range, and its one declaration is the SDK header. The dead `buildViewBlock` references become references to
   the function that actually folds the projection.
4. **The backend keeps only its own half**: it aliases the SDK type (`using PushRangeConstants = VinePushRange`), sizes
   its pipeline layout from `sizeof`, and fills the two values. A second backend inherits the contract instead of
   rediscovering it from a private header.

## Rejected options

| Option | Why not |
|---|---|
| Keep `VineViewBlock` and bind it as a uniform block | No consumer today (`G8`), and one draw would then carry two spellings of the view (the range and the block) with the pipeline layout, the descriptor set, the shader declaration and the packing to keep in step (`G1`, `G7`). |
| Leave the layout in the backend and document it in the guide only | A host reads headers, and a second backend would re-derive the contract from a private one (`G6`). |
| Give a host its own push range or a second range | The device guarantees 128 bytes in total, and the built-in stages already use all of it. |
| Fill `VineViewBlock` into a buffer for every content draw | A per-draw buffer that only a host program reads is a cost paid by every draw for a feature nobody in the tree asks for. |

## Consequences

- A host stage that wants more than the two matrices - the camera position for a fog, the inverse projection for a
  depth reconstruct, an unfolded clip space, the frame time - cannot get it. That is a stated boundary now, not a
  missing block; adding it is a new decision with a consumer (the trigger the original review named: the first host
  post-processing program, or the second backend).
- Every backend must produce the same two values or the picture is visibly wrong, which is what the content-cache and
  draw tests already pin; the host-program contract itself is a header statement, and `ShaderAbiTest` pins its size and
  offsets.
- Deleting `VineViewBlock` changes no byte on any device: nothing uploaded it.
