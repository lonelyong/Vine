# appfw's render view

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the host-facing side of `RenderControl` / `Viewer` / `SceneView` / `PassChain`; the surface lifetime is in `appfw-render-surface.md`, and the graphics layer's recipes and pass scheduling are in `graphics-architecture.md`.
> **Sources**: `RenderControl.hpp`, `Viewer.hpp`, `ViewerOptions.hpp`, `SceneView.hpp`, `PassChain.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

A host that wants a 3D area otherwise repeats the same bookkeeping — build a pipeline, follow the window's resize, fill in the DPR, route the input — and the graphics SDK mixes the **recipe** (which chain, which overlays) with the **geometry** (camera, content, viewport) on the view.
Both sides need one clear division.

### 1.2 Goals

- A host gets a 3D area that draws, zooms and reacts with one line of configuration.
- A host that wants to go deeper builds its own chain and declares its own main pass without the higher layer standing in the way.
- A recipe change pays for one chain build; a run-time switch (shading mode, overlay visibility) rebuilds nothing.
- One area shows one picture, and there is no second answer for clearing, order or presenting.

### 1.3 Non-goals

- Multi-pane / split / PiP in the framework: the app computes the rectangles and hands the pass its sub-viewports.
- One top-level window per document: one control is one area today.
- Selection / highlight / rubber-band in the framework: picking is in the SDK and selection is business logic.
- More than four simultaneous 3D documents: `VSG_MAX_DEVICES=4`, and more needs a patch to vsg.

### 1.4 One-paragraph conclusion

`RenderControl` gives the host a 3D area; `Viewer` turns the host's request (`ViewerOptions`) into one `PassChain` and adopts it on the `SceneView`; a host that wants to go deeper calls `PassChain::build()` + `view()->setChain()` itself, and the higher layer stands down once the lower one appears.

## 2. Design Points

### 2.1 Objects and ownership

| Object | Owns | Does not own |
| --- | --- | --- |
| `RenderControl` (the public control) | the private `SurfaceWindow`; forwards `engine()` / `view()` / `viewer()` / `surface()` / `init()` / `state()` | builds no chain and interprets no recipe |
| `SurfaceWindow` (the private implementation) | the native `QWindow`, the `RenderEngine`, the `SceneView`, the `Viewer`; the attach rules, deferred resize, Qt events → view input | keeps no second copy of the surface or size facts |
| `RenderArea` (graphics layer) | one area's presentation declaration, size and target; it only **resolves** the declaration | builds no chain, creates no view, guesses no default pass |
| `SceneView` (graphics layer) | camera, content scene, manipulators, 2D groups and input, **the chain it adopted** (a strong reference) | owns no recipe and no surface |
| `PassChain` (graphics layer) | one chain's passes, offscreen targets, overlays and the built `ChainDesc`; it hands the passes back to the engine when destroyed | owns no engine (a lease) and no view |
| `Viewer` (appfw) | **one area's request** (`ViewerOptions`) plus building, adopting, declaring and carrying run-time choices across a rebuild | owns no presentation (`isActive()` reads the area) and no view |
| `RenderEngine` (graphics layer) | the device, the pass slots, the frame scheduling | knows neither views nor chains |

**One hard edge**: `SceneView` → `PassChain` is a strong reference; `PassChain` → `RenderEngine` is a borrowed lease; `RenderEngine` → `SceneView` / `PassChain` has no edge (the engine knows only passes).

### 2.2 The two API layers

**The fast layer** (`RenderControl` / `Viewer`): `viewerOptions()` / `setViewerOptions()` / `setShadingMode()` / `release(view)` / `view()` / `isActive()` / `takeArea()`.
`ViewerOptions` is a value: `path`, `mode`, `gizmo` / `stats`, `offscreen_width/height`, `layer_mask`.

**The low level** (the graphics SDK): `PassChain::build(engine, desc)`, `view()->setChain(chain)`, `surface()->present(pass)`, `view()->setManipulator(...)`.

**The standing-down rule**: once the host declares its own main pass on the area, `Viewer::isActive()` is false and `takeArea()` builds no default chain; `release(view)` hands the view back to the host.
Once the lower level appears, the higher one stops deciding for it.

### 2.3 One area draws one picture

`RenderArea` has one presentation; a second one is refused and reported once.
Several pictures are the app's business: it computes the rectangles, gives the pass its sub-viewports and decides the clearing itself.
When the host takes over, `surface()->present(pass)` declares its main pass and the default chain stands down.

### 2.4 Input has one channel: the manipulator

`SceneView` pushes window events to its manipulator (`CameraManipulator`); a host or plugin that wants interaction swaps the manipulator, and there is no second route through `setInputRouter` / `focusedView`.
Picking is in the SDK (`Camera::screenToWorldRay` + `RayIntersection::intersectScene`); selection, highlighting and rubber-banding are business logic.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | Construction only assembles; attaching has an explicit entry | `new RenderControl()` chooses no backend and attaches nothing; `init()` / `initAsync()` / the first-turn fallback attach | landed |
| F2 | `Viewer` turns a request into a chain | `setViewerOptions()` builds, adopts and declares on the area | landed |
| F3 | Rebuild only when the recipe changes | changing `ViewerOptions` = one chain build; shading mode / overlay visibility rebuild nothing | landed |
| F4 | One area, one picture, and host takeover | a second presentation is refused; a host main pass makes the default chain stand down | landed |
| F5 | Input and picking | events go to the manipulator only; picking is in the SDK, selection in business code | landed |
| F6 | Several 3D documents, each with its own device | one `RenderControl` per document; the cap is 4 and the fifth fails with a reason | landed |

### 3.1.1 F1 Construction only assembles; attaching has an explicit entry

- **Behaviour**: the constructor builds the engine, view, viewer and diagnostic routing and chooses no backend; the first attach comes from the first-turn event-loop fallback, or from the host's explicit `init()` / `initAsync()`.
- **Constraints** (C1): `engine()->setDevice()`, `viewer()->setViewerOptions()`, `view()->setChain()` and the content assembly must all be able to run synchronously after construction and before the first attach.
- **Requirements** (R1): no chain is built and no pipeline compiled before the attach; configured before the attach = zero rebuilds.
- **Acceptance** (T1): `RenderControlTest.ConstructionOnlyComposesAndTheFirstTurnAttaches`.
- **Caveat** (N1): `initAsync()` is `co_return init()` — it does not suspend and goes to no pool; the device, session, pipeline and warm-up all run on the application thread.

### 3.1.2 F2 `Viewer` turns a request into a chain

- **Behaviour**: `setViewerOptions(options)` builds a `PassChain` from the `ViewerOptions`, adopts it on `view()` and declares the presentation on the `RenderArea`. With no request, `takeArea()` builds one from the default `ViewerOptions`.
- **Constraints** (C2): the chain must be built for this view's camera and content; `setChain` does not check, and the engine reports a shape mismatch.
- **Requirements** (R2): setting the same request again must rebuild nothing (idempotent); with a host declaration already on the area no default chain may be built.
- **Acceptance** (T2): `TheControlsSurfacePresentsItsOwnViewByDefault`, `ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt`.
- **Caveat** (N2): `Viewer` owns the request only; `isActive()` reads the area, not a memory.

### 3.1.3 F3 Rebuild only when the recipe changes

- **Behaviour**: `ViewerOptions` is a build request; one change = one chain build.
  A `ShadingMode` the chain can already express is just one override through `PassChain::setShadingMode()`; overlay visibility is `setGizmoVisible()` / `setStatsVisible()`.
  Switching a mode or a toggle **only changes state on the chain and produces no frame by itself**: the area renders on demand (attach, resize, input, `requestFrame()`), so whoever changes the state requests the frame — the model's shading command (`ShadingModeCommand`) calls `requestFrame()` after it changes (a request, not a synchronous `renderFrame()`: the draw lands in the display-sync turn, coalesced within it, and nothing is drawn while off screen).
- **Constraints** (C3): `PassChain::effectivePath(desc)` is the single rule for "which chain will this request build": a Deferred request with a non-Lit mode builds Forward, but `desc.path` is kept, so switching back to Lit builds Deferred again.
- **Requirements** (R3): a run-time switch rebuilds nothing; a rebuild must carry the run-time choices (an overlay the user hid stays hidden).
- **Acceptance** (T3): `ARebuildCarriesTheLiveChainsRuntimeChoices`.
- **Caveat** (N3): after `setViewerOptions()` rebuilds, an existing declaration on the area is left alone and re-resolves to the new chain's presenting pass.

### 3.1.4 F4 One area, one picture, and host takeover

- **Behaviour**: `RenderArea` has one presentation; a second is refused and reported once. After the host declares its own main pass with `surface()->present(pass)`, `Viewer::isActive()` is false and `takeArea()` does nothing; `release(view)` hands the view back to the host.
- **Constraints** (C4): one area cannot carry both a host pass and the default chain; two pictures in one area have no correct answer for clearing or order.
- **Requirements** (R4): while the host has taken over, `Viewer` builds no chain and declares nothing; after `release(view)` the chain stays adopted on the view and the host replaces it.
- **Acceptance** (T4): `AHostPassTakesTheAreaAndTheViewerStandsDown`, `ReleasingTheViewTakesItOutOfTheArea`.
- **Caveat** (N4): multi-pane / split is not in the framework; the app computes the rectangles and gives the pass its sub-viewports.

### 3.1.5 F5 Input and picking

- **Behaviour**: the `RenderControl` container translates Qt events into view input, and `SceneView` pushes them to the manipulator. Picking uses `Camera::screenToWorldRay` + `RayIntersection::intersectScene`; selection, highlighting and rubber-banding are business logic.
- **Constraints** (C5): input cannot bypass the manipulator; the route is chosen on press and captured until release; a drag end is not a click (the slop lives in the manipulator).
- **Requirements** (R5): navigation behaviour does not change when the manipulator is swapped (a picking manipulator forwards to the inner one unchanged).
- **Acceptance** (T5): `TheViewReceivesInputAndADragKeepsSteeringIt`, `ModelViewerTest.AClickSelectsTheGeometryUnderTheCursorAndHighlightsIt`.
- **Caveat** (N5): clicking empty space is a legitimate deselection, and the original material pointers captured before the selection must be handed back.

### 3.1.6 F6 Several 3D documents, each with its own device

- **Behaviour**: each 3D document has its own `RenderControl` (its own surface / session / device / pipeline); only the DockPanel and ribbon shell are shared at application level.
- **Constraints** (C6): `VSG_MAX_DEVICES=4`; the fifth is refused by a `vsg::Device` constructor throwing, and `deviceFailureMessage()` turns that into a reason.
- **Requirements** (R6): the fifth failure must carry the reason "at most 4 simultaneous render devices" and must not escape to the event loop.
- **Acceptance** (T6): `ModelViewerTest.EachDocumentViewOwnsItsOwnRenderArea`, `VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException`.
- **Caveat** (N6): the fixed array branch for `<=4` does no bounds check, so the fifth must never be written into it.

## 4. Public Interface

### 4.1 `RenderControl` (appfw)

| Interface | Contract | Note |
| --- | --- | --- |
| `engine()` / `view()` / `viewer()` / `surface()` | borrowed, the control owns them | `view()` is null after `release` |
| `viewerOptions()` / `setViewerOptions()` / `setShadingMode()` | forwarded to `Viewer` | see F2/F3 |
| `init()` / `initAsync()` | synchronous, idempotent attach | `initAsync()` never suspends |
| `state()` / `failureReason()` | `Pending` / `Presenting` / `Failed` | `failureReason` non-empty ⟺ `Failed` |
| `renderFrame()` / `fitToScreen()` | render on demand / frame the content | must not push the state to `Presenting` before the attach |

### 4.2 `Viewer` (appfw)

| Interface | Contract | Note |
| --- | --- | --- |
| `setViewerOptions(options)` | build + adopt + declare | a failure keeps the old chain and the old request |
| `setShadingMode(mode)` | try the live chain's switch first, rebuild only if it cannot express it | zero rebuilds first |
| `release(view)` | withdraw the declaration, forget the view, drop the reference | the chain stays adopted on the view |
| `view()` / `isActive()` | read the area / read the view | keeps no second answer |
| `takeArea()` | build and declare a default chain when there is none; stand down when a host pass exists | called on attach |

### 4.3 The graphics SDK

| Interface | Contract | Note |
| --- | --- | --- |
| `PassChain::build(engine, desc)` | the only chain-building entry; a failure returns null and reports once | see `PassChain.hpp` |
| `SceneView::setChain(chain)` | adopt / replace / drop a chain | the chain must be built for this view's camera and content |
| `RenderArea::present(view/pass)` | declares the presentation | one presentation per area |
| `SceneView::setManipulator(...)` | swaps the input handler | the only input channel |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | Configuration completes before the first attach; no chain is built and no pipeline compiled before it. |
| C2 | A chain must be built for this view's camera and content; `setChain` does not check and the engine reports a shape mismatch. |
| C3 | `effectivePath(desc)` is the only request→chain rule; `desc.path` is kept. |
| C4 | One area has exactly one presentation; a host pass and the default chain never coexist. |
| C5 | Input goes through the manipulator only; there is no second route. |
| C6 | `VSG_MAX_DEVICES=4`; the backend must refuse the fifth with a reason. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | Configured before the attach = zero rebuilds; a recipe change after it = one chain build. |
| R2 | Setting the same `ViewerOptions` again is idempotent; no default chain while the host has declared a main pass. |
| R3 | Run-time switches (mode, overlay visibility) rebuild nothing; a rebuild carries the run-time choices. |
| R4 | While the host has taken over, `Viewer` builds and declares nothing; `release(view)` hands the view back. |
| R5 | Swapping the manipulator must not change navigation; a picking manipulator forwards unchanged. |
| R6 | A device-cap failure must carry a reason and must not escape to the event loop. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | One area draws one picture; a second presentation is refused and reported once. |
| N2 | A view adopts at most one chain; `setChain(nullptr)` takes the view out of the frame. |
| N3 | `Viewer` owns the request, not the presentation; `isActive()` reads the area, not a memory. |
| N4 | The engine knows neither views nor chains; a chain only borrows the engine and never extends its life. |
| N5 | `Presenting` ⟺ a frame is already in the surface; before that the area stays hidden. |
| N6 | The public surface has one configuration entry: `ViewerOptions` or `PassChain`, not both. |
| N7 | With no host request the default chain comes from `Viewer::takeArea()`; with a host main pass it is not built. |

## 6. Caveats and Traps

### 6.1 A chain and its view must have the same shape

- How it fails: the chain was built for another camera or content, so the picture belongs to something else; each pass looks legal on its own and nothing downstream notices.
- How to detect it: the engine reports a shape mismatch; `PassChain::draws(scene)` says whether the chain draws this scene.
- How to prevent it: build the chain for this view before adopting it; never reuse a chain across views.

### 6.2 `initAsync()` is not background initialisation

- How it fails: assuming it pushes the device, session and pipeline onto a pool, and therefore changing the scene concurrently or waiting at the wrong moment.
- How to detect it: it is `co_return init()` and never suspends; `InitAsyncCompletesWithoutSuspending`.
- How to prevent it: treat `initAsync()` as the coroutine shape of a synchronous attach; assemble content after it returns.

### 6.3 A host pass and the default chain coexisting

- How it fails: one area with two pictures, and no correct answer for clearing or order.
- How to detect it: `Viewer::isActive()` is false and `takeArea()` does nothing.
- How to prevent it: the host declares its main pass with `surface()->present(pass)` first, or calls `release(view)` first.

### 6.4 The complexity of multi-pane / split spilling over

- How it fails: split views, PiP and focus routing pushed back into the framework, each adding a second answer for surface, input and clearing.
  Detection: an API such as `addView` / `setPaneRect` / `focusedView` appearing is the regression.
  Prevention: the app computes the rectangles, gives the pass its sub-viewports and decides the clearing — the framework only guarantees one picture per area.

## 7. Rejected Options

| # | Rejected | Reason | ADR |
| --- | --- | --- | --- |
| D1 | Multi-pane / split in the framework | The complexity belongs to the business layer that needs it; the framework only guarantees one picture per area. | — |
| D2 | One process-wide session + one `SceneView` per document | Each 3D document carries its own `RenderControl` / device; the shared-session API no longer exists. | — |
| D3 | An independent widget per view | Reparenting the central area recreates the native window; one picture per area with page switching inside a container is simpler. | — |
| D4 | A host pass coexisting with the default chain | Two pictures in one area have no correct answer for clearing or order. | — |
| D5 | `setInputRouter` / `focusedView` multi-route input | Input has one channel, the manipulator, and after multi-pane went away there is no second destination. | — |
| D6 | Putting the recipe back into `SceneView::Options` | The recipe belongs to the chain and the view only adopts it; two configuration surfaces would fork. | — |
| D7 | Silently downgrading a Deferred request to Forward | Request and result stay apart: `effectivePath()` says which chain was built and `desc.path` is left alone. | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
| --- | --- | --- |
| T1 | construction only assembles, the first turn attaches | `RenderControlTest.ConstructionOnlyComposesAndTheFirstTurnAttaches` |
| T2 | the control draws its own view by default; only a change of request rebuilds | `TheControlsSurfacePresentsItsOwnViewByDefault`, `ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt` |
| T3 | a rebuild carries the run-time choices | `ARebuildCarriesTheLiveChainsRuntimeChoices` |
| T4 | a host main pass takes over, releasing the view | `AHostPassTakesTheAreaAndTheViewerStandsDown`, `ReleasingTheViewTakesItOutOfTheArea` |
| T5 | input reaches the view, a drag keeps steering | `TheViewReceivesInputAndADragKeepsSteeringIt` |
| T6 | each document view has its own render area; the fifth gives a reason | `ModelViewerTest.EachDocumentViewOwnsItsOwnRenderArea`, `VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException` |
| T7 | one view with two panes: the target carries the view and the view answers which pane counts | `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast` |

## 9. Status and Evolution

### 9.1 Current status

F1–F6 are in place, and there is no multi-pane or shared-session mode.
`Viewer` is the request half of one area, and `SceneView` adopts one `PassChain`.

### 9.2 Known gaps / backlog

- More than four 3D documents needs a patch to vsg; this tree stays on the unpatched version plus the gate.
- Multiple windows (one top-level window per document) are outside this design.
- **A command finding its view**: dispatch resolves the target (`gui::resolveCommandTarget()`: the current document's view → its document) and carries it with the execution; there is no window-level `primaryRenderControl()` and no publish slot.
  A window shows one view at a time (tabs), and several 3D areas side by side are expressed by the view's panes.
- Per-instance attributes and GPU indirect are not in the render-view layer, see `../backlog/graphics.md`.

### 9.3 Compatibility / migration

The names are `PassChain` / `ChainDesc`, not `Pipeline` / `PipelineOptions`.
There is no `RenderControl::present()`, no public `SceneView::removeWindowPass()`, no `setInputRouter` and no `focusedView`.
New code uses the interface of §4 only.

## 10. References

### 10.1 Code

- `src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/Viewer.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ViewerOptions.hpp`
- `src/viz/graphics/sdk/vine/graphics/SceneView.hpp`
- `src/viz/graphics/sdk/vine/graphics/PassChain.hpp`
- `src/viz/graphics/sdk/vine/graphics/RenderArea.hpp`

### 10.2 Documents and ADRs

- `appfw-render-surface.md`
- `appfw-startup-splash.md`
- `graphics-architecture.md`
- `../backlog/graphics.md`

### 10.3 External material

- Host usage: `docs/guides/render-views.md`
- The reference picking implementation: `src/plugins/model_viewer/src/ModelRenderView.hpp`
