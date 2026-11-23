# graphics architecture: device, engine, chain, one frame

> **Status**: current
> **Last verified**: 2026-11-23
> **Scope**: the layering of device/window/area/view/engine, and one frame's compilation and scheduling.
> **Sources**: `RenderEngine.hpp` / `PassChain.hpp` / `PassRegistry.hpp` / `OutputRegistry.hpp` /
> `DiagnosticsHandle.hpp` / `SceneView.hpp` / `RenderArea.hpp` / `RenderBackend.hpp`.
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
    `DiagnosticsHandle`; presentation only `RenderArea`'s present table.
2.4 **A device is not a window**: `RenderDevice` owns the GPU device and queues and can serve several windows; window targets are issued by the device.
2.5 **There is one scheduler implementation**: the engine's `detail::FrameScheduler` sorts; the backend no longer reorders by dependency and no longer reports cycles.

2.6 **Content belongs to the pass's declaration**: `PassDesc::content` is the only storage; the engine's slot holds only order / owner / role / transient / timing.
Why: a pass is one declaration (`RenderPass.hpp`'s "ONE FACT, ONE STORAGE"), the content is written by the declarer and read by the compiler; a second copy in the engine
would immediately become two writable values. Boundary: the same pass instance registered with two engines shares its content — a direct consequence of "one declaration"; for two pictures use two passes.
`applyPlan(plan, owner)` only writes declarations; clearing uses `bindPassContent(pass, nullptr)` (or the plan no longer names that row).
It satisfies `G1` / `G4`; for `G1` it gives up "one instance, several contents".

2.7 **A change that can fail is visible from the return value**: `applyPlan` returns the three-state `PlanResult` (`Applied` / `Invalid` + problems /
`Refused` + `PlanRefusal::Busy`), `bindPassContent` / `PassRegistry::bindContent` return whether a registered pass was hit,
`frame()`'s `FrameCallOutcome` answers only Ran / Submitted (`NotReady` + `EngineNotReadyReason` is a precondition, not a frame ending),
`prepare()` reuses the same `EngineNotReadyReason`.
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

2.9 **Shared mechanisms live in the same directory and namespace as the host SDK**: in `sdk/vine/graphics/` besides the host types there are 8 mechanisms, each the family-neutral implementation of a promise a host can observe ([ADR-2026-11-23](ADR-2026-11-23-retiring-the-reference-backend.md)):
    `FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState` (formerly `Keys`),
    `TargetPlan`, `DiagnosticDraft`. They once had their own directory, turned "the host includes it" into a compile error through an include guard, and were installed as
    a separate component; that whole "backend half" has been deleted (
    [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md)). Reason: the directory, the guard, the install component and the gate maintained the same boundary in four places, at a cost
    above the errors it prevented; and the property worth keeping is the **dependency direction**, not a path.
    Boundary: two things the gates still pin are — the graphics module (`sdk/vine/graphics` and `src/`) must not include a render API or any concrete backend
    (`check_include_hygiene.py`, `check_vk_exposure.py` V2), and a shared mechanism's exported symbols must carry `VN_GRAPHICS_API`
    (`check_export_annotations.py`'s file list). **The seam's membership rule is the PROMISE, not the reader count**
    ([ADR-2026-11-23](ADR-2026-11-23-retiring-the-reference-backend.md)): a mechanism belongs here when a sentence a host
    can read is true only because it is ONE implementation, and it moves into a plugin when its only justification is
    "two backends happen to call it". "Both backends use it" was the EVIDENCE while two backends existed, and
    `check_include_hygiene.py` keeps measuring it until the deletion re-pins the list to "reached by the shipped
    backend" (`../backlog/graphics.md` T60); the rule itself survives the deletion and a second owner, because a promise
    enforced twice is two answers to one host (`G6`).

| Mechanism | The promise a host reads |
|---|---|
| `FrameGate` | One frame at a time per executor; a re-entrant host verb is REFUSED, not deadlocked (`RenderDevice.hpp`). |
| `DeviceClock` | A frame reported complete is provably complete, and its slot is safe to record into again (`FrameProgress`). |
| `FrameTimeline` | The same watermark, N frames wide, never going backwards (`FrameProgress`). |
| `RetirementQueue` | A release is SCHEDULED, not synchronous, and the frame path does not idle the device (`RetentionStats::device_waits`). |
| `Readback` | A refusal is classified before any wait: `Unsupported` ("never") versus `NotReady` ("not yet"). |
| `DiagnosticDraft` | One refusal shape and one stable code identity per module (`RenderDiagnostic`). |
| `DrawState` | Identity is what a pipeline is keyed by; a per-draw value never recompiles (`PipelineKey`, `RenderState.hpp`). |
| `TargetPlan` | A resize does not recompile a pipeline, and a shape change does (`PrepareReport::pipelines`). |

    The 17 headers and 16 `.cpp` files only vsg reads moved into `gfx_backend_vsg/internal/` in ADR-2026-11-16 §2, vsg's own types live in `vn::backend_vsg::core`
    (`G13`). The price, honestly: the host can now include these mechanism headers and they are visible in the SDK's namespace (a residue of `G7`); what it buys is
    a rule that does not need syncing in four places. `DrawState` / `TargetPlan`'s device format codes are now the **opaque**
    `DeviceFormat` (0 = the device's UNDEFINED), the render-pass structure is carried by the backend-computed `RenderPassKey` — neither
    `VkFormat` values nor subpass are on the seam, so a third backend need not learn Vulkan's formats and
    subpass first. As of 2026-11-23 the last one is gone too: `ImageLayout` and `LoadOpVariantKey` left the
    SDK for the backends that build pass objects (each backend owns its own; T15 closed - review 8 §4.7/§4.8),
    so the seam carries no Vulkan SEMANTICS at all, only the neutral vocabulary. C6's "no new spellings"
    stays the rule and `check_vk_exposure.py` V2 is its gate.
    **The seam has no namespace of its own**
    ([ADR-2026-11-18](ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md)): it is the property "both backends
    reach it", recorded once in `check_include_hygiene.py`'s `SEAM_HEADERS`, and the two pages that state it (this point and
    `docs/guides/graphics-backends.md` 5) are checked against that list, so a mechanism cannot join or leave it without them
    changing too. An alias group (`vn::graphics::backend`) was rejected: it gives one class two names, and the shared surface
    is 25 headers, not the 8 mechanisms - a name covering 8 of 25 claims a boundary that is not there (`G1`, `G4`).
2.10 **The migration APIs are closed**: the `addPass` family is gone, `Plan` is the only shape of a pass declaration;
    `applyPlan` / `plan` / `bindSnapshot` / `prepare` are host contracts rather than migration entries. `FrameTable` is the compiled frame table (not the
    plan), `frame()` answers only Ran / Submitted, `applyPlan` is three-state, `SessionBusy` is no longer a plan problem code.
    Reason: changing during migration in two passes would leave three shapes alive; now there is one shape and the public signatures match the documentation (`G1` / `G6`).
2.11 **An extension point's cost is a table, not an adjective** (audit `A12`): saying "easy to extend" without saying "which places a new one touches" cannot be falsified.
    So every extension point has a row here, listing **today's actual files** — read this table before adding something, update it afterwards.
    Criterion: if an extension point starts needing changes to the **core model** (`FrameDescription`'s shape, the engine's compilation, scheduling), it is not an extension
    but a refactor, and that step needs an ADR (the reverse of `G9` is exactly "every addition touches the core").

| Extension point | Where it touches today (actual files) | Places |
|---|---|---|
| A **pass kind** (e.g. compute) | `DrawKind.hpp`'s `DrawKind`; `Plan.cpp`'s validation; `RenderPass.cpp`'s kind → node mapping; a new node class (following `FullscreenPass.cpp`); `EngineCompile.cpp`'s describe; each backend's frame compiler and recorder (switches over `DrawKind`) | 6–8 |
| A **pixel format** | `RenderTarget.hpp`'s `ColorFormat` / `DepthFormat`; vk `TargetObjects.cpp`'s `colorFormatOf` / `depthFormatOf`; vk `ContentImages.cpp`'s `formatOf`; vsg `VsgSceneRules.cpp`'s format mapping | 4 |
| A **pass declaration field** | `PassDesc.hpp`; `Plan.cpp` (diff and validation); `FrameDescription.hpp`'s `PassDraw`; each backend's recorder | 4+ |
| A **content channel** (vertex attribute) | `Geometry.hpp`'s `GeometryDescription` channel table; `ShaderAbi.hpp`'s block layout; vk `ContentDraw.cpp`'s channel → location mapping and vertex format; vsg's binding | 4 |
| A **backend** (and, for a second API FAMILY such as D3D12 or Metal, also its `check_<family>_exposure` gate over the SDK's family spelling and its own shader translation - the SDK keeps ONE shader source language, review 8 `P4`) | a new plugin (`Plugin.cpp` + `BackendFactory.cpp`), the two halves of `RenderBackend`, its own `test_*`, a `check_*_exposure` gate, `RenderBackendRegistry` self-registration | about 6 kinds of file, +2 for a new family |

2.12 **`Content*` names only the content model and identity** (naming, `G12`): the `Content*` prefix belongs to **content objects themselves** — `ContentId` /
    `ContentRef` / `ContentRegistry` / `ContentDeclarations` / `ContentStore` (the device's "identity + row" table). GPU objects compiled or cached from content are named by **what they are**, without the `Content` prefix: `CompiledPrograms` (the compiled half of each
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
| Frame driver (one frame's machinery and scratch) | `Device::FrameDriver` (lent by `createFrameDriver`) | `VsgDriver` (lent by `createFrameDriver`) |
| The value plan resolved at frame head | `FrameHead` | `FrameHead` |
| The device's target table / target objects | `TargetTable` / `TargetObjects` | `TargetTable` / `TargetObjects` |
| Window target | `WindowTarget` | `WindowTarget` |
| Window / offscreen session | no separate type (the device's own frame machinery) | `WindowSession` / `OffscreenSession` |
| Content facts and rows | `ContentCaches` (rows and command plans included) | `ContentStore` + `ContentFacts` |
| Compiled half (per (program, rev, layout, variant)) | no separate type (`ContentCaches`'s `ContentPipelineKey`) | `CompiledPrograms` |
| Declared descriptor sets | `ContentCaches`'s set cache | `BindingSets` |
| Readback | `ReadbackStaging` | `HostReadback` |
| Lights | SDK `LightSet` | SDK `LightSet` |

    The `Content*` names that stay differ between the two sides (`ContentCaches` / `ContentStore` / `ContentFacts`) and that is not drift: they all belong to
    "the content model and identity" (2.12's first sentence), and one backend keeps rows and command plans in one object while the other separates them —
    the name says that.

2.13 **`SceneView` is a per-window controller, not a `View` value** (naming, `G12`): the class owns one window's view state - camera, content scene,
    manipulator, input, the surface size it follows and the `PassChain` it ADOPTS - and answers "which pass draws me" for the area that presents it.
    Why: the name states half of what the object is (it is also the chain's adopter and the surface layout's host), and a reader arriving from Filament
    reads `View` as the per-frame camera + viewport value. Boundary: the `View` VALUE in a frame description is the camera snapshot + device-pixel area +
    layer mask and lives for one derivation; `SceneView` lives as long as its window, owns no passes (the chain does), and does not decide presentation -
    that is the AREA's declaration (`RenderArea::present`), never the view's guess. Renaming it is a public-API batch, not a doc edit (the reader-facing
    table is `docs/guides/graphics.md` §5.6).

2.14 **A public name says what the caller is handed** (naming, `G4`): three spellings changed on 2026-11-18 because each described a
    neighbouring thing rather than the thing in the caller's hand; the migration notes are `BC-22` (`DiagnosticsRoute` →
    `DiagnosticsHandle`), `BC-23` (the `MaterialPtr` alias deleted from `Geometry.hpp` - `Material.hpp` is its home) and `BC-24`
    (the one-argument `resolveRenderState` → `applyStateDefaults`). Reasons and rejected options:
    [ADR-2026-11-18](ADR-2026-11-18-names-state-what-is-handed-over.md).
    * `Diagnostics` is the ROUTE (counting plus the host's sink) and is what a DEVICE is handed (`RenderDevice::setDiagnosticsRoute`);
      `DiagnosticsHandle` is a borrowed `Lease` onto the engine's route, which is what the ENGINE hands out (`RenderEngine::diagnostics()`,
      and what a chain holds). The old name made the handle read as the route itself (`G1`).
    * A pointer alias declared in two headers, one of which never uses it, teaches the wrong owner; deleting the copy costs a host one
      include it already needed (`G1`).
    * `applyStateDefaults(state)` fills what nobody stated and takes no context, so under the derivation rule of
      `graphics-scene.md` 2.12 it was never a `resolve*`; the arity rule that separated it from the pass-aware overload is one a reader
      cannot see at the call site (`G4`).
    Boundary: this is a vocabulary batch, not a behaviour one - no signature changed shape, no default changed, and the in-tree call sites
    are the whole migration.

    The same rule covers **verbs**, because two spellings were doing one job: a **driver** is spelled `update`
    (`PassChain::update` / `SceneView::update` - its job is to walk its objects and call them), while a **reaction** is
    spelled `on<Event>` (`onSurfaceResized`, `onResize`, `onKeyDown`, `onScroll`, and - since 2026-11-18 - `FrameUpdate::onFrame`).
    The former `FrameUpdate::update()` was the reaction side wearing the driver's verb, and the pair read as a stutter;
    it is the one rename of that batch that changes no type (`BC-25`).

2.15 **A fact earns a place on the public surface when the host must decide on it, never because a backend can**
    (public surface, `G1` / `G8` / `G13`): "should this backend capability be exposed?" has been asked twice about GPU
    resources, and both times the answer was decidable by a test rather than by argument (`../backlog/graphics.md` T30).
    * **Three questions.** For a candidate member: (a) without it, would a host have to GUESS? (b) without it, would a host
      have to open a backend header? (c) is it only "this backend has the ability"? Answers (a) or (b) admit it; (c) alone
      excludes it.
    * **Three admissions, each a host decision.** Something to BRANCH on before drawing (`Capabilities::features.supports`,
      `Limits::max_samples`); something to REQUEST whose refusal is classified (`ReadbackOutcome::Unsupported` means "this
      device never will", and a host gates its feature on it); something to READ and act on (`FrameReport::retention` for
      trimming, `FrameReport`, `PrepareReport`).
    * **Three exclusions.** A resource TYPE or handle - a host never names a GPU object, and the SDK's only `void*` members
      name content identities (`DrawState::material`, `Geometry::Facts::buffer`) or a window (`DeviceDesc::native_handle`);
      a SEMANTIC only one API family has (`DrawState::ImageLayout` used to be this example: T15 moved it into
      each backend on 2026-11-23, which is the exclusion APPLIED rather than an exception to it); an
      internal POLICY (a cache, a barrier, a table - T10).
    * **A feature is the DEVICE's answer; a report part is the BACKEND's** (`K1`, `ADR-2026-11-23`): `Features::GpuTimings`
      says the device can write timestamps, `FrameReport::timing` says this backend measured them, and the two are not one
      claim - a backend that leaves the report part disengaged while the feature is true is honest, not broken. The pair is
      never collapsed into one flag, because a fourth feature for it would move a backend fact onto the public surface,
      which (c) excludes.
    * **Three costs, all permanent.** A promoted fact is one more obligation on every future backend (`RenderDevice` carries
      19 virtuals today, 5 of them pure); a DEFAULTED one is a silent lie, because a backend that forgets to override reports
      the base default, which reads as an honest "no" while meaning "did not answer" - the distinction `ResidencyRow` states
      as "absent is not zero"; and a TYPE cannot be defaulted at all, so promoting one locks it into the ABI where a field
      would have stayed additive.
    * **Applied to residency (`BC-26`)**: `ResidencyKind` speaks the host's content model - `Streams` → `Geometry`,
      `Images` → `Textures`, `MaterialBlocks` → `Materials`, `Halves` → `Programs` - and `Sets` is DELETED because it had no
      producer (one reference in the tree, its own name table). No deprecated aliases, for the reason `BC-17`…`BC-21`
      give. Every backend notes every kind, so a row's existence is the promise, a number inside it may stay absent, and a
      host writes one loop over `rows()` instead of asking which backend answered.
    * **Applied again (`BC-27`)**: `ResidencyTotals` is DELETED, and `ResidencyLedger::live()` is what remains - the one
      sum every row can answer. `bytes` could never be engaged: no GPU API reports the memory a compiled pipeline occupies
      (Vulkan has no query; D3D12's allocation info covers resources, not PSOs; Metal's `allocatedSize` likewise; WebGPU
      exposes neither), so a total would always have left `Programs` out while looking complete - and `unknown` existed
      only to explain that gap. Same rule as `Sets`: a promise no backend can keep does not stay on the public surface. The
      device-wide memory figure is a capability plus a report part instead (`../backlog/graphics.md` T46).
    Boundary: this decides what may be PUBLIC, not what a backend may HAVE. A mechanism both backends use is a fact about the
    backends, and it lives flat in the SDK mainline (`ADR-2026-11-16`); this point does not reopen that - a second CONSUMER is
    what moves a mechanism there, which is a different trigger from "a second backend exists".

## 3. Features

### 3.1 Device, window, area, view, engine

| # | Feature | Notes |
|---|---|---|
| F1 | Device | `RenderDevice` (`RenderBackend`): device + queues; owns no window. |
| F2 | Window target | `WindowPresentTarget`: window handle + swapchain; one target per window. |
| F3 | Area | `RenderArea`: size + present table; created by `RenderEngine::createArea()`; one area, one picture. |
| F4 | View | `SceneView`: camera + scene + manipulator + input + the adopted `PassChain`; answers "which pass draws me" (input ownership is in `appfw-render-view.md` §2.1 / §2.4). |
| F5 | Engine | `RenderEngine`: `initialize` / `shutdown` / `frame() → FrameCallOutcome`, device borrowing, size, capabilities, reports. |

### 3.2 The engine's three facades

| # | Feature | Notes |
|---|---|---|
| F6 | `PassRegistry` | The only pass registration table; `add` / `remove` / `count` / `orderOf` / `contentOf` / `bindContent(...) → bool`. |
| F7 | `OutputRegistry` | Named outputs and transients: `publish` / `resolve` / `unpublish` / `transientTarget`. |
| F8 | `DiagnosticsHandle` | Diagnostics: `setSink` / counts / `isClean`; the engine dies first ⇒ `isValid()` false. |

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
| F20 | Size | The single owner of the surface size is `RenderArea`; views follow through `resize` / `addSurfaceResizeHandler`. |

## 4. Public Interface

### 4.1 `RenderEngine`

| Interface | Purpose |
|---|---|
| `initialize()` / `shutdown()` | Lifetime |
| `frame()` | Compile and execute one frame; returns `FrameCallOutcome` (Ran / Submitted; `NotReady` + `EngineNotReadyReason`) |
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
| `DiagnosticsHandle::setSink()` / `count()` / `isClean()` | Diagnostics |

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
| `SceneView::resize()` / `addSurfaceResizeHandler()` | Follow the size |
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
| N1 | `PassChain` only borrows the engine (`PassRegistry` / `DiagnosticsHandle` are leases) and does not own it; the engine dies first ⇒ `isValid()` false. |
| N2 | The chain's passes are registered with the engine; the chain keeps no second pass list. |
| N3 | `PassDesc::camera` is an owning `intrusive_ptr<Camera>`; do not pass the address of a stack camera. |
| N4 | One area, one picture; multi-cell layout belongs to the application layer, do not add a second area to the engine. |
| N5 | `Camera` carries its own aspect; a pass rectangle inconsistent with the camera shape is reported at compile time. |
| N6 | The backend materialises `FrameDescription.passes` only; do not reorder or build a dependency graph on the recording path. |
| N7 | `frame()`'s `FrameCallOutcome` only says whether it ran (Ran / Submitted) or the engine was not up (NotReady + reason); if it ran but drew nothing, read `frameReport()`, do not infer the drawing result from the return value. |

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
| D7 | A public GPU resource type, or a polymorphic resource table | A host never names a GPU object, so the type would have no consumer at any backend count; a base class would be a forwarding layer over whichever library creates the object (`G7`), and it would need a device reference on the resource (`G2`). See 2.15 and [ADR-2026-11-10](../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md) option D. |

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
`graphics-content-lifecycle.md` · `vsg-backend.md` · `../backlog/graphics.md` ·
`../decisions/ADR-2026-11-23-retiring-the-reference-backend.md` (the seam's promise rule and the retirement's terms)
