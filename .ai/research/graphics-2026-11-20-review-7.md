# graphics review 7: implementation quality (duplication, dead noise, comment truth)

> **Status**: draft, non-authoritative - a working record. The changes it describes are in the tree; the
> items in §3 and §4 are proposals, not decisions.
> **Last verified**: 2026-11-20
> **Scope**: implementation-level quality of `src/viz/graphics` (34,299 lines, 204 files): duplicated
> logic, needless syntax, comment/code contradictions, and abstraction opportunities. It deliberately does
> NOT re-walk architecture, layering, threading, naming or the incremental edit path - reviews 4, 5 and 6
> own those.
> **Sources**: the tree at `453c63b8` plus this round's diff; `src/viz/graphics/**`, `tests/test_graphics`,
> `tests/test_gui`, `tests/test_vsg`.
> **Backlog**: `../backlog/graphics.md` - this round added no entry (see §6).

## 1. What this round is

The earlier rounds concluded clean on header contracts, layering, seam identity, public vocabulary,
allocation and the edit path. This round asked three narrower questions:

1. Is the same logic written twice?
2. Is there syntax that says nothing (an unused include, an empty namespace, a temporary that only
   renames an expression)?
3. Does every comment describe the code under it?

Answers: (1) once, seriously - a byte-identical 38-line box builder; (2) a handful; (3) nine sentences
that had drifted from the code. The batch below fixes exactly those, because each one is a fact with one
wrong home or two homes, not a matter of taste (`G1` / `G4` / `G6` / `G12`).

## 2. Implemented in this round

| # | Change | Files | Why | `G#` |
|---|---|---|---|---|
| 1 | One `detail::appendBox` for the HUD overlays | new `src/OverlayMesh.hpp`; `AxisGizmo.cpp`, `FpsOverlay.cpp` | The two copies were byte-identical except one trailing period in a comment, and FpsOverlay's own comment said it "mirrors the box builder used by AxisGizmo". A winding fix could land in one only. | `G1`, `G5`, `G6` |
| 2 | `take` / `takeFloor` in `StateResolution.cpp` | `StateResolution.cpp` | The eleven items of the state priority table each spelled "if authored, assign, stamp origin"; one helper per item makes the precedence read as the table it is. No behaviour change (the two floors stay explicit at the call). | `G4`, `G12` |
| 3 | `Geometry::updatePrimitive` private template | `Geometry.hpp`, `Geometry.cpp` | The four per-primitive mutators each repeated the range guard, the ignored-edit counter and the copy-modify-store frame. A future mutator can no longer forget the counter. `setInstanceTransforms` keeps its extra announcement. | `G1`, `G12` |
| 4 | `retakeDescription` in `DrawListBake.cpp` | `DrawListBake.cpp` | Six copies of "re-take a live object's description, compare, store"; the resolved side still compares BEFORE the copy `storeResolved` needs, so a steady row pays no extra copy. | `G1`, `G12` |
| 5 | `ChainPresets.cpp` uses `Aabbd::isValid()` and one `lightBasis()` | `ChainPresets.cpp` | The validity test was hand-spelled twice while the helper already existed; the direction/up basis of the two shadow cameras was copied, and two cameras disagreeing about `up` mirror the shadow map. | `G1`, `G12` |
| 6 | `ChainBuilder` asks `Light::hasDirection()` | `ChainBuilder.cpp` | "directional or spot" is already a named fact on `Light`; the two-enum test was re-spelled twice. | `G1`, `G12` |
| 7 | Unused includes, empty namespace, shadowed forward declaration | `Scene.cpp`, `SceneView.cpp`, `SceneView.hpp` | No symbol from those headers is used; `SceneView.cpp` declared an empty anonymous namespace; `class PassChain;` sat beside `#include "PassChain.hpp"`. | `G11` |
| 8 | Nine comment corrections | `Light.hpp`, `RenderTarget.hpp`, `RenderArea.hpp`, `Camera.hpp`, `Scene.hpp` | Each sentence stated the opposite of the adjacent code: spot shadows exist; `ShadowSettings` has no filter; the member is `setDepthPromotion`; readback is `RenderDevice`'s; `resize` does not resize the window half; `up` is stored normalised; the "two halves" note is gone. | `G4`, `G11` |
| 9 | Drop a temporary that renamed an expression; range-for a slot list | `Plan.cpp`, `EngineCompile.cpp` | Local noise only; the loop index was never read. | `G11` |

Net: one internal header added, ~150 lines of duplicated/empty code removed, no public signature and no
behaviour changed.

## 3. Verified, intentionally left alone

