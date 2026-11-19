# A layout step runs from `SceneView`'s destruction and calls back into the widget that owned the view

- **Status**: Fixed (2026-11-20). `SceneView` runs no layout step while it is destroyed, and dropping a chain applies
  no geometry; the demo's step captures the view instead of re-entering `RenderControl::view()`.
- **Found**: 2026-11-20 (shutdown crash of `Vine.exe`; dump `build/bin/Debug/Vine_crash.dmp`)
- **Module**: `src/viz/graphics`, `src/fw/appfw`, `src/plugins/demo_plugin`
- **Files**: `src/viz/graphics/src/SceneView.cpp`, `src/viz/graphics/sdk/vine/graphics/SceneView.hpp`,
  `src/plugins/demo_plugin/src/DemoScene.cpp`

## Symptom and impact

Closing the application crashed in `RenderControl::view()` (`RenderControl.cpp:135`), reached from the demo's layout
step (`DemoScene.cpp:1668`) through `LayoutParticipant::applySize`. The stack is a pure teardown chain:
`~GuiApplication` -> `~CentralDocumentHost` -> `~SurfaceWindow` -> `~SceneView` -> `SceneView::setChain` ->
`applySurfaceGeometry` -> the step -> `RenderControl::view()` on a control whose `Impl` was already destroyed.
The crash is at exit, so the visible symptom is "the application crashes when it closes" - after the picture was
already correct, which is why no functional test noticed.

## Root cause

Two rules met.

1. `SceneView::~SceneView` dropped the adopted chain with `setChain(nullptr)`, and `setChain` applied the surface
   geometry on **every** change - including a drop. That ran the creator's layout steps from the destructor.
2. A layout step is the creator's closure over its own objects, and the demo's captured the `RenderControl` and asked
   it for `view()` / `chain()`. The view is owned by the `SurfaceWindow` the control creates, and Qt's widget tree
   destroys that `QWindow` **after** the control's own `Impl` - so by the time `~SceneView` ran, `dptr()` was gone.
   The step's guard (`render_control->view() == nullptr`) could not help: calling a method on a destroyed object is
   the defect itself.

## Fix

- `SceneView::~SceneView` clears the layout steps first and drops the chain without a geometry pass.
- `SceneView::setChain` applies the surface geometry only when a chain is **adopted**: a drop has nothing to size and
  a host step has no subject.
- `SceneView::addSurfaceLayout` states the contract: a step runs only while the view is alive, and only from a
  geometry event (a resize, an adoption, presentation on a sized area); destroying the view runs none.
- The demo's step captures the view (which owns it, so it cannot dangle while the step runs) for `chain()`, and keeps
  `render_control` only as the plan's owner identity - one value, not a call.

## Prevention

- Rule: **a creator callback is never invoked from a destructor path**. The object that owns the callback is the only
  one guaranteed alive when it runs; a step that reaches back to the callback's registrant may be called during that
  registrant's teardown.
- Test: `SceneViewViewportTest.ALayoutStepNeverRunsWhileTheViewIsTornDown` - adoption runs the step once,
  `setChain(nullptr)` and destroying the view run none.

## References

- `src/viz/graphics/src/SceneView.cpp` (`~SceneView`, `setChain`, `applySurfaceGeometry`)
- `src/plugins/demo_plugin/src/DemoScene.cpp` (`addDemoPipeline`'s layout step)
- `.ai/design/graphics-architecture.md` F20 (the surface size has one owner; views follow through `resize` /
  `addSurfaceLayout`)
