# .ai/ — the AI-facing directory

Repo-versioned knowledge for AI assistants and the team. `docs/guides/` is user documentation; design
pages and notes live here. Closed, explicitly-not-doing and superseded material is not kept in the tree
(look in git when you need it): only the current design, the decision records, open backlog items and
module notes stay.

**The index is machine-checked** (`scripts/check_ai_docs.py`): every `.md` under `.ai/` must be named
here, and every name here must exist.
**Writing**: conclusion first, history below; a current section that names something absent carries
`<!-- drift-ok -->`.

**Layers**: `design/` states **what is** (current, undated); `decisions/` states **why it is** (ADRs,
immutable dates); `backlog/` holds **what is not done and what would trigger it**; `memory/` holds
short facts; `bugs/` is one file per incident. History belongs to git; it does not go into `design/`.

**Directory split**: `src/<layer>` holds **libraries**; `src/apps/<product>` holds **installable
applications** (`src/apps/vine` → `Vine.exe`); `tools/<name>` holds **non-installed developer tools**
(`tools/urdf2vine`); `tests/` holds the suites. An executable is not automatically a product: a
module's own process half (e.g. `src/base/crash/handler`'s `CrashHandler.exe`) stays in the module and
does not move into `src/apps/`.

## design/ — the living design (59)

