# Bug: the demo builds a `PassChain` and never adopts it, so the chain-owned scene is not published and the size is not maintained by the view

- **Status**: Fixed (2026-11-14, `demo_plugin`)
- **Found**: 2026-11-14
- **Module**: `src/plugins/demo_plugin` (host-side wiring; the engine contract itself is correct)
- **Files**: `src/plugins/demo_plugin/src/DemoScene.cpp` (`addDemoPipeline`),
  `src/viz/graphics/src/PassChain.cpp` (`PassChain::build` does not adopt the chain itself), `src/viz/graphics/src/SceneView.cpp`
  (`setChain` / `publishContent` / `applySurfaceGeometry`)

## Symptom and impact

After the demo opens, the log reports one line on every frame:

```
[graphics] pass 'forward_transparent' draws a scene that was never published: publish it (Scene::publish),
or bind a publication before the frame (RenderEngine::bindSnapshot)
```

That pass therefore **draws nothing**: the overlay content of the deferred path (skybox, blended volumes) is missing entirely.
At the same time the chain's offscreen targets and HUD box were not maintained by the view, so the demo added a
`pipeline->resize(...)` call of its own in `addSurfaceLayout` - two sources of one size, and any change to the view's layout
behaviour would drift the other one.

**Why this is not the backend's fault**: `vsg` and `vk` both report the same warning; publishing is an engine/host
convention and has nothing to do with the backend.

## Root cause

`PassChain::build()` only **builds and registers** (`ChainBuilder` + `apply`) and **hands the chain to no view**; adoption
is the host's verb (`SceneView::setChain`). `Viewer` did it right, the demo did not:

- `SceneView::publishContent()` does "publish `scene_` first, then `pipeline_->publishContent()`" (the latter publishes
  the `transparent` scene the chain owns);
- `SceneView::applySurfaceGeometry()` calls `pipeline_->resize(width, height, ratio)` in its own layout step.

The view's `pipeline_` was empty, so neither happened. The chain stayed alive only because the demo's layout lambda captured
it; a chain that is only captured does not exist as far as the view is concerned.

## Fix

`addDemoPipeline` calls `view->setChain(pipeline)` right after building the chain; the demo's own layout step keeps only
the half that re-arranges the G-buffer preview viewports by the source image's aspect ratio and no longer repeats the
`resize` (the view's step runs first and has already fitted the chain to the surface). `surface->present(presentingPass())`
is unchanged - that answers "who presents", which is a different question from adoption.

## Verification

- Application (once on vk, once on vsg, `--open demo`): the `never published` count is **0** (it was 1 per run before the fix).
- Picture: the sky and overlay content appear on both backends after the fix; the same-size render area differs between vk and
  vsg by **1.88%** of pixels (only the FPS digits and the gizmo's timing pixels).
- After a window resize (`xwinresize.py 0x<top> 1200 800`) the picture follows with no warning - the chain's size really is
  maintained by the view.

## Design constraints

- **G3** (create, configure, run, get the result and release must read in one pass): building a chain and adopting it are two
  verbs, and `PassChain::build` not adopting itself is **deliberate** (the host decides which view draws it), but the
  documentation and the example must show the two steps as one usage.
- **G4** (interface semantics): "the chain publishes the scene it owns" in `publishContent` only holds once the chain is
  adopted; not adopting it produces no error, only a one-off warning - which is why the symptom was so quiet.
- **G6** (uniform and consistent): one fact (the chain's size) had two drivers (the view step and the demo step); after the
  fix only the view drives it.
