# graphics architecture: device, engine, chain, one frame

> **Status**: current
> **Last verified**: 2026-11-16
> **Scope**: the layering of device/window/area/view/engine, and one frame's compilation and scheduling.
> **Sources**: `RenderEngine.hpp` / `PassChain.hpp` / `PassRegistry.hpp` / `OutputRegistry.hpp` /
> `DiagnosticsRoute.hpp` / `SceneView.hpp` / `RenderArea.hpp` / `RenderBackend.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

This file defines the current design of the graphics architecture: device, engine, chain, one frame: first the objects and their boundaries, then features, interfaces, constraints and acceptance.

### 1.2 Goals

- Turn §2's design points into §3's features, §4's interfaces and §8's acceptance.
- Keep "what it is now" separate from "why it was decided that way": the reasons are in §2/§7, history is in git.

### 1.3 Non-goals

- This file does not repeat the full signatures from the headers; an interface gives the contract and a pointer only.
- This file keeps no closed history or per-round review records.
- A new capability gets a design first, then a backlog entry, then a signature (`G8`); the migration window of "add only, change nothing" closed with 5b-2c.

### 1.4 One-paragraph conclusion

> A device is not a window; the engine owns the lifetime and one frame; `PassChain` is one picture's recipe and registration home;  scheduling happens in one place, the engine, and the backend only materialises.

## 2. Design Points

2.1 **Mechanisms go into the SDK, composition belongs to the application layer**: split-screen layout, several transients borrowing depth, and the boundaries of convenience objects are not made engine policy.
2.2 **Declaration and execution separated**: a pass is a declaration, the frame compiles into values (`FrameDescription` / `FrameTable`), the backend only consumes values.
2.3 **One fact, one home**: pass registration is only `PassRegistry`; named outputs only `OutputRegistry`; diagnostics only
    `DiagnosticsRoute`; presentation only `RenderArea`'s present table.
2.4 **A device is not a window**: `RenderDevice` owns the GPU device and queues and can serve several windows; window targets are issued by the device.
2.5 **There is one scheduler implementation**: the engine's `detail::FrameScheduler` sorts; the backend no longer reorders by dependency and no longer reports cycles.

2.6 **Content belongs to the pass's declaration**: `PassDesc::content` is the only storage; the engine's slot holds only order / owner / role / transient / timing.
Why: a pass is one declaration (`RenderPass.hpp`'s "ONE FACT, ONE STORAGE"), the content is written by the declarer and read by the compiler; a second copy in the engine
would immediately become two writable values. Boundary: the same pass instance registered with two engines shares its content — a direct consequence of "one declaration"; for two pictures use two passes.
`applyPlan(plan, owner)` only writes declarations; clearing uses `bindPassContent(pass, nullptr)` (or the plan no longer names that row).
It satisfies `G1` / `G4`; for `G1` it gives up "one instance, several contents".

2.7 **A change that can fail is visible from the return value**: `applyPlan` returns the three-state `PlanResult` (`Applied` / `Invalid` + problems /
`Refused` + `PlanRefusal::Busy`), `bindPassContent` / `PassRegistry::bindContent` return whether a registered pass was hit,
`frame()`'s `FrameOutcome` answers only Ran / Submitted (`NotReady` + `EngineNotReady` is a precondition, not a frame ending),
`prepare()` reuses the same `EngineNotReady`.
Why: a silent no-op lets the host take "did not take effect" for "took effect"; the return value is the only answer the call site can judge by. Boundary: `removePass` stays idempotent and returns nothing —
removing an unregistered pass and removing an already removed pass are the same result. It satisfies `G4` / `G6`; the price is one ignorable return value per call site.

2.8 **`RenderEngine` is the composition root, not a home for features**: it owns and composes three mechanisms — the frame pump (`detail::FramePump`: frame number, one frame in flight,
    `waitForFrame`, `presentRecord`), the session shape (`detail::SessionShape`: pass declarations, `plan` / `applyPlan`, frame compilation and wiring checks),
    the surface ledger (`detail::SurfaceLedger`: the one area, announced size, window target) — and keeps only composition and lifetime (`initialize` /
    `shutdown` / `device` / `createArea` and the forwarding of reports). Why: these mechanisms each have their own state and failure modes; inside the composition root,
    changing the threading policy touched 9 files and adding one wiring check meant a paragraph in a 1500-line header (`G5` / `G12`). Boundary: a new **mechanism** belongs to `detail::`,
    not to `RenderEngine`; `FramePump` only answers "where does the other half run, when does it answer", `SessionShape` only "what does this frame run, in what order,
    where is it wired wrong", `SurfaceLedger` only "which target does this frame land on, what is the size" — each has a consumer today (two threading modes, two backends'
    compilation comparison, one engine one surface), none is a pre-built abstraction (`G8`).
    One terminology discipline comes with it: `PlanRefusal::Busy` means "**a frame is recording in the engine**"; the SDK has **no** `Session` type today,
    and needs none: **one engine drives one surface** ([ADR-2026-11-13](ADR-2026-11-13-one-engine-one-surface.md)), `Session` only names the execution side of one engine as a design
    term. Documentation and new interfaces no longer refer to a type that does not exist.

2.10 **The migration APIs are closed**: the `addPass` family is gone, `Plan` is the only shape of a pass declaration;
    `applyPlan` / `plan` / `bindSnapshot` / `prepare` are host contracts rather than migration entries. `FrameTable` is the compiled frame table (not the
    plan), `frame()` answers only Ran / Submitted, `applyPlan` is three-state, `SessionBusy` is no longer a plan problem code.
    Reason: changing during migration in two passes would leave three shapes alive; now there is one shape and the public signatures match the documentation (`G1` / `G6`).

2.9 **Shared mechanisms live in the same directory and namespace as the host SDK**: in `sdk/vine/graphics/` besides the host types there are 8 mechanisms "both backends use"
    — `FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState` (formerly `Keys`),
    `TargetPlan`, `DiagnosticDraft`. They once had their own directory, turned "the host includes it" into a compile error through an include guard, and were installed as
    a separate component; that whole "backend half" has been deleted (
    [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md)). Reason: the directory, the guard, the install component and the gate maintained the same boundary in four places, at a cost
    above the errors it prevented; and the property worth keeping is the **dependency direction**, not a path.
    Boundary: two things the gates still pin are — the graphics module (`sdk/vine/graphics` and `src/`) must not include a render API or any concrete backend
    (`check_include_hygiene.py`, `check_vk_exposure.py` V2), and a shared mechanism's exported symbols must carry `VN_GRAPHICS_API`
    (`check_export_annotations.py`'s file list). **The seam's definition is still "both backends use it"**: the 17 headers and 16
    `.cpp` files only vsg reads moved into `gfx_backend_vsg/internal/` in ADR-2026-11-16 §2, vsg's own types live in `vn::backend_vsg::core`
    (`G13`). The price, honestly: the host can now include these mechanism headers and they are visible in the SDK's namespace (a residue of `G7`); what it buys is
    a rule that does not need syncing in four places. `DrawState` / `TargetPlan`'s device format codes are now the **opaque**
    `DeviceFormat` (0 = the device's UNDEFINED), the render-pass structure is carried by the backend-computed `RenderPassKey` — neither
    `VkFormat` values nor subpass are on the seam, so a third backend need not learn Vulkan's formats and
    subpass first; `ImageLayout` is still Vulkan semantics inside the SDK, C6's "no new spellings" still applies, and retiring it belongs to
    the same gate's next step.
2.11 **An extension point's cost is a table, not an adjective** (audit `A12`): saying "easy to extend" without saying "which places a new one touches" cannot be falsified.
    So every extension point has a row here, listing **today's actual files** — read this table before adding something, update it afterwards.
    Criterion: if an extension point starts needing changes to the **core model** (`FrameDescription`'s shape, the engine's compilation, scheduling), it is not an extension
    but a refactor, and that step needs an ADR (the reverse of `G9` is exactly "every addition touches the core").

| Extension point | Where it touches today (actual files) | Places |
|---|---|---|
| A **pass kind** (e.g. compute) | `PassDesc.hpp`'s `PassKind`; `Plan.cpp`'s validation; `RenderPass.cpp`'s kind → node mapping; a new node class (following `ScreenPass.cpp`); `EngineCompile.cpp`'s describe; each backend recording by `DrawDescription::Kind` | 6–8 |
| A **pixel format** | `RenderTarget.hpp`'s `ColorFormat` / `DepthFormat`; vk `TargetObjects.cpp`'s `colorFormatOf` / `depthFormatOf`; vk `ContentImages.cpp`'s `formatOf`; vsg `VsgSceneRules.cpp`'s format mapping | 4 |
| A **pass declaration field** | `PassDesc.hpp`; `Plan.cpp` (diff and validation); `FrameDescription.hpp`'s `PassDraw`; each backend's recorder | 4+ |
| A **content channel** (vertex attribute) | `Geometry.hpp`'s `GeometryDescription` channel table; `ShaderAbi.hpp`'s block layout; vk `ContentDraw.cpp`'s channel → location mapping and vertex format; vsg's binding | 4 |
| A **backend** | a new plugin (`Plugin.cpp` + `BackendFactory.cpp`), the two halves of `RenderBackend`, its own `test_*`, a `check_*_exposure` gate, `RenderBackendRegistry` self-registration | about 6 kinds of file |

2.12 **`Content*` names only the content model and identity** (naming, `G12`): the `Content*` prefix belongs to **content objects themselves** — `ContentId` /
    `ContentRef` / `ContentRegistry` / `ContentDeclarations` / `ContentStore` (the device's "identity + row" table). GPU objects compiled or cached from content are named by **what they are**, without the `Content` prefix: `ProgramHalves` (the compiled half of each
    (program, revision, layout, variant)), `BindingSets` (the descriptor sets built from declarations),
    `PushRange` (fills the members of one declared push range by name).
    Why: a prefix used on "descriptor set", "push constants" and "compiled half" stops saying anything (`G12`), and `ContentPush` reads
    like a content object while it is actually the fill rule for push bytes. Boundary: the other `Content*` names (`ContentStore` / `ContentCaches` /
    `ContentDraw` / `ContentImages` / `ContentPipeline` / `ContentAssembly` / `ContentPass` / `ContentFacts` /
    `ContentSources`) **stay** — they at least say they handle content (renaming item by item is churn, `G8`); new names follow this rule.
    Spelling deviation: the research document's `E-08` suggested `PushConstants`, not adopted — the same TUs already have `::vsg::PushConstants`
    (the object actually recorded), and two identical names would create a "whose is this" question at every use site (`G1`); `PushRange` says the fact this unit owns
    (one declared range and its named members).

    The names of same-role types in the two backends (alignment is the rule, a deviation must state its reason):

| Role | vk | vsg |
|---|---|---|
| Facade (= `RenderBackend`) | `Device` | `VsgBackend` |
| Frame driver (one frame's machinery and scratch) | `Device::FrameDriver` (lent by `createSession`) | `VsgDriver` (lent by `createSession`) |
| The value plan resolved at frame head | `FrameHead` | `FrameHead` |
| The device's target table / target objects | `TargetTable` / `TargetObjects` | `TargetTable` / `TargetObjects` |
| Window target | `WindowTarget` | `WindowTarget` |
| Window / offscreen session | no separate type (the device's own frame machinery) | `WindowSession` / `OffscreenSession` |
| Content facts and rows | `ContentCaches` (rows and command plans included) | `ContentStore` + `ContentFacts` |
| Compiled half (per (program, rev, layout, variant)) | no separate type (the pipeline key in `ContentDraw`) | `ProgramHalves` |
| Declared descriptor sets | `ContentCaches`'s set cache | `BindingSets` |
| Readback | `ReadbackStaging` | `HostReadback` |
| Lights | SDK `LightSet` | SDK `LightSet` |

    The `Content*` names that stay differ between the two sides (`ContentCaches` / `ContentStore` / `ContentFacts`) and that is not drift: they all belong to
    "the content model and identity" (2.12's first sentence), and one backend keeps rows and command plans in one object while the other separates them —
    the name says that.

## 3. Features

### 3.1 Device, window, area, view, engine

| # | Feature | Notes |
|---|---|---|
| F1 | Device | `RenderDevice` (`RenderBackend`): device + queues; owns no window. |
| F2 | Window target | `WindowPresentTarget`: window handle + swapchain; one target per window. |
| F3 | Area | `RenderArea`: size + present table; created by `RenderEngine::createArea()`; one area, one picture. |
| F4 | View | `SceneView`: camera + scene + manipulator + input + the adopted `PassChain`; answers "which pass draws me" (input ownership is in `appfw-render-view.md` §2.1 / §2.4). |
| F5 | Engine | `RenderEngine`: `initialize` / `shutdown` / `frame() → FrameOutcome`, device borrowing, size, capabilities, reports. |

### 3.2 The engine's three facades

| # | Feature | Notes |
|---|---|---|
| F6 | `PassRegistry` | The only pass registration table; `add` / `remove` / `count` / `orderOf` / `contentOf` / `bindContent(...) → bool`. |
| F7 | `OutputRegistry` | Named outputs and transients: `publish` / `resolve` / `unpublish` / `transientTarget`. |
| F8 | `DiagnosticsRoute` | Diagnostics: `setSink` / counts / `isClean`; the engine dies first ⇒ `isValid()` false. |

### 3.3 Chain (recipe + registration home)

| # | Feature | Notes |
|---|---|---|
| F9 | Building a chain | `PassChain::build(engine, ChainDesc)`: builds the passes, registers them, returns a handle. |
| F10 | Unregistration | Chain destruction hands the registered passes back to the engine; the engine dies first ⇒ the handle is invalid and the chain idles. |
| F11 | Picture facts | `draws(scene)` / `presentingPass()` / `offscreenTarget()` / `compositeTarget()`. |
| F12 | Runtime switches | `setShadingMode` / `setGizmoVisible` / `setStatsVisible` / `resize`. |
| F13 | overlay | role + content + pass; `overlayPass(role)` / `overlayContent(role)`; a third overlay changes no class. |

### 3.4 One frame

| # | Step | Notes |
|---|---|---|
| F14 | Compilation | `compileFrame()`: resolve inputs → build edges → stable topological sort → allocate transients → `FrameDescription`. |
| F15 | Recording | `beginFrame` → per pass (`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`) → `endFrame`. |
| F16 | Materialisation | The backend builds targets/subpass/barrier/clear from `FrameDescription.passes`, records commands, reports what it cannot serve. |
| F17 | Presentation | `RenderArea`'s present table resolves to a concrete pass; two layers, `PresentTarget` / `WindowPresentTarget`. |

### 3.5 Presentation and multiple windows

| # | Feature | Notes |
|---|---|---|
| F18 | Declarative presentation | `RenderArea::present(view)` / `present(pass)`; who draws this surface is answered once by the view/surface. |
| F19 | Multiple windows | **One device can serve several engines** (one engine / one area / one `Viewer` per window); destroying a window does not take the other windows with it. One engine driving several windows is rejected (one frame one surface, see §7 D2 and ADR-2026-11-13). |
| F20 | Size | The single owner of the surface size is `RenderArea`; views follow through `resize` / `addSurfaceLayout`. |

## 4. Public Interface

### 4.1 `RenderEngine`

| Interface | Purpose |
|---|---|
| `initialize()` / `shutdown()` | Lifetime |
| `frame()` | Compile and execute one frame; returns `FrameOutcome` (Ran / Submitted; `NotReady` + `EngineNotReady`) |
| `passes()` / `outputs()` / `diagnostics()` | The three facades |
| `device()` / `setDevice()` | Borrow/install a device |
| `createArea()` | Create the one area |
| `applyPlan()` / `applyPlan(plan, owner)` / `plan()` / `planFor(owner)` | The host's and a contributor's shape (`PlanResult` three states; the `addPass` family is deleted) |
| `publish()` / `resolve()` / `unpublish()` | Named outputs (equivalent to `outputs()`) |
| `transientTarget()` | Transient target |
| `releaseContent()` | Release content's GPU residency |
| `frameTable()` / `frameDescription()` | This frame's compiled products |
| `areaWidth()` / `areaHeight()` / `capabilities()` / `frameReport()` | Queries |

### 4.2 Registries and diagnostics

| Interface | Purpose |
|---|---|
| `PassRegistry::add(PassDesc)` / `remove()` / `count()` | Pass registration |
| `PassRegistry::orderOf()` / `contentOf()` / `bindContent()` | A pass's order / content; `bindContent` returns whether a registered pass was hit |
| `OutputRegistry::publish()` / `resolve()` / `unpublish()` | Named outputs |
| `OutputRegistry::transientTarget()` / `transientCount()` | Transients |
| `DiagnosticsRoute::setSink()` / `count()` / `isClean()` | Diagnostics |

### 4.3 `PassChain`

| Interface | Purpose |
|---|---|
| `static build(engine, ChainDesc)` | Build and register a chain |
| `isValid()` | Whether the engine is still alive |
| `draws(scene)` | Whether this chain draws that scene |
| `presentingPass()` | The pass that presents to the window |
| `offscreenTarget()` / `compositeTarget()` | Offscreen targets |
| `resize(w, h, ratio)` | Target and overlay layout |
| `setShadingMode()` / `shadingMode()` / `shadingPath()` | Shading mode |
| `setGizmoVisible()` / `setStatsVisible()` | Overlay switches |
| `overlayPass(role)` / `overlayContent(role)` / `setOverlayVisible(role, on)` | Overlay queries/switches |

### 4.4 `SceneView` / `RenderArea`

| Interface | Purpose |
|---|---|
| `SceneView::setEngine()` / `setScene()` / `setCamera()` | Binding |
| `SceneView::setChain()` / `chain()` | Adopt the recipe |
| `SceneView::presentingPass()` | Forward the surface's question |
| `SceneView::resize()` / `addSurfaceLayout()` | Follow the size |
| `RenderArea::present(view)` / `present(pass)` | Declare presentation |
| `RenderArea::resize(w, h, ratio)` | The single definition of the surface size |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | A device is not a window: `RenderDevice` owns the device/queues, window targets are issued by the device. |
| C2 | Pass registration, named outputs, diagnostics and presentation each have one owner, no second list. |
| C3 | Scheduling is in the engine only; the backend only materialises `FrameDescription.passes`. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | A backend must not reorder passes, build its own dependency graph or report cycles. |
| R2 | `PassChain` only borrows the engine and does not extend its lifetime; the engine dies first ⇒ `isValid()` false. |
| R3 | One area, one picture; multi-cell layout belongs to the application layer, no second area in the engine. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | `PassChain` only borrows the engine (`PassRegistry` / `DiagnosticsRoute` are leases) and does not own it; the engine dies first ⇒ `isValid()` false. |
| N2 | The chain's passes are registered with the engine; the chain keeps no second pass list. |
| N3 | `PassDesc::camera` is an owning `intrusive_ptr<Camera>`; do not pass the address of a stack camera. |
| N4 | One area, one picture; multi-cell layout belongs to the application layer, do not add a second area to the engine. |
| N5 | `Camera` carries its own aspect; a pass rectangle inconsistent with the camera shape is reported at compile time. |
| N6 | The backend materialises `FrameDescription.passes` only; do not reorder or build a dependency graph on the recording path. |
| N7 | `frame()`'s `FrameOutcome` only says whether it ran (Ran / Submitted) or the engine was not up (NotReady + reason); if it ran but drew nothing, read `frameReport()`, do not infer the drawing result from the return value. |

## 6. Caveats and Traps

### 6.1 One fact, one home

- How it fails: the same state is kept in two places, change one and the other goes stale.
- How to detect it: two writable values / two writable registries appear.
- How to prevent it: go back to §5's N#; keep one owner, everything else read-only.

### 6.2 Backend/platform boundary

- How it fails: platform differences are written into the engine/application layer, and every platform change forks everywhere.
- How to detect it: an "if Windows/X11 then …" branch appears.
- How to prevent it: platform facts stay in the backend, the engine consumes only compiled frame values.

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Built-in multi-cell split-screen layout in the engine | Composition belongs to the application layer; the SDK guarantees only the three mechanisms of rectangle, input and size fan-out. |
| D2 | One frame across several windows / one engine driving several areas | Only "one picture across two screens" would need it; today one window, one picture. Multiple windows = one device serving several engines (see [ADR-2026-11-13](ADR-2026-11-13-one-engine-one-surface.md), which also rejects graphics-next's F8). |
| D3 | `PassPipeline` / `ScenePicture` naming | Reopens K1's PSO ambiguity / reads as "scene picture"; the current name is `PassChain`. |
| D4 | One engine doing the pass table, the output table, diagnostics and the UI facade | One type, several jobs; split into three narrow facades. |
| D5 | The backend sorting the passes again | Two sortings, two frame vocabularies; scheduling moved up into the engine. See [ADR](../decisions/ADR-2026-10-22-engine-schedules-passes.md). |
| D6 | A `FrameBorrowed<T>` marker type on `FrameDescription` | The answer to borrowing is already "own it if you keep it" (`OwnedFrame`); a borrowed window is only one `executeFrame` call wide, and a marker type would pierce `FrameDescription` / `PassDraw` / `ImageSource` and could be bypassed by `auto` (`G8` / `G10`). |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `test_graphics` / `test_gui` / `test_core` all green. |
| T2 | `check_ai_docs.py --strict` / `check_doc_symbols.py` / `check_public_docs.py` 0 findings. |
| T3 | `scripts/vsg_rewrite_gate.sh`: 0 VUID / 0 SYNC-HAZARD, phase lines closed, application picture percentage. |

## 9. Status and Evolution

### 9.1 Current status

Current design; the feature points and interfaces are the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition.

### 9.3 Compatibility / migration

Old names/paths appear only in §7's rejected options or in git history; new code follows §4's interfaces.

## 10. References

`graphics-scene.md` · `graphics-geometry.md` · `graphics-rendering.md` ·
`graphics-content-lifecycle.md` · `vsg-backend.md` · `../backlog/graphics.md`
