# A vk full-screen draw inherits the pass's depth policy, so the window and composite targets are discarded entirely by the depth test

- **Status**: Fixed (2026-11-14, `gfx_backend_vk`; regression case `VkContentDrawTest.AScreenCopyIsDrawnWhateverThePassDepthPolicySays`)
- **Found**: 2026-11-14
- **Module**: `src/plugins/gfx_backend_vk`
- **Files**: `src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp` (`Impl::planScreen`),
  `tests/test_vk/ContentDrawTest.cpp`, reference rule: `.ai/design/vsg-backend.md` section 11.16bl (M4b)

## Symptom and impact

`Vine --open demo` on vk showed **only the HUD axes and FPS digits**, with the 3D area entirely black; the same code is
fine on vsg. The render area reads 98.8% near-black. The `VN_FRAME_PLAN` frame table listed all 8 passes, the recording
probe showed `recorded=1` for every draw, and the validation layer reported **zero VUIDs** - "recorded, not drawn".

Two shapes were hit at once:

- **Window pass**: `WindowTarget`'s render pass carries D24, and the first window pass clears it to 0.0 (the far plane
  under reverse-Z);
- **The deferred chain's composite**: it has no depth of its own, and `RenderPass::setDepthView` makes it **borrow** the
  G-buffer's depth (`RenderTarget::shareDepth`).

## Root cause

`planScreen` resolved the full-screen call's state with `resolveRenderState(RenderState{}, pass.state, Triangles)`, and the
depth policy in `pass.state` defaults to `DepthMode::TestAndWrite`. A full-screen call is the canonical triangle at z = 0.0,
which under this engine's reverse-Z convention is the **far plane**; a `GREATER` test against either target above fails for
every fragment, so the whole `present` / `gbuffer_preview` / deferred-lighting output was discarded. The clear value
(black) stayed in the target, so "drawn" equalled "not drawn".

Why the test suite missed it: the screen fixture in `tests/test_vk` (`screenFrame`) sets `pass.state.depth` to `Disabled`
**explicitly**, while the passes of a real engine chain do not; and the window-path cases draw only `Kind::Scene`. The
missing shape was "full-screen draw + target with depth + a pass depth policy that is not explicitly off".

## Fix

`planScreen` forces the pass state's depth policy to `Disabled` before resolving:

```cpp
vn::graphics::PassRenderState screen_state = pass.state;
screen_state.depth                         = vn::graphics::DepthMode::Disabled;
plan.resolved = resolveRenderState(RenderState{}, screen_state, Topology::Triangles);
```

A pass's depth policy speaks for the **content** it draws (the reference backend's M4b rule: a full-screen call is always
`Disabled`, and the canonical triangle sits at z = 0.0); the backend must not apply the content policy to that call.

## Verification

- New case (a target with D24 plus the default pass state): red before the fix (the target centre keeps the clear black),
  green after it. **Mutation proof**: switching `screen_state` back to `pass.state` and rebuilding turns the case red.
- `test_vk` **71/71** (lavapipe, validation silent); `BackendParityTest` in `test_vsg` **12/12**.
- Application: the vk render area shows a picture (the composite centre is the ambient-lit ground colour), and the
  same-size render area differs from vsg by **1.88%** of pixels.

## Design constraints

- **G4** (interface semantics): `PassDraw::state` is "the depth policy this pass states for its content", not "the final
  state of every draw"; a backend must resolve it by the ABI rules instead of applying it wholesale.
- **G6** (uniform and consistent): one `FrameDescription` must draw the same picture on both backends - this was the
  textbook "same frame, two results", and the coverage hole in `BackendParityTest` (a screen target with no depth) was
  exactly the missing cell.
- **G11** (debuggable): the symptom was "recorded, zero VUIDs, not drawn"; locating it took a recording probe plus copying
  the GBuffer into a readable target, not reading code and guessing.
