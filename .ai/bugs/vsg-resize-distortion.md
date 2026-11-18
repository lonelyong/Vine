# Bug: geometry is distorted (squashed or stretched) while the window is resized (Historical record)

- **Date**: 2026-09-02
- **Module**: viz/graphics + appfw/gui + gfx_backend_vsg
- **Status**: Fixed

> **Historical code locations**: `VsgRenderer` / `SceneBridge` were removed in the backend rewrite; the file and class
> names below are kept as they were recorded then; the current structure is in `.ai/design/vsg-backend.md`.

## Symptom

Dragging a window edge to change the window's aspect ratio squashed or stretched the triangle, so its proportions were
wrong.

## Root cause

The application attached no `CameraManipulator` to its `RenderEngine` (`camera_manipulator_` defaults to empty).
`RenderEngine::pushEvent(ResizeEvent)` used to update the camera projection aspect **only when a manipulator existed**, and
otherwise only called `backend->resize()`. Therefore:

1. the camera aspect stayed at its default `1.0` from initialisation on and never followed the window aspect ratio -> the
   projection frustum and the viewport disagreed -> the geometry was distorted;
2. `VsgRenderer::resize()` rebuilt the swapchain only and did not update the vsg camera's `viewportState` - and
   `RenderGraph` takes the render area from it every frame, so the picture kept rendering into the initial area;
3. no `ResizeEvent` was posted after the first attach, so the first frame rendered with the wrong aspect (a non-square
   initial window was distorted from the start).

## Fix

1. `RenderEngine::pushEvent(ResizeEvent)` (`src/viz/graphics/src/RenderEngine.cpp`): with no manipulator, update the main
   camera's aspect from the new window's `w/h` (`setPerspective(fov, w/h, near, far)`), the same thing
   `OrbitCameraManipulator::onResize()` does.
2. `RenderControl::initializeBackend()` (`src/fw/appfw/src/gui/RenderControl.cpp`): after a successful first attach, push
   one `ResizeEvent{surfaceWidth(), surfaceHeight()}` before the first `renderFrame()`, so the first frame uses the right
   aspect.
3. `VsgRenderer::resize()` (`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`): after `window->resize()`, update
   `d->vsg_camera->viewportState = ViewportState::create(window->extent2D())` so the render area follows the live window
   size.

## Verification

After a window resize the geometry keeps its proportions and is no longer squashed or stretched.

## Follow-up (M2c, 2026-10-11 - reverted, superseded by A, see below)

The fix at the time was "**remember** to push the new size to the camera" (items 1 and 2 above) - one state maintained in
two places, and one omission distorted the picture again. M2c moved the aspect out of the camera (the projection computed
`projectionMatrix(aspect)` on demand, one derivation in `RenderBackend::viewAspect()`), and that version was **reverted the
same day**: see "follow-up two" below.
See `graphics-architecture.md` section 3.2.3 (item 3 and its "revert") and the "aspect out of the camera (M2c)" entry in
`.ai/memory/graphics.md`.

## Follow-up two (A, 2026-10-11, same day): the shape returns to the camera, with a single source

The `viewAspect()` version turned "the shape" into something **only the backend could answer, and whose answer depended on
what had been announced before** (it read the open scope's target/viewport), so "assemble the whole frame description before
dispatch" became unexpressible. A put it back **into the camera**: `Camera::setAspect(w / h)` is the only write entry for
the shape, and the projection, the culling, the view block and the picking rays read the same number - the state "the
camera's aspect is stale" still does not exist (there is one writer, and it is written by the **same** rectangle:
`SceneView::applySurfaceGeometry` pushes the pane's shape), and "the rectangle disagrees with the camera" (one camera shared
by two differently shaped rectangles) is no longer silently stretched but reported at engine compile time
(`RenderEngine::collectPictureShapeMismatches()`).

**So neither of this bug's two "at the time" fixes needs to come back**: neither "remember to push to the camera" (the view
pushes once, because the shape really is its pane's) nor "ask the backend" (the shape lives in the camera and is answerable
at compile time).
See `.ai/design/graphics-architecture.md` "landed: A" and the A entry in `.ai/memory/graphics.md`.