Every design page has the same shape: **one-line purpose → design points (2.x) → features (F#) →
public interface → caveats (N#) → rejected options (D#, one line + ADR link) → acceptance (T#) →
related documents**.

**graphics / vk / vsg (13)**
- `graphics-architecture.md` — layering, device/window/engine/chain/view, a frame and scheduling.
- `graphics-scene.md` — nodes, state, collection, layers, view context, picking.
- `graphics-geometry.md` — channels, `Primitive`, instancing, revision, bounding boxes, storage and sharing; mesh triangle identity (`triangleCount()` / `triangleCorners()`) and the triangle-tree cache (F32).
- `graphics-rendering.md` — targets, MSAA, image sources, colour, shading, shadows.
- `graphics-content-lifecycle.md` — content release and cache policy.
- `graphics-frame-pipeline.md` — the frame pipeline (**draft**): the target shape once graphics-next is adopted — content → snapshot → derive → execute, four one-way layers, 8 nouns / 3 verbs, `C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` / `P1–P11` / `D1–D11` / `T1–T34`, a landing status per feature, and the migration table (step 5 split into 5a / 5b-1 / 5b-2a / 5b-2b / 5b-2c) in §9.3.
- `vsg-backend.md` — vsg backend architecture / class diagram / contracts / keys and state / pitfall index / rejected options / gates.
- `vsg-pitfalls.md` — the vsg backend's P1–P82 pitfall table, grouped by nine risk domains, each with its symptom and criterion.
- `graphics-shader-params.md` — the shading-parameter ABI: five blocks, one declaring owner each, GLSL deduplicated, names only at wiring time.
- `graphics-material.md` — material: metallic-roughness PBR, texture sets, asset/instance, both paths agree (draft).
- `graphics-sessions.md` — the **implementation design** of device and session (**draft**): a session is a frame driver (not a window); the frame-head snapshot keeps the record path lock-free; residency has three states (publish or withdraw, a race copies); targets are claimed per frame; cross-session retirement reuses `DeviceClock` + `RetirementQueue`; a nine-step migration table. The basis for `T48`.
- `graphics-threading.md` — the threading model (**living**): the content side publishes one immutable snapshot with `publish()` and the engine half only reads it; each of the two threads has one writer, the only payloads that cross the boundary are "the published value" and "the completion report", and the bytes are polled through two atomic revisions; the surface belongs to the content thread and the swapchain to the engine thread, and parallelism lives only in the pure values of the derive layer.
- `vk-backend.md` — **the normative design of `gfx_backend_vk` (direct Vulkan)**: session / one frame / targets / content / memory pool / pipeline cache, `F1–F15` / `C1–C9` / `R1–R6` / `N1–N7` / `P1–P15` / `D1–D6` / `T1–T9`; §9.2 is its gap list.

**appfw (17)**
- `appfw-startup-phases.md` — the three startup beats and the startup worker thread.
- `appfw-startup-splash.md` — the splash frame and startup progress.
- `appfw-render-surface.md` — render-surface lifetime and the platform fallback.
- `appfw-render-view.md` — the render-view design (four-layer API, recipes belong to the chain, input routing, shading modes).
- `appfw-command-manager.md` — CommandManager.
- `appfw-command-arguments.md` — command arguments: one tokenizer, carried with the execution, parsed by the command, history/events replayable.
- `appfw-eventbus.md` — EventBus.
- `appfw-plugin-system.md` — the plugin system (disable and uninitialise included; one manager per process; command attribution per registration).
- `appfw-config.md` — the configuration subsystem.
- `appfw-progress.md` — ambient progress (`ProgressHost`).
- `appfw-userio.md` — UserIO.
- `appfw-document-model.md` — the document model (identity + lifetime + type registration).
- `appfw-ribbon.md` — Ribbon identity/roles/placement: id, role, placement, the drop zone and order resolution.
- `appfw-command-binding.md` — a command entry's display state: enabled / target requirements / tooltip description / checked provider.
- `appfw-backstage.md` — the application-button event, menu versus Backstage, the top-level overlay window and page navigation.
- `appfw-app-info.md` — app identity and app info: `AppInfo` / `AppConfig::info`, `Application::appInfo()`, the about panel and dialogs.
- `appfw-translation.md` — UI text translation: only appfw and above, source language English, translations Chinese only, the Qt catalogue ships with the module and is embedded (a file may override it), loaded at startup with an English fallback.

**base and the rest (29)**
- `async-design.md` — the `src/base/async` design (three rules that must hold).
- `base-crash.md` — crash reporting: `vn::Crash` assembly / report format / handler symbolisation and the crash window.
- `base-progress.md` — progress primitives: one [0, 1] scale, the move-only reporting handle and the staged scope, coalesced notification, the destruction-order and restart traps, and what appfw adds on top.
- `base-runtime.md` — dynamic library loading (identity is the file, retention is the owner's choice, `lastError` explains a failure) and the cache: one typed `Cache` seam with explicit `Ok`/`Miss`/`Failure` answers, the in-memory implementation that never serialises, one detached sweeper per stored type, and the persistent seam (`KeyValueStore` + `SerializedCache`) whose file/database/network backends live in their own modules.
- `base-system.md` — the platform query module: one platform family per file behind a private seam, the shared policy (pid guard, query-once-and-cache, zero-core fallback, no trailing separator), the per-platform vocabulary written into the headers (`version` spelling, `exists()` semantics, volume-vs-device, the empty answers), and the traps that cost a defect each.
- `brep-operations.md` — runtime B-rep operations: module ownership (`brep` / `brepio` / `geometry`), tessellation and primitive-to-solid contracts, failure representation.
- `crypto.md` — the crypto module: algorithms only, one object one direction, incremental and one-shot are synonyms, backend initialisation in exactly one place (`detail::ensureLibraryInitialized`); today's implementation (digest / HMAC / HKDF / password hashing / AES-GCM·CBC·CTR / RSA / ECC plus DER, PEM and password-protected PKCS#8 key files) and the gaps (certificate parsing).
- `geometry-axis-surfaces.md` — axisymmetric analytic surfaces (cylinder / cone / torus): the `Placement` value, the shared "in-plane axis" rule (`Frame.hpp`), rectangularly trimmed surfaces, and where spline surfaces stand.
- `geometry-kinds.md` — the geometry families: `Curve` / `Surface` are evaluation interfaces (`Curve<T>` eleven items, `Surface<T>` fourteen including `isValid()`), surfaces add a classification tag (`ElementarySurface` / `BoundedSurface`); no placeholder types remain in either family.
- `geometry-planar.md` — planar geometry: `geom::planar::Curve<T>` is a second contract with the same meaning as the 3D one (not a generic specialisation, not an inheritance), today with line, circle, ellipse, hyperbola, parabola, arc, Bezier, BSpline and offset; the three non-circular conics share `ConicCurve` (centre + start axis + `CN`) while the circle stays out of that layer; an arc is "a circle by value + an angle interval"; family-level algorithms are nearest point, three-way intersection, nearest distance between two curves, arc length and its inverse, and signed curvature; bounded types each have `bounds()` (`math::Rect2`, the ellipse via its support function, splines as a pole box); the module is header-only templates in the same library (`vn::Geom`) and the same global as the spatial side, and names carry no dimension marker.
- `geometry-parametric.md` — parametric geometry is a modelling type (the five `Primitive` subclasses live together in `Primitive.hpp`); the canonical local space and how far OpenCASCADE's responsibilities are pushed back.
- `geometry-primitives.md` — geometry's spatial value types: ownership, naming and contracts of `Aabb` / `Obb` / `LineCurve` / `Plane`.
- `geometry-module.md` — the geometry module's shape and naming: one library `vn::Geom`, one global, four kinds of location (the two dimension trees / `Continuity` / `geom/detail`), the three-step "what goes where" test, five isolation gates (including that `detail` names no dimension and includes no domain), naming rules (no dimension markers on types), and the module-level rejected options.
- `geometry-ray.md` — rays and primitive intersection: `Ray` belongs to `geometry`, intersection hangs off the object being intersected, scene traversal stays in `graphics`.
- `geometry-bvh.md` — the spatial bounding-volume hierarchy: `Bvh` is pure structure (bounds, 32-byte flat nodes, `parent + 1` layout, two allocation-free traversals, `refit`, `validate`), `BvhBuilder` is pure construction (two quality levels, injectable foreign algorithms), and the only thing between them is `detail::BuildJob`; `Obb` volume was measured and rejected (about 2× leaf surface area, containment needs a tolerance).
- `geometry-intersection.md` — intersection pairing and result shape: the verb, the result type decided by the intersection's dimension, out-parameter semantics when `false`, the entry-point pairing table and the gaps; signed-distance root finding for curve × analytic surface (`intersectCurveWithSurface` + `signedValue`) and projection root finding for curve × arbitrary surface (`intersectCurveWithSurfaceByProjection`).
- `geometry-surface-queries.md` — family-level surface queries: nearest point / distance from a point (Gauss-Newton) and ray × surface (three-variable Newton), parameter-window rules, coincidence classification and the cost of local solutions.
- `geometry-curve-queries.md` — a curve's nearest point / distance (root finding on the squared-distance stationary points), nearest distance between two curves, arc length and its inverse, curvature (unsigned in 3D), window rules and the limits of local minima.
- `geometry-curve-discretization.md` — curve polyline approximation: the `CurveSampler<T>` base plus three subclasses (equal parameter, equal arc length, chord deviation), the result-object shell (`curve(c)` / `range()` / parameterless `compute()` / `polyline()`) and its reset rules, `CurvePolyline<T>{points, parameters}`, the 2D/3D shared `detail` kernel, seeding and point budgets, why a corner needs no discontinuity table, and progress hooks left as placeholders.
- `geometry-solid.md` — the analytic-solid family `Solid<T>` (five kinds, by value): nearest point and ray intersection; the model stores dimensions only and builds the solid on demand.
- `geometry-tessellation.md` — tessellating analytic solids: free-function overloads from the five kinds to `Triangulation<T>` (positions / normals / indices), quality parameters, exact normals and vertex duplication, closed and outward winding, and the model-side `meshFromTriangulation()` assembly.
- `imaging-design.md` — the imaging module (CPU pixel data).
- `imageio-design.md` — imageio's public face (container capability table, `Result`/`IoError`, stateless free functions).
- `iobase-vfs-design.md` — IOBase (`vn::io`) VFS and Stream.
- `math-base-layer.md` — `math` is the base layer and holds mathematical vocabulary only: value types / constants / concepts / 3×3 eigendecomposition, 18 headers with no subdirectories and no procedures; root finding, integration, Bernstein evaluation and the spline representation conventions live in `geom` (the two split criteria plus the cost and the reversal path).
- `meshio-brepio-design.md` — meshio / brepio's public face (draft: format enums, `Result`/`IoError`, no singletons).
- `robotics-io-design.md` — Robotics IO (XML, VFS packaging, material libraries).
- `robotics-proximity-design.md` — Robotics proximity (interfaces, the VMR comparison, the FCL integration point).
- `window-design.md` — the window module's cross-platform window context.

## decisions/ — decision records (ADRs) (84)

One ADR records one decision: background, options, decision, consequences, evidence. Status is
`accepted` (or partly superseded, when a live half remains); the date is immutable; when a later ADR
replaces one whole, the replaced file is deleted and its successor states what it replaced - git
keeps the old argument.

- `ADR-2026-11-19-cache-interface-and-backends.md` — one typed `Cache` interface with three-state
  answers; `InMemoryCache` implements it without serialising, a persistent backend serialises into a
  `KeyValueStore` behind an injected serializer, and the rejected erased/byte/dual-form shapes are
  recorded.
- `ADR-2026-11-19-iobase-whole-content-bound.md` — a whole-content read (zip entries, `addFile` on a buffering backend,
  `Zip::decompress`) is bounded at 1 GiB and answers `CapacityExceeded`; `openRead`/sinks/staging carry larger content.
- `ADR-2026-11-19-iobase-writes-publish-whole.md` — every write that replaces a file stages into a uniquely named
  sibling and renames it into place; a failed save or `addFile` leaves the previous file whole.
- `ADR-2026-11-19-progress-range-move-only.md` — the progress reporting handle is move-only: a copy that disarmed its source changed a `const` object's state and let assignment drop a pending portion; move assignment now completes the target first, "empty" is one invariant (`parent_scope_ == nullptr`), and a reference-counted handle is rejected because the reporting moment becomes unknowable.
- `ADR-2026-11-18-plugin-command-attribution.md` — **a command is attributed by the call that registers it, not by the stack it happens to run on**: `PluginLoadContext::registerCommand()` sets the owner tag around that one call and restores it, `PluginManager` holds no tag across a lifecycle hook (the synchronous door pumps, so a suspended hook is not a boundary), and passing the owner per registration (an ABI change for every call site) is rejected.
- `ADR-2026-11-18-the-frame-carries-its-context.md` — **the frame carries its context, and one clock measures it**:
  `FrameContext` (frame id, interval, area size, ratio) is composed by `RenderEngine::frameContext()` and handed to
  `FrameUpdate::onFrame(const FrameContext&)`; the clock is read once per frame at the head of `frame()`, so the value
  is a pure read one frame behind; `FpsOverlay` stops reading `steady_clock` (`BC-25`). Landed on an explicit go
  ahead of its trigger, which the ADR records as not hit.
- `ADR-2026-11-18-names-state-what-is-handed-over.md` — **a public name says what the caller is handed**: `DiagnosticsRoute` → `DiagnosticsHandle` (the route is `Diagnostics`; the handle is what the engine lends), the duplicate `MaterialPtr` alias deleted from `Geometry.hpp`, and the one-argument `resolveRenderState` → `applyStateDefaults`; migration notes `BC-22`…`BC-24` in `graphics-architecture.md` 2.14, no deprecated aliases.
- `ADR-2026-11-18-public-vocabulary-unification.md` — **the public vocabulary is unified on what each thing is**: `RenderCommand` → `DrawItem`, `DrawDescription` → `DrawCall`, `MaterialBase` → `MaterialValues`, `EngineNotReady` → `EngineNotReadyReason`, `createSession` / `releaseSession` → `createFrameDriver` / `releaseFrameDriver`; five migration notes (`BC-17`…`BC-21`) in `graphics-rendering.md` 2.10 and **no deprecated aliases** - an alias would restore the double spelling the batch removes.
- `ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md` — **the seam is a property and a checked list, not a namespace**: the third-round audit's `vn::graphics::backend` alias group is rejected (one class would get two names; it would cover the 8 mechanisms while 25 SDK headers are shared, so the name would understate the seam), and the seam's identity becomes a measured property ("both backends reach it") recorded once in `check_include_hygiene.py`'s `SEAM_HEADERS`, with the two pages that state it (the design page 2.9 and the backend guide 5) required to name every mechanism the list holds.
- `ADR-2026-11-16-authoring-view-and-target-vocabulary.md` — **values carry declarations, behaviour belongs to objects**: a `View` value (`CameraSnapshot` + `Viewport` + layer mask, `CollectView` retired), `RenderArea` → `RenderArea` (surface stays with the platform), a pass's target collapsed into the two values `Target` + `Output`, `PassDesc` as the only authoring shape (`RenderPass`'s 35 setters become a read-only live handle), zero callbacks in values (`before_draw` moves to the object's `update()`, `OverlayLayout` returns a `Viewport`), the dead field `tag` deleted, `setRenderTarget` made real (vk pre-builds; CAP-86 added) or retired, the `releasePass` contract narrowed, `Capabilities` = `Features` + `Limits`, diagnostics gain `origin` (CAP-87), the light set no longer allocated per pass; four normative text revisions plus 26 section-comment cleanups. The 18 batches landed on 2026-11-17, each one's numbers in its commit body.
- `ADR-2026-11-16-redesign-seam-and-vocabulary.md` — **the redesign is names and ownership, not capability**: the two backends' seam (`backend/`'s 25 headers reduced to 6 shared mechanisms + the guard, 18 vsg-only headers moved into the plugin), vocabulary (the design noun `Session` retired in favour of `FrameDriver` / `FrameDriverId` / `WindowSession` / `FrameHead`), a frame in three phases (frame-head resolution under one lock / lock-free recording / one submit lock, `prepare` = `Submission::TransferOnly`), one refusal shape (`DiagnosticDraft`), the target table and the engine composition root put back where they belong, the selection rule collapsed into one; `RenderBackend` the aggregate and `RenderDevice`'s five responsibility faces are kept (`G8`: no consumer), with the trigger conditions written into the ADR; the 18 batches landed on 2026-11-17. §2 and batch 3's "the directory is the seam" are superseded by the next entry.
- `ADR-2026-11-16-no-backend-half.md` — **delete the "backend half"**: directory, include guard, the `VineGraphicsBackendSdk` install component and the file-set gate all maintained one boundary from four places, at a cost above the benefit (`G7`); the 8 shared mechanisms are flattened into `sdk/vine/graphics/` and merged into `vn::graphics` (`Keys.hpp` → `DrawState.hpp`, `ReadbackResult` → `ReadbackAnswer`), and `gfx_backend_vsg`'s own types move into `vn::backend_vsg::core` (`G13`). Gates became property-based (the module must not include a render API or a concrete backend; exported symbols are checked by file list).
- `ADR-2026-11-14-sample-count-refused-not-lowered.md` — **a sample count is a count; a request the device cannot build is refused once, not lowered**: `Capabilities::max_samples` is a power-of-two count (`vsg` used to answer the bit mask 15); above the limit or not a power of two is refused (`SamplesUnsupported` / `UnsupportedRequest`, reported once), so `RenderTarget::samples()` is always the number the images are drawn with; lowering, rounding up and adding a bit set to `Capabilities` are rejected.
- `ADR-2026-11-14-prepare-may-submit-transfers.md` — **preparation may submit one transfer-only batch**: `prepareFrame` still submits no frame (no acquire / no present / no `FrameProgress` movement), but it may record and submit **one** command buffer containing copies and layout transitions, waiting on its own fence — so texture uploads land during loading and `R23` is no longer "except textures"; a second entry point, a background thread, and "upload while filling" are rejected.
- `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` — **vk keeps one frame in flight**: measured, one `renderFrame()` has a median wall clock of 8.63–8.94 ms and 7.95 ms of device time in the same frame ⇒ **the device is ~90%, host work is ~0.9 ms**, so multiple frames in flight could buy at most ~10% while costing the "draw-then-read" contract; multiple slots and "wait only for the previous frame" are rejected, with trigger conditions (device time below half the wall clock, a product shape that needs to overlap the GPU, a second backend wanting the same semantics) written into the ADR. **Also corrects** `design/vk-backend.md` §2.3, which had earlier read "wall clock including the fence wait" as host work.
- `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` — **the device is a resource pool, sessions own frames**: two engines share geometry / shader / texture (one upload, one compile) but **not frames** — `RenderDevice` is the pool (memory, residency cache, targets, pipeline cache, locked per resource), `RenderFrameExecutor` is the session (command pool / fence / constant ring / descriptor pool, thread-confined); residency has three states `Absent → Uploading(owner, frame) → Resident`, only `Resident` may be drawn, a contending session builds a temporary copy instead of waiting (no cross-session blocking); a target is claimed per frame and a conflict skips only that pass; a release parks until every session's `completed ≥ epoch`; a global device lock, per-entry reader/writer locks and one device per engine are rejected; done in two steps (frame gate + contract, then the split, ~1350–2100 lines).
- `ADR-2026-11-14-vk-becomes-the-default-target.md` — **`gfx_backend_vk` becomes the production backend and eventually replaces `vsg`**: platforms are Linux/X11 first and Win32 later (one `WindowSystem` per device; adding a platform is an addition); unimplemented capabilities answer `Unsupported` plus one diagnostic instead of degrading silently; the gates that must pass before switching the default (`releaseRenderTarget`, `readDepthBuffer` or a difference table, device-lost diagnostics, ASan/TSan/validation, platform and performance evidence, user documentation) are written into the ADR; switching immediately and long-term coexistence of two backends are rejected.
- `ADR-2026-11-13-content-seam-stays-in-the-plugin.md` — the content-bookkeeping half (facts table / halves / keys, guarded by `check_vsg_content_seam.cmake`) **stays inside the backend plugin** and is not promoted to an SDK library (`G8`: there is no second backend, so packaging must not precede the consumer); when a second backend appears, move or copy it along the seam.
- `ADR-2026-11-13-staging-keeps-the-reusable-block.md` — upload staging **keeps the reusable minimum-size block** (`staging->size <= minimumStagingBufferSize`) and frees only the one-off large blocks above it: a steady-state upload frame allocates nothing on the host, while a 1.2 GB model load is still not retained; supersedes ADR-2026-11-10's "retain nothing".
- `ADR-2026-11-13-one-engine-one-surface.md` — **one engine drives one surface**: rejects graphics-next's F8 (one engine, many surfaces / `Target{SurfaceId}`); multiple windows mean one device serving several engines (one engine / one area / one `Viewer` per window); `FrameResult.present` / `PresentRecord` stay single-valued; trigger conditions (a real-host dual viewport / a second backend) are written into the ADR.
- `ADR-2026-11-12-adopt-graphics-next.md` — **adopt graphics-next as the next frame-pipeline design** (four one-way layers: content → snapshot → derive → execute); the normative document for the target shape is `design/graphics-frame-pipeline.md` (draft), `K1–K11` turn from "reject the proposal" into "acceptance thresholds and fallbacks per step", and backlog `T32` leaves the trigger-gated table; keeping the status quo, keeping only the landed identity/lifetime layer, and a big-bang rewrite are rejected.
- `ADR-2026-11-12-snapshot-takes-over-collection.md` — **the snapshot taking over production collection is split into 5a shadow / 5b-1 flip the source / 5b-2a public entry / 5b-2b delete / 5b-2c close the passes**: 5a runs both paths and diffs item by item while the old path stays the rendering truth; 5b-1 binds Published mode to `SnapshotSource`; 5b-2a publishes the three-state `Scene::publish()` and lets `FrameWorker` hold `SnapshotSource` directly; 5b-2b deletes `ContentMirror` / `DeltaChannel` / `RenderWorld` / `PublishOutcome` and 23 LEGACY cases; 5b-2c only then deletes the `addPass` family and the runtime refusal; doing it in one step, in three steps, and keeping the mirror plus a debug-only dual run are rejected.
- `ADR-2026-11-11-content-identity-is-a-handle.md` — content identity is a **handle** (`Handle` gen24/domain8/index32 + `ContentRef{id,revision}`), not an address: the public `ContentId.hpp`, `Node::id()`, row identity / mirror keys / `RenderCommand::node` / plan rows / the backend geometry table all move to `NodeId`; landed in three stages (public identity → `ContentRef` as the single key → epoch reclamation); partially supersedes `ADR-2026-11-09-address-keys-hold-the-object`.
- `ADR-2026-11-10-render-world-mirrors-content.md` — the render world **mirrors** the content world: content stays on the UI thread (the editing API is unchanged), the mirror (world matrices / world boxes / folded state / proxies) lives on the engine thread and is updated by deltas (a payload is mandatory, so a missing one does not compile), with bytes polled through an atomic revision; the surface belongs to the content thread and the swapchain to the engine thread; handing content to the engine thread (the first draft), per-frame extract, a double-buffered world, locking, a thread per frame, per-pass parallel recording and vsg's threading path are rejected.
- `ADR-2026-11-10-gpu-residency-pinned-by-content.md` — GPU residency is pinned by the **content object (identity + revision)** and reclamation only asks "who is alive right now" (the facts table enumerates the pin set); the time window and the capacity cap are deleted: one criterion replaces today's five (the stream time window, texture/block refcounts, the counterfactual scans of halves and descriptor sets); that the GPU memory is no longer automatically yielded is its stated cost. The first step (`ResidencyLedger` vocabulary + the stream/image/material-block rows) has landed.
- `ADR-2026-11-10-curve-sampling-is-an-object.md` — curve discretisation is an object: a base class plus three rule subclasses, setter configuration and a parameterless `compute()`; computing on construction and "three free functions plus a return value" are rejected.
- `ADR-2026-11-10-bvh-seal-and-coarse-picking-tree.md` — a BVH can be sealed (`seal()` drops the build-time primitive boxes, keeping only nodes / node boxes / order), the picking tree uses coarse leaves of 64 and is sealed as soon as it is built, and vsg and the index upload alias the model buffers (a 14M-triangle picking tree: 1.36 GB → 88 MB, plus 1.18 GB saved in the backend); a separate sealed type, deleting `update`/`refit`, and changing the library's default leaf capacity are rejected.
- `ADR-2026-11-09-address-keys-hold-the-object.md` — a cache keyed by address must hold the object the key names: an address can be recycled while the row lives, and then the key names a new object.
- `ADR-2026-11-09-buffer-edit-scope.md` — `Buffer`'s write entry collapses into one scope (shared mutable bytes have exactly one writer).
- `ADR-2026-11-09-scene-collection-two-halves.md` — scene collection splits into "bake" and "select" halves (the first half has landed; the second is awaiting measurement).
- `ADR-2026-11-09-box-transform-lives-in-geom.md` — the box transform operator returns to `geom` (`Aabb::transformed()`, affine only, a non-affine transform answers an empty box), deleting `graphics::transformBox()` and the copy in `RayIntersection.cpp`; `Obb` is not offered for now (it cannot represent a general affine transform, and has no consumer).
- `ADR-2026-11-09-solid-tessellation-in-geom.md` — tessellation returns to `geom`: `tessellateSolid()` returns the precision template `Triangulation<T>` (positions / normals / indices), and `meshFromTriangulation()` assembles it into a `TriangleMesh` on the model side; returning `TriangleMesh` (a back edge) and `Buffer` (one extra copy) are rejected.
- `ADR-2026-11-09-solid-closed-set-without-variant.md` — delete the `SolidShape` variant and `solidShapeOf()`: the closed set is expressed by five kinds + `visitPrimitive()` + one overload per kind; the `Solid<T>` family interface and the five kind values are unchanged.
- `ADR-2026-11-09-bvh-shape-is-the-tree-storage.md` — the BVH's shape is its own storage (`BvhTopology`: three arrays plus parallel node volumes) and `BvhTopology.hpp` keeps only vocabulary; restore and move get two entry points (`RestoreTopology`, `Bvh::restoreTopology()`) and one check. Inner classes, AoS transcription and accessor-ising are rejected.
- `ADR-2026-11-04-curve-evaluation-interface.md` — the curve family becomes an evaluation interface (`Curve<T>` virtuals + `TrimmedCurve`), and `Ray` leaves the family.
- `ADR-2026-11-04-surface-family-queries.md` — family-level surface queries use free functions + parameter windows + Newton iteration; the `Surface` interface is not extended and no per-type special cases are written.
- `ADR-2026-11-04-surface-evaluation-interface.md` — the surface family becomes an evaluation interface (`Surface<T>` thirteen virtuals + two classification tags), superseding item 6 of the curve ADR.
- `ADR-2026-11-04-geometry-precision-and-instantiation.md` — geometry types keep the precision template, with both `float` and `double` explicitly instantiated and exported.
- `ADR-2026-11-04-geometry-kinds.md` — geometry families are classified with empty tags, not virtual base classes.
- `ADR-2026-11-04-primitive-is-a-shape.md` — parametric geometry is a modelling type (`Primitive` subclasses) and the algorithms belong to the surfaces; OpenCASCADE shrinks to STEP parsing and tessellation.
- `ADR-2026-11-05-primitive-is-a-modelling-base.md` — `Primitive` is a modelling base: it holds no solid, its five subclasses store dimensions and `solid()` builds on demand; the geometry kinds are renamed `*Solid`.
- `ADR-2026-11-05-planar-curve-contract.md` — a planar curve is a second contract (same names and meaning as 3D, no inheritance between them) and the shared vocabulary stays in `math`; a generic `Curve<PointT>`, inheriting the 3D contract, and embedding `z = 0` are rejected.
- `ADR-2026-11-05-planar-trimmed-curve.md` — the generic trimmed view `TrimmedCurve<T>` (`shared_ptr` base curve + interval) replaces `ArcCurve`; the tight box of an arc returns to `CircleCurve::bounds(first, last)`.
- `ADR-2026-11-05-planar-offset-holds-handle.md` — a planar offset holds a `shared_ptr<const Curve<T>>` base curve and its order is the base curve's capability minus one (capped at 2); `isValid()` is false when the base cannot supply a third derivative.
- `ADR-2026-11-05-curve-derivative-capability.md` — derivatives are exposed by capability: `Curve<T>` gains non-pure `maxDerivativeOrder()` / `derivativeAt(u, order)`, answering `std::nullopt` out of range; making one order pure virtual and the OpenCASCADE-style arbitrary order plus exceptions are rejected.
- `ADR-2026-11-06-space-third-derivative-and-offset.md` — the spatial curve family gains a non-virtual `thirdDerivative()` and a capability override (reporting 3), `OffsetCurve` answers the second derivative through the base's third and becomes a constructible value type, with order = base capability minus one.
- `ADR-2026-11-06-merged-geom-module.md` — `geom/planar` and `geom/spatial` merge into `src/base/geom` with a single library `vn::Geom`, and the two SDK trees share one `geom/geom_global.hpp`; 2D/3D isolation moves from a compiler guarantee to `check_geom_layering.py`.
- `ADR-2026-11-06-geom-detail-subtree.md` — the module gains a `geom/detail/` subtree for the dimension-independent shared implementation (curve scalars, window/tolerance rules, bracket narrowing, arc length and its inverse, two-curve refinement); both trees may include it, it may include neither, and public names do not move.
- `ADR-2026-11-06-spline-model-moves-to-geom.md` — the B-spline knot model and basis functions move into `geom/detail` and `Continuity` into the `geom` root, the criterion being "does the signature name a data model of ours"; `math` keeps only mathematical vocabulary and numeric procedures (supersedes the decision that `math` is the base layer; git holds that record).
- `ADR-2026-11-06-math-keeps-only-vocabulary.md` — root finding, integration and Bernstein evaluation also move to `geom/detail` and `math` keeps only vocabulary (supersedes the decision that the numeric kernels live in `math`; git holds that record); the cost (a non-geometry module must depend on `vn::Geom` for an integral) and the reversal path (promote to its own module when a second consumer appears) are written down.
- `ADR-2026-11-06-geom-contracts-nested-under-module.md` — both contracts sink into the `geom/{planar,spatial}` directories and the `vn::geom::{planar,spatial}` sub-namespaces (superseding the previous entry's "keep sibling directories"); re-exports go to zero, directory and namespace agree; and one expected benefit that did not materialise is recorded (nesting cannot shorten `geom::detail::`, because the contracts have their own `detail`).
- `ADR-2026-11-07-crypto-api-shape.md` — the crypto API shape: two factories rather than a `mode` flag, incremental GCM buffering and computing at the end (a failure would release unauthenticated data; the cost and the escape hatch are written down), and backend initialisation in one place inside the module.
- `ADR-2026-11-07-certificates-and-legacy-keys.md` — a certificate yields its public key only and makes no trust decision (the boundary named F15, not a gap), legacy `Proc-Type` PEM is read but never written, and `WOLFSSL_MD5` becomes explicit (an enum with `kMd5` cannot rely on a preset's side effect).
- `ADR-2026-11-07-encrypted-private-keys.md` — password-protected private keys use PKCS#8 + PBES2 (AES-256-CBC + PBKDF2) rather than a home-made container: the whole value of a private-key file is that other tools can read it; a `toPrivatePem(password)` overload (a name must state a format change), a hard-coded iteration count, and writing PBES1 are rejected.
- `ADR-2026-11-07-bvh-as-structure-plus-builder.md` — the BVH splits into structure and builder (`BuildJob` is the only contract); `Obb` volume was measured and rejected; supersedes `geometry-ray.md`'s `D5`.
- `ADR-2026-11-07-mesh-triangle-tree-cache.md` — a mesh carries triangle identity (`triangleCount()` / `triangleCorners()` move up to `Mesh`) and treats the triangle tree as a cache (`Bvh<double>`, one primitive per triangle, id = index).
- `ADR-2026-11-07-incremental-api-shape.md` — the incremental API's shape and names: one-shot work uses free functions (`digest()` / `hmac()`), incremental work is named after the thing itself (`Hasher` / `Hmac`), methods are `update()` / `finish()`, and the home-made `*Calculator` names are dropped.
- `ADR-2026-11-07-aesgcm-streaming.md` — AES-GCM becomes truly streaming (buffering removed): wolfSSL 5.9.4's `Init/Update/Final` entry points plus `WARN_UNUSED_RESULT`, at the cost that "decrypted output is untrusted before `finish()`" must be stated explicitly.
- `ADR-2026-11-04-surface-placement-value-type.md` — a surface's frame is the value type `Placement` (origin + main axis + X direction), not an inheritance layer; the "in-plane axis" rule still lives only in `Frame.hpp`.
- `ADR-2026-11-04-isvalid-on-family-interfaces.md` — `isValid()` is a pure virtual on `Curve<T>` / `Surface<T>`, so a `const Curve<T>&` can be asked too; `Plane` therefore gets a zero-normal criterion for the first time.
- `ADR-2026-11-04-bspline-without-knot-breaks.md` — a spline's internal multiplicity never exceeds the degree: internal multiplicity = degree + 1 is the gap, `isValid()` refuses it, and `C0` therefore always means "the pieces meet at a point".
- `ADR-2026-11-03-geometry-primitives-ownership.md` — the spatial value types belong to `geometry` and the axis-aligned box is renamed `Aabb`; `Rect2` stays in `math`, with no compatibility alias.
- `ADR-2026-11-03-brep-operations-module.md` — runtime B-rep operations become their own `brep` module; `geometry` never touches OpenCASCADE, and detection is centralised in `VineOpenCASCADE.cmake`.
- `ADR-2026-10-24-command-target.md` — a command's target is the view (`DocumentView`), layers may be null, requirements are declared per command, and `RenderControl` is not a target.
- `ADR-2026-11-01-translation-scope.md` — only the UI layer (appfw and above) is translatable; the source language was Chinese, the catalogue used Qt Linguist, and v1 had no runtime switching (the source-language item is superseded by the next entry).
- `ADR-2026-11-02-english-source-language.md` — the source language becomes English with Chinese in the catalogue; a missing translation falls back to English and a non-Chinese system lands on `en`.
- `ADR-2026-11-01-translation-catalog-embedding.md` — the translation catalogue ships with the module and is embedded in it, with a same-named `.qm` beside the module able to override it; the language set is Chinese and English only, so there is one Chinese catalogue.
- `ADR-2026-10-28-ribbon-roles-and-placement.md` — Ribbon places by role + id + anchor, and the drop zone is a resolver invariant; no global sequence numbers and no item ids.
- `ADR-2026-10-28-backstage-overlay-window.md` — Backstage uses a top-level Tool window rather than a SARibbon child-control overlay (whose native surface gets occluded).
- `ADR-2026-10-30-crash-handler-x11-on-linux.md` — the Linux crash window is hand-drawn with Xlib + Xft (system libraries, no UI toolkit linked; supersedes the same day's Qt version).
- `ADR-2026-10-28-document-tabs-and-floating.md` — document tabs live in the central area, and a float is its own top-level window (it does not enter the dock tree).
- `ADR-2026-10-28-central-area-shows-current-document.md` — the central area shows the current document only, and a floated current document is represented by a placeholder page (one `current`, three places agreeing).
- `ADR-2026-10-24-command-target-no-window-query.md` — the target takes one path only: no window-level "current 3D area" query, and the view's accessor is `renderControl()` (partially supersedes decision 4 of the previous entry).
- `ADR-2026-10-24-tabs-not-mdi.md` — a window shows one view at a time (tabs); seeing several 3D areas side by side is expressed by the view's own panes, with no MDI or split-screen.
- `ADR-2026-10-24-material-model.md` — material becomes a single metallic-roughness PBR model plus sparse instances; the alpha mode enters the material and the G-buffer is re-laid out.
- `ADR-2026-10-24-light-block-typed-lights.md` — the light block becomes a typed light array (position / range / cone angle), shared as one UBO by both paths.
- `ADR-2026-10-24-shader-parameter-abi.md` — shading parameters are hand-written and deduplicated; no reflection and no material DSL.
- `ADR-2026-10-23-no-multi-parent-dag.md` — no multiple parents: a strict tree plus instancing.
- `ADR-2026-10-23-instancing-per-primitive.md` — instancing hangs off `Primitive` and has no scope.
- `ADR-2026-10-23-l2a-reverted.md` — L2a is reverted; layered targets wait for L2b.
- `ADR-2026-10-22-engine-schedules-passes.md` — pass scheduling lives only in the engine.
- `ADR-2026-10-22-cache-unbounded-explicit-release.md` — caches are not capped; release is explicit.
- `ADR-2026-10-22-msaa-target-property.md` — MSAA is a target property; colour and depth each resolve one image.
- `ADR-2026-10-21-colour-space-in-types.md` — colour space and numeric domain go into the types.
- `ADR-2026-10-19-produced-vs-imported-image-sources.md` — produced and imported image sources are not merged.

## research/ — research and proposals (2)

Non-authoritative and dated; once adopted, the result moves into `design/` or `decisions/` and the file
here is deleted. Every proposal and audit this tree carried has been converted except the two below,
which exist because the third-round audit's open items were left with no home when its page was
deleted (git is still the archive for the page itself), and because the fifth review's two deferred
proposals (`T40` / `T41`) keep their arguments where a design page cannot carry them.

- `graphics-2026-11-18-audit-4.md` — the third-round audit's open items (`B3`-`B14`, `T-04`-`T-16`)
  re-established against the tree, plus the drift the re-check found (`A4-01`-`A4-04`).
- `graphics-2026-11-18-review-5.md` — the fifth pass: the first-contact example that did not compile,
  four section references that led nowhere, the three derivation verbs written down, the eight-engine
  benchmark, and the two deferred proposals (`T40` `FrameContext`, `T41` the `DiagnosticsRoute` name).

(The earlier triangle-tree two-frame unification and `.vbtp` persistence have been merged into
`design/graphics-geometry.md` 2.10 / F33, `design/geometry-bvh.md` 2.13 / F8 and
`design/robotics-io-design.md` F7.)

## backlog/ — open work (16)

Only items that are **not done and have a trigger**; delete them when done, and delete them when the
decision is not to do them (the reason goes into an ADR).

- `graphics.md` — graphics / vsg open items (content, backend resources, platforms, object model).
- `appfw-startup.md` — what is left of the startup flow (the Windows half).
- `appfw-ribbon.md` — Ribbon open items (order, item id, custom layouts, contextual tabs, manifest).
- `appfw-command-arguments.md` — what is left of command arguments (declarative arguments, help/completion, asking for a missing argument, object arguments, length limits).
- `appfw-eventbus.md` — EventBus open items (the O(N²) drain on a large pending set, and the enum width that belongs to the next plugin-ABI batch).
- `appfw-app-info.md` — app-info open items (build-time version, manifest and third-party acknowledgements).
- `appfw-translation.md` — translation open items (runtime switching, the first English catalogue, plugin metadata, the lupdate gate, the deployment rule for the override file).
- `async.md` — async's next steps (cancellation environment, scope, the go/no-go on frame allocation).
- `base-crash.md` — crash-report open items (last-crash prompt, terminate, stack guarantees, read-only directories, Linux, the upload trigger).
- `base-progress.md` — progress open items (consumer-less scope accessors, stale-stage detection, the `max <= 0` policy, a non-throwing callback, the appfw plugin-ABI coupling).
- `base-runtime.md` — persistent cache backends (file/database) when a consumer appears, real plugin unload, and Windows identity coverage.
- `base-system.md` — the missing design page for a platform-seam module, the fixed-width value types its SDK predates, and the macOS enumeration gaps.
- `brep.md` — runtime B-rep open items (OCCT backend verification, booleans and sewing).
- `crypto.md` — crypto open items (error-code classification, Argon2 threading mode, configurable write-out scheme, a second AEAD, leak detection).
- `geom-planar.md` — planar-geometry open items (ellipse / arc / the `ConicCurve` layer, planar queries, `CircleCurve::pointAt`).
- `geometry.md` — spatial-geometry open items (solid intersection, triangle-face types, fourth derivative and offset order, table algorithms and temporary allocation in spline evaluation, the missing intersection pairings).
- `loaders.md` — loaders open items (ownership and entry point for point-cloud I/O).

## memory/ — module notes (14)

- `glossary.md` — **one spelling per concept**: the English code spelling ↔ the words the Chinese pages use, the retired spellings and the false friends; prose always uses the code spelling.
- `graphics.md` — graphics / vsg live facts: mechanisms, criteria, traps and "why it was decided this way at the time".
- `core.md` — core facts: a type descriptor registers only through `Type::obtain()`, the two declaration forms (inline and out-of-line), and the export convention for interfaces.
- `crypto.md` — crypto facts: backend initialisation in one place, wolfSSL's `Md5`/`Sha256` macros colliding with `::Hmac`, incremental GCM buffering, test vectors and the commands.
- `geometry.md` — geometry facts: the SDK class export convention and the five primitives' local space.
- `appfw.md` — appfw cross-cutting rules (threading / no user code under a lock / lifetime).
- `async.md` — `src/base/async` facts.
- `base-runtime.md` — loader identity and retention, the typed cache seam and its outcomes, the detached process-lifetime sweeper, and the overwrite rule.
- `base-system.md` — `base/system` facts: one platform per file behind `SystemPlatform.hpp`, the CMake `HEADER_FILE_ONLY` rule, the sysfs / windows.h traps, and the per-platform contracts.
- `imaging.md` — imaging facts.
- `imageio.md` — imageio facts.
- `loaders.md` — the loaders layer index.
- `robotics-kinematics.md` — Robotics kinematics facts.
- `robotics-proximity.md` — Robotics proximity facts.

## bugs/ — bug records (44)

Symptom / root cause / status / files involved / verification, one file per bug; a fixed record keeps
its history marker, an unfixed one states its status.

- `vsg-embedded-init-crash.md`
- `vsg-content-address-recycled-before-rows.md`
- `spline-knotspan-hang.md` — `geom::detail::knotSpan()` made no progress on the "last two spans" bracket, so evaluating a B-spline with internal knots hung.
- `vsg-embedded-blank-render.md`
- `vsg-resize-distortion.md`
- `vsg-maximize-black-band.md`
- `shared-task-batch-resume-uaf.md`
- `base-runtime-sweeper-static-terminate.md` — the cache's sweeper was a joinable static `std::thread`, so a started cache with static storage duration aborted the process at exit; the state is now immortal and the worker detached.
- `docking-flyout-under-render.md`
- `docking-layout-transitions.md`
- `docking-tab-dangling-client.md`
- `docking-title-click-hides-pane.md`
- `model-viewer-fit-not-framing.md`
- `async-scope-reuse-after-stop.md`
- `async-runon-move-only-arguments.md`
- `async-queue-reentrant-pop-uaf.md`
- `async-cv-wait-cancel-lock.md`
- `async-tcs-multiple-value-consumers.md`
- `async-queue-nondefault-values.md`
- `async-withcancellation-token-propagation.md`
- `async-queue-noexcept-failures.md`
- `appfw-commandmanager-shutdown-drain-timeout.md`
- `appfw-commandmanager-exclusive-admission-race.md`
- `appfw-commandmanager-admission-exception-safety.md`
- `appfw-commandmanager-current-command-pointer-lifetime.md`
- `appfw-commandmanager-async-instance-lifetime.md`
- `appfw-commandmanager-disabled-preference-lost-update.md`
- `appfw-userio-destructor-pending-read.md`
- `appfw-progress-host-pointer-across-threads.md`
- `appfw-console-progress-reporter-sink-under-lock.md`
- `appfw-console-answer-in-history.md` — the console recorded every submitted line, so an answer to a prompt (a file path, a number) came back through Up as a command, and a matching prefix let completion rewrite the answer into a command name; `beginInput()` / `endInput()` now bound an explicit answering mode that history, completion and the accent all follow.
- `appfw-console-completion-refresh-off-thread.md` — `commandsChanged` fires on the thread that changed the command set, and the console's handler read the bound panel and the plugin display names there before marshalling only the final write; the handler now posts, carrying a liveness token.
- `appfw-console-rebind-leaves-read-waiting.md` — changing (or clearing) the bound console panel left a waiting read parked on a prompt drawn on the old panel; the binding change now releases it.
- `appfw-posted-message-outlives-application.md` — `reportToUser()` posted a borrowed `UserIO*`; the queued call now asks `Application::current()`, which is cleared before the I/O is released.
- `vsg-depth-bias-not-declared-dynamic.md`
- `graphics-renderengine-addpass-null-pass-crash.md`
- `graphics-scene-memo-key-missing-view-basis.md`
- `graphics-transparent-sort-uses-origin.md`
- `demo-chain-never-adopted.md` — the demo built a `PassChain` and never adopted it with `setChain`: the transparent scene the chain owns was never published (sky/overlay missing) and the chain's size could only be fixed up by the demo itself; after the fix the view maintains it in one place.
- `vk-screen-draw-inherits-pass-depth.md` — vk's full-screen draw inherited the pass's `TestAndWrite` depth policy, so against a target with depth / borrowed depth the whole screen pass was discarded (the window and the deferred composite went black with zero VUIDs); a full-screen call now always uses `Disabled`, with a regression case.
- `vk-screen-pass-shaded-without-the-map.md` — vk's full-screen set always bound the shadow white stand-in / zero block, so a deferred lighting program never saw the real shadow map (forward had shadows, deferred did not, with zero diagnostics); the screen path now uses the pass-resolved `ShadowSource`, with a regression case.
- `demo-preview-passes-shared-one-name.md` — the demo's four G-buffer previews shared the single name `gbuffer_preview`, and the plan matched rows by `(owner, name)`, so only the last one survived; a layout reshuffle then changed the objects without re-declaring them, so the rectangles never took effect. Fixed as one preview per name plus a re-declaration on the layout step.
- `vsg-teardown-frees-in-flight-resources.md` — the vsg backend freed resources still being read by a submission at teardown / frame-graph replacement (the source of the heap corruption); includes the valgrind evidence, the two fixed sites and the remaining prepare-only path.
- `vk-target-fresh-data-race.md` — vk's readback "no-wait refusal check" read the target record's fields outside any lock, racing the frame body's write of `fresh` (one TSan report); the record became a value snapshot taken under the table lock (`TargetTable::peek`) plus an atomic `fresh`, and the phase-two and frame-body read sites became atomic loads.

Note: `memory/` stays terse and fact-shaped; detailed design goes to `design/`; one file per bug.