| Item | Where | Why not now |
|---|---|---|
| `Camera::screenToWorldRay` and `CameraSnapshot::screenRay` implement the same question twice | `Camera.cpp`, `Camera.cpp` (`CameraSnapshot::screenRay`) | Not mechanical: for an ORTHOGRAPHIC camera the snapshot starts the ray where the pixel crosses the NEAR plane, the live camera starts it on the plane through the EYE. Those are the same line but not the same origin, and a picker that reads `t` sees the difference. Unifying needs a decision about which origin is the contract (and the existing equality test then becomes tautological). |
| Public API with zero tree consumers: `Scene::Edit`, `ScenePublishResult::operator bool`, `SceneView::presentedCamera()`, the const `SceneView::inputRouter()` overload | `Scene.hpp`, `SceneView.hpp` | `G8` says they do not earn a place, but deleting public surface is an API batch with its own compatibility note, not a quality sweep. `Scene::Edit` is also documented as the scoped batch spelling, so it is the first candidate for an owner decision. |
| `RenderEngine`'s flat `pass*` / `publish` / `resolve` shorthands duplicate `PassRegistry` / `OutputRegistry` | `RenderEngine.hpp` | Same reason: public surface. The handles already exist, so the flat half is removable in one documented batch. |
| `Geometry`'s `hasXxx()` / `xxxCount()` families | `Geometry.cpp` | A private `countAt(location)` would save ~12 lines but lose the names, which are the documentation; the bodies are already one statement over `buffer(location)`. |
| `AutoTransform` repeats `inverted ? M * p : p` for the eye and the up vector | `AutoTransform.cpp` | The two branches are a POINT transform (translation applies) and a DIRECTION transform (it does not); one uniform lambda would hide that distinction. |
| `ChainBuilder::applyOverlays` builds two similar HUD `PassDesc`s | `ChainBuilder.cpp` | The differences are the options that name the overlay; a helper would take six parameters to save ~15 lines. |
| `Scene::lightSet`'s manual key comparison | `Scene.cpp` | `std::equal` + a key lambda is not clearly more readable than the six-line short-circuit loop, and it is not wrong today. |

## 4. Larger candidates (not done this round)

| # | Candidate | Size today | Shape | Risk |
|---|---|---|---|---|
| 1 | `SessionShape::collectPlanProblems` | 412 lines, four comment-numbered phases + nine capturing lambdas | A `WiringReport` (four vectors + the six lookups) and four methods: fill/claim, inputs-produced, screen-pass, unbound-images | Lowest of the four: diagnostic-only code, phases already delimited; the diagnostic tests are the oracle |
| 2 | `ScenePublisher::ensureRows` | 151 lines, nesting depth 7 | `resetToEmpty` / `rowsStillMatchTheGraph` / `refreshChangedRows` / `rebuildRows` | Do AFTER review 6's regression test: review-6 §3 shows the incremental path has no independent oracle, so a structural change now could hide P0-1/P0-2 rather than expose them |
| 3 | `Plan::validatePlan` | 111 lines | Declaration checks -> producer edges -> cycle report, three scopes | Low |
| 4 | `RenderEngine::buildContentHalf` | 101 lines | `takeFrameExtent` / `capturePresentedView` / `capturePresentedSnapshot` / `announceContentFrame` | Low |
| 5 | `detail::SessionShape` holds five jobs (slot table, plan diff, compile, wiring validation, diagnostic identity) | ~35 members, `SessionShape.cpp` + `EngineCompile.cpp` | Split the compile job into its own `detail` class | Design change, needs an ADR (`G5` / `G8`), not a refactor |
| 6 | `PassChain`'s four gizmo/stats visibility methods | 4 methods | Two role-based accessors (`overlayVisible(role)` / `setOverlayVisible(role, ...)`) | Public surface change |
| 7 | `RenderEngine` carries 72 members across eight jobs | `RenderEngine.hpp` | Mostly composition by design; the actionable part is §3's flat shorthands | API decision |

## 5. Verification

| Check | Result |
|---|---|
| `cmake --build build --target Graphics` (MSVC Debug) | clean |
| `cmake --build build --target test_graphics` | clean |
| `ctest -R test_graphics` | passed |
| `ctest -R test_gui` | 2 failures - **identical at the stashed baseline**: `RenderControlTest.ContinuousRenderingDrivesFramesWithoutInput` (one frame presented after the stop; a timing race) and `PluginLifecycleTest.RescanFindsANewRegistrationWithoutALoad` (plugin manager behavior; no graphics code involved) |
| `ctest -R test_vsg` | 525 passed, 3 skipped, 1 failure - `CorePhaseTableTest.AFramesPhaseGatesOnTheCountersAndOnTheHeap` ("two steady frames allocate nothing") - **also identical at the stashed baseline**; it exercises `vn::backend_vsg::core` only, and its own note says the numbers were measured on another platform |

The baseline comparison used `git stash push -- src/viz/graphics`, a rebuild of `test_vsg`, the same
tree-level runs, and `git stash pop`. Both remaining failures are therefore pre-existing Windows Debug
observations, not effects of this batch.

## 6. Next step

Take §4's candidate 1 (`collectPlanProblems`) as its own batch: it is the module's longest function,
its phases are already named in comments, and nothing outside the diagnostics path changes. Candidate 2
waits for review 6's regression test. The §3 API deletions need an owner decision; they are not quality
sweeps.
