# Bug: the newly exposed area is black first after maximising (and what a middle frame should show) (Historical record)

- **Date**: 2026-09-17
- **Module**: gfx_backend_vsg + appfw/gui
- **Status**: Fixed

> **Historical code locations**: `VsgRenderer` / `SceneBridge` were removed in the backend rewrite; the file and class
> names below are kept as they were recorded then; the current structure is in `.ai/design/vsg-backend.md`.

## Symptom

After maximising (or widening the window) the **newly exposed part of the render area was black first** and only filled in
after about 250 ms.

## Root cause (measured on a real Windows machine, three causes)

How the evidence was gathered: `Vine.exe` was run with the default deferred pipeline and `scripts/win_maximize_probe.ps1`
(`ShowWindow(SW_MAXIMIZE)`), and the timeline was read from **temporary** instrumentation (window extent in
`beginFrame`/`endFrame`/`resize`, the window `RenderGraph`'s `renderArea`, each slot's viewport rectangle, and the rebuild
timestamps of each target/slot).

1. **The window's shared `RenderGraph` `renderArea` lagged a frame.** The window graph built by `VsgRenderer::initialize()`
   (`targets[nullptr].graph`) only received the extent it had at creation; afterwards vsg itself resized it inside
   `RenderGraph::accept()` when it noticed an extent change **during recording**. The frames after a resize therefore still
   cleared and drew by the **old rectangle**: the newly exposed part was neither cleared nor drawn, and the swapchain image
   had just been recreated, so it was black to begin with.
   One measurement: `resize announced=1176x444 live=2352x888 graphRenderArea=752x480+0,0` -> the frame still ended with
   `renderArea=752x480`.
2. **"The first frame is the expensive one."** `RenderControl::handleSurfaceUpdate()` runs the creator's layout step first
   (`view->onSurfaceResized()` -> rebuild the whole offscreen chain: gbuffer, composite and the full-screen program slots
   that sample them) and only then `renderFrame()`; that frame takes ~250 ms, and **no frame was presented at the new size**
   in the meantime.
3. **The same resize made every full-screen program slot in the window stale**: a `ProgramSlot`'s rebuild identity included
   the target **surface** size (`dest_w`/`dest_h`), so a single window resize rebuilt the nodes of the window's 5-6 programs
   (~20-40 ms each).

## Fix

1. `VsgRenderer::resize()` (`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`): write the window graph's
   `renderArea` / `viewportState` to the new extent immediately and set `previous_extent` to the same value. Synchronising
   `previous_extent` also **turns off vsg's own scaling path**, which would scale the already-correct rectangle by new/old
   once more: measured, the HUD overlay's rectangle went `16,368 96x96` -> `50,1436 300x177` (outside a window that is 888
   high) and the fps overlay -> `7003,1561` (x > 2352); after the fix they are `16,776 96x96` / `2239,844 105x36`, both
   inside the window.
2. Remove the target surface size from a program slot's rebuild identity
   (`src/plugins/gfx_backend_vsg/src/VsgProgramSlot.cpp`, fields in
   `include/vine/backend_vsg/VsgRenderTargetEntry.hpp`): the node's geometry is a full-screen triangle and the rectangle is
   **dynamic state** (written from the pass's viewport into `slot.camera->viewportState` every frame), and every full-screen
   fragment stage samples by `vn_uv`, independent of size. That "the rectangle is dynamic, not baked" is **proved by
   contradiction** by the self-test's PiP phase: the PiP samples the **whole** source into a small 96x54 rectangle, while its
   pipeline was baked at the **surface** size.
3. What a middle frame should do (`src/fw/appfw/src/gui/RenderControl.cpp::handleSurfaceUpdate`): **keep one frame**.
   Presenting a frame before the layout step was tried (the offscreen chain still had the old size, so the old picture was
   **stretched** to fill the new window; the first frame was submitted after 26 ms) and then **reverted** after looking at it
   on a real machine: the stretching disfigures the picture and is harder to accept than the new area filling in ~250 ms
   later. **Rule: the picture is never distorted, at any moment.**

## Files involved

- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`
- `src/plugins/gfx_backend_vsg/src/VsgProgramSlot.cpp`
- `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/VsgRenderTargetEntry.hpp`
- `src/fw/appfw/src/gui/RenderControl.cpp`

## Verification

- Full build green; `test_vsg` / `test_graphics` / `test_gui` pass; `vsg_backend_selftest` green (the `[host-surface]
  move:` line plus 0 VUID plus `[selftest] done`).
- Real machine (Windows 11 + RTX 4060): after `ShowWindow(SW_MAXIMIZE)` the rebuild sequence is normal (gbuffer plus
  composite plus 6 full-screen program nodes), with 0 VUIDs and no crash; the rebuild frame covers the whole new window,
  because `renderArea` was already written correctly.
- **Still left at the time (H9, fixed later)**: the rebuild frame itself took ~240 ms (6 full-screen program nodes ~180 ms
  plus 2 targets ~40 ms; **glslang was only ~50 ms** of it, the rest was vsg building a pipeline and descriptors per node).
  During that time the screen showed the old picture (undistorted), and the window's new part filled in when that frame
  landed. Shortening it further would mean changing the slot rebuild strategy (re-point the descriptors in place plus
  dynamic viewport state), because after a resize the **source image view changes**, so the nodes have to be rebuilt.

  > **H9 fixed (2026-09-19) - that "still left" is now 22 ms**: the slot rebuild strategy became "resize the target in place
  > and re-point the sampler's descriptors", as guessed at the end of this section; but **the clause "the nodes have to be
  > rebuilt" was wrong**: `GraphicsPipeline::compile` reuses implementations inside the same `GraphicsPipeline` object by
  > comparing pipeline states (the comparison does **not** include the render pass), so keeping the node and the View is an
  > early exit and the pipeline is not rebuilt. Measured: maximising 752x480 -> 2352x888 went from **225.5 to 36.4-51.4 ms**
  > (`targets 0`, `target resizes 2`, `rebind compiles 1`, of which 22-36 ms is one slot built the first time a new panel
  > became visible and has nothing to do with the resize), restoring **-> 2.2 ms**, and `device_waits` does not grow. The
  > pixel side is covered by the new self-test phase `runTargetResizePhase` (a copy program fills the new size, centre and
  > corners correct); the design and the measurements are in `.ai/design/vsg-backend.md`. The old performance to-do list has
  > been deleted; the current open items are in `.ai/backlog/graphics.md`.
