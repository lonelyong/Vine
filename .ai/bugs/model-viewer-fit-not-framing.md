# Bug: the model viewer does not aim at the content it opens, and "fit to screen" pushes the model out of reach of zooming back

- **Date**: 2026-10-26
- **Module**: plugins/model_viewer + viz/graphics
- **Status**: Fixed

## Symptom

After opening a millimetre-scale STL (about 1000x1000x510 with the origin off the model's centre) through
`open_model_file`:

- the initial camera was not aimed at the content: the model filled the picture and could not be seen as a whole;
- the right-click menu's "fit to screen" made it worse: the model was made especially large, and the wheel could hardly
  zoom back out.

## Root cause

Two layers on top of each other:

1. `ModelPickManipulator` built its inner `OrbitCameraManipulator` **without the scene** (`OrbitCameraManipulator(camera)`).
   `fitToScreen()` frames with `scene->boundingBox()`, and without a scene it returns false on the null pointer - so
   `SurfaceWindow::fitToScreen()` fell back to `home()`, which is the camera as it was when the manipulator was built: the
   default `eye=(0,0,5)` and `target=origin`. A millimetre-scale model has a half diagonal of about 750, so the default
   viewpoint naturally looks enormous, and the wheel step scales with the current distance, so starting from 5 needs a
   thousand notches before anything changes.
2. `fitToScreen()` changed the distance only and not the camera's near and far planes. The camera's default `far` is 1000,
   while the framed distance of this model is about 2200 - even a successful framing would put the whole model beyond the
   far plane (an empty picture).

## Fix

1. The `ModelPickManipulator` constructor takes `scene` and hands it to the inner `OrbitCameraManipulator`;
   `ModelRenderView::buildScene()` passes `view_->scene()`.
2. `OrbitCameraManipulator::fitToScreen()` scales the near and far planes with the framing distance (`near = 1% of d`,
   `far = 100x d`, keeping the camera's default 1:10000 depth ratio); both the perspective and the orthographic branch do
   this.

## Verification

- `ThePickManipulatorFramesTheSceneItDraws` in `tests/test_gui/ModelViewerTest.cpp`: a millimetre-scale cube (side 2000)
  is asserted to make `fitToScreen()` return true, aim the camera at the box centre, scale the distance with the content
  size, and satisfy `near < d - r` and `far > d + r`. With the scene argument removed, the case fails at the
  `fitToScreen()` assertion (verified by running it).
- `test_gui`, `test_graphics` and `test_vsg` are all green.
