# graphics frame pipeline

> **Status**: current — the takeover is complete (§9.3), and the shapes below are the ones in use.
> **Last verified**: 2026-11-16
> **Scope**: the shape, identity, lifetime and plan semantics of the four layers content → snapshot → derivation → execution. It excludes the geometry / picking
> algorithms, the material model, backend implementation details (`vsg-backend.md`) and the threading contract's migration-era state (`graphics-threading.md`).
> **Sources**: `src/viz/graphics/sdk/vine/graphics/ContentId.hpp`, `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/PinnedResources.hpp`,
> `src/viz/graphics/src/SceneSnapshot.hpp` (the prototype), `Scene.hpp` / `RenderEngine.hpp` (the live entry points).
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

"when does the copy that crosses a layer expire" had five answers at once: address + version (the engine table), mirror + delta (`ContentMirror` /
`DeltaChannel`), memo + epoch (`DrawListMemo` / `Version`), reference counting (`markUsed`), and a parking window
(`RetirementQueue` / `kept`). The bridge and lifetime layers together were 2519 lines, about 60% of which answered the same question; two real incidents
(a mirror born inside a frame without a frame token, `refreshMaterials` lagging because it only touched publish) and one UAF
(`.ai/bugs/vsg-content-address-recycled-before-rows.md`) all happened in this layer.

### 1.2 Goals

1. Only **immutable values** cross a layer; mutable objects live on the content side only (`C1`).
2. Publication is **one atomic pointer swap**; version, mirror, delta and memo are not concepts (`R9`).
3. Identity is stable and decidable after death; residency is **computed** from "who still references it" (`I2` / `I4`).
4. A plan is a **declaration**, and misuse cannot be expressed (`R21` / `R38`).
5. Derivation is a **pure function**: parallel, reproducible, dumpable, replayable (`R14` / `R30`).

### 1.3 Non-goals

- No multi-threaded writing on the content side (`C2`); no GPU-driven / bindless (no second backend, no measured bottleneck).
- No automatic barrier / render graph in this layer (synchronisation belongs to the backend).
- No ABI stability promise; vsg's internals are not changed, only the shape of its interface with this layer.
- No promise that every `-design` algorithm (poses, the post-processing dependency graph) lands now: they follow the triggers in `../backlog/graphics.md`.

### 1.4 One-paragraph conclusion

> The content side publishes one immutable snapshot, the derivation layer turns `(snapshot, view)` into a value, the execution layer consumes values only and answers one;
> the four layers have one edge between each pair, and no edge's payload is a live object.

## 2. Design Points

### 2.1 Four layers, one one-way edge

- What it is: ① AUTHORING (mutable, one writer) → `publish` → ② SNAPSHOT (immutable value) → `derive` → ③ DERIVATION
  (pure values) → `execute` → ④ EXECUTION (device state) → `FrameResult` (a value) back to ①.
- Why: each of the four edges has one verb, and a layer sees only the value the layer above handed it ⇒ "who reads whose write" is enumerable (`G2`).
  The one back edge is a value too, because a query must aim at "the frame the user saw" (`I7`).
- Boundary: `execute` is the only verb in this layer that may touch the device; the backend's synchronisation, allocation and submission are not concepts of this layer (`G5`).

### 2.2 Eight nouns

| Noun | One definition | Owner | Lifetime |
|---|---|---|---|
| **Scene** | author tree + light set + writer role and publication point | content side | held by the host |
| **Node** | a **position** in the tree: placement + children + state contribution + payload | content side | a prototype may be built externally; the position is expressed by `Instance` |
| **Geometry / Material / Program / Texture** | reusable content values, each with an `XxxId` + `revision` | content side | reference counted |
| **Snapshot** | **this one publication** of the scene | publication layer (value) | referenced by in-flight frames and Sessions |
| **View** | camera value + viewport + layer mask (the projection kind is a value) | derivation layer (value) | within one derivation |
| **DrawList** | the ordered draw items of `(Snapshot, View)`, everything already resolved to values | derivation layer (value) | within one frame |
| **Session** | device + backend + handle tables + pipeline cache + arena + the single engine thread (the SDK has **no** type of that name: it means one engine's execution side) | execution layer | held by the host |
| **FrameResult** | present, timings, diagnostics, which Snapshot / View was presented | a value | read by the host |

- Why: one concept, one definition, one owner (`G1`). The **uniform trio** — handle (identity) · revision (the value changed) ·
  value snapshot (crosses layers) — is written once and is isomorphic in all eight places (`G6`).
- Boundary: the skeleton has **no** render world object, mirror, delta channel, memo + version, `addPass`/`removePass`,
  mode enum, manager or control. That is part of the design, not an omission.

### 2.3 Three verbs

| Verb | Meaning | Called by | Constraint |
|---|---|---|---|
| `publish` | turns mutable content into a new Snapshot and swaps the pointer atomically | the content thread | the one mutable → immutable transition (`I3`) |
| `derive` | `(Snapshot, View) → DrawList → FrameValue`, a pure function | any thread | same input, same output (`I5`) |
| `execute` | `FrameValue → device actions → FrameResult` | the engine thread | the only place that touches the device (`I6`) |

### 2.4 A node contributes four kinds of fact only

- What it is: `Node` is a **position**, not a kind, and contributes placement / children / state contribution / payload only; there is one folding rule:
  child = parent + self (placements multiply, state merges by documented rules, layers union, overrides in order).
- Why: folding is a value operation ⇒ thread-safe, unit-testable, fuzzable (`G11`); new node behaviour = one more placement value, without touching the folding core (`G9`).
- Boundary: `StateNode` / `MatrixTransform` / `ScreenPass` are not "kinds" in the **snapshot** but different values of one concept; the authoring-layer sugar
  stays. Hiding is a **snapshot representation** (it produces no rows); `Node::setVisible` stays and publication compiles it into structure (`R5`).

### 2.5 Identity: the registry and four kinds of id

| id | What it is | Created when | Unique within |
|---|---|---|---|
| `NodeId` | the identity of a **prototype** | construction | the content domain |
| `InstanceId` | **one position** in the tree (a prototype reference + local placement / overrides) | insertion | the content domain |
| `RowIndex` | a dense index inside a snapshot (the SoA subscript) | publication | one Snapshot |
| `PassId` / `TargetId` | a handle internal to a session | plan alignment | one engine (one engine, one surface, see `../decisions/ADR-2026-11-13-one-engine-one-surface.md`) |

- What it is: one `ContentRegistry` per content domain (slot allocation + generation); 64 bits = `generation:24 | domain:8 | index:32`;
  releasing changes the generation and a wrap retires the slot; **only the registry can mint an id** (`C4` / `C5`).
- Why: there is no Session yet when content is built externally, and sharing across windows must be the same id ⇒ the content domain is the scope (`G1`);
  a stale id answers gone and never dangles (`I2`).
- Boundary: an id is a **value** and the registry is the content side's ledger; the engine side **never resolves an id** — it looks a table up by index and
  validates with the revision.

### 2.6 Prototypes and instances: fold once

- What it is: the tree stores an `Instance` (`NodeId` + local placement / overrides); `rows(instance) = Placement(the instance chain) ∘
  PrototypeLocalRows(the prototype, revision)`; folding inside a prototype is computed once, and v1's materialised placement cost is N × the prototype's row count.
- Why: one prototype is referenced from any number of positions, across Scenes too (`R2`), and editing the prototype changes every instance; sharing local rows
  (option B) could compress static instances to `O(rows + N)`, but that waits for a measurement (`G8`).
- Boundary: prototype containment must be acyclic (checked at insertion, `R7`); the row budget is observable (instance count × prototype row count, one episode above
  the threshold, `R8`); the folding cache lives inside the Snapshot in v1.

### 2.7 Residency, pipelines and uploads

| Decision | Why | What it replaces |
|---|---|---|
| The engine table is indexed by **handle index** and validated with generation + revision | an array, no hashing; an old frame reads the old version | the lifetime negotiation of address + `markUsed` |
| The engine table holds **shared resources** only, never per-instance state | the table's size is prototypes × versions, not instances | touching a live object per command |
| Residency ⟺ an accepted or in-flight Snapshot references its `(id, revision)`; a snapshot the host holds itself pins nothing | an exact rule that does not look at a time window (`I4` / `R22`) | the `parking` / `kept` safety net |
| The pipeline cache key is a **value** (program@rev, layout@rev, state, formats, samples) | reproducible, shareable, unit-testable | a pointer + variant bits |
| A row's / command's payload is **values**: `GeometryDescription` / `MaterialDescription` / `ShaderProgramDescription` + `ContentRef`; lights are `LightValue` | the recording side reads no live object, and frame N draws the value it described with (`G1` / `G10`); each command saves 3 `intrusive_ptr` atomic increments / decrements | reading live objects (program text / material alpha mode / `Light` fields) |
| Preparation is a first-class entry point: `prepare()` answers "what is needed" | compile stalls become budgetable, and loading can precompile everything (`R23`); the batch of transfer-only work it may do itself lands here too (ADR-2026-11-14) | discovering at record time that something must compile, or must upload |
| A per-frame arena, zero allocation outside it during a frame | the allocation ceiling turns from a "rule" into a constant (`R24`) | a per-frame `vector` / `shared_ptr` |

### 2.8 A plan is a declaration

- What it is: `Pass = (name, View, Target, state overrides, order, inputs)`; `Target = Transient{name, shape} |
  Explicit{RenderTarget} | Window` (no target = draw into this area's default framebuffer); `inputs` refer by stable names.
  One engine drives one surface (several windows = several engines sharing one device, see `../decisions/ADR-2026-11-13-one-engine-one-surface.md`).
- Why: the host publishes "the frame graph I want" and the Session aligns it by name at a frame boundary ⇒ adding or removing a pass needs no add/remove call, and
  "changing the session's shape while a frame is in flight" **cannot be expressed** (`R21`; `G12`).
- Boundary: `PassId` exists only inside a Session; the difference is taken by **name**, not by pointer (`C7`); a transient target's lifetime is declared by the plan.
  The table that builds them belongs to the ENGINE, and the device it builds them on is borrowed and may outlive it - so a shutdown hands every live transient back
  (`FrameResources::releaseAll` → `releaseRenderTarget`) instead of dropping the table and leaving the device holding storage nothing will name again.
  A plan expresses only the shapes `PassKind` can express (`Scene` / `Screen`): a pass subclass is no longer a registrable shape, and customisation lives in the
  **data** (program override, viewport, depth range); a kind change rebuilds the built-in node. There is no `PassKind::Custom` and no factory registry (`G8`).

### 2.9 Derivation is a pure function

- What it is: `select(snapshot, view) → DrawList` is a pure function; `record` happens inside the backend's `execute`; the sort key is a
  documented total-order value (`draw_order, layer, opaque/transparent, quantised depth`).
- Why: several views in parallel, dumpable, replayable, comparable across backends (`R14` / `R30`); "why do these two frames differ" becomes a question that can be replayed.
- Boundary: view-dependent rows (billboard / screen space) are recomputed per view without changing the result of the view-independent rows (`R16`); if a backend records
  in parallel, the determinism of the merged result is that backend's own to prove (`R20`).

### 2.10 Where the five expiry answers went

| Before | Destination | Replaced by |
|---|---|---|
| address + version (the engine table) | deleted | handle + generation + revision (2.5 / 2.7) |
| mirror + delta (`ContentMirror` / `DeltaChannel`) | deleted | one `publish` swapping the pointer (`R9`) |
| memo + epoch (`DrawListMemo` / `Version`) | deleted | a Snapshot is a value and derivation is pure (`R14`) |
| reference counting (`markUsed`) | deleted | row-level epoch reclamation (`F5`) |
| parking window (`parked` / `kept`) | half kept | GPU objects are still replaced (pipelines / descriptor sets / buffers), but **residency** no longer depends on it (`I4`) |

### 2.11 Five `Frame*` values, five questions

| Value | The one question it answers | Lifetime | Who reads it |
|---|---|---|---|
| `FrameDescription` | What THIS frame is: every enabled pass' identity, order, resolved inputs and what the pass described | rebuilt every frame; the backend consumes it and keeps nothing | `RenderFrameExecutor::executeFrame`, `RenderEngine::frameDescription()` |
| `FrameTable` | What this frame consists of, as rows a host may read | rebuilt every frame from what the slots hold | `RenderEngine::frameTable()`, a log line, a HUD |
| `FrameOutcome` | What ONE `frame()` call did | one call | the call site |
| `FrameResult` | What the last ANSWERED frame was: its picture, the world it saw, what it cost | replaced when the next frame is answered | `RenderEngine::frameResult()` |
| `FrameQuery` | Which world that picture saw: the publication it drew and the view it was taken with | the `query` field of one `FrameResult` | a pick / anchor / fit aimed at what the viewer sees |

- What it is: four values and one field type. `FrameQuery` is not a sixth answer - it is `FrameResult::query`'s type, split out because the two facts it carries (publication, view) are always asked for together.
- Why: a value that answers two questions goes stale in one half, and "which publication did the picture come from" used to be a separate question with a separate lifetime (`G1` / `I7`).
- Boundary: `FrameOutcome`'s `NotReady` is a precondition answer, not a third frame ending (`RenderEngine.hpp` says so at the enum); a host that needs the answered frame reads `FrameResult`, never `FrameOutcome`.

### 2.12 The engine is a frame compiler, not a render graph

- What it is: a pass declares `(target, state, inputs)`; compilation resolves the input names, sorts, allocates transients and emits `FrameDescription`; the backend records that value. No layer derives resource dependencies, inserts barriers, aliases memory, or reorders passes to that effect.
- Why: today's consumers do not need one - a chain is a handful of passes whose order its recipe states, transient sharing is by declared identity + shape (2.8), and barriers are the backend's own table (`vk-backend.md` 2.7). A render graph's mechanism (implicit resource lifetimes, automatic barriers, aliasing) would be a second place deciding what the picture is, with no witness asking for it (`G8`); synchronisation stays where it belongs - behind the seam (`G2`).
- Boundary: `Transient` borrows a render graph WORD, not its semantics: it names "the shape of canvas this pass needs", the compile shares it by name + shape, and no read/write edge is expressed. Wiring is exactly the input-name system (2.8 / F6). "Pass", "target" and "transient" therefore mean less here than in UE's RDG, Frostbite's FrameGraph or O3DE's pass tree - deliberately.
- Revisit when a real consumer needs resource lifetimes DERIVED rather than declared (a pass count at which hand-stated order and sharing stop being readable). The reader-facing version of this boundary is `docs/guides/graphics.md` §5.5.

### 2.13 The frame carries its context, and one clock measures it

- What it is: `FrameContext` - the frame's id, the interval since the previous frame, the announced surface size in logical pixels and the ratio - is composed by the ENGINE (`RenderEngine::frameContext()`), handed to the per-frame work (`FrameUpdate::onFrame(const FrameContext&)` - a REACTION, so it carries the `on<Event>` spelling of `graphics-architecture.md` 2.14), and forwarded unchanged by `SceneView::update` / `PassChain::update` (DRIVERS: their job is to walk and call). One value per frame, so every object doing per-frame work in that frame sees the same frame.
- Why: each object used to derive what it needed on its own, and the only time-dependent one (`FpsOverlay`) read `steady_clock` itself. That is N clocks for one frame, and a gizmo and a readout could window the same frame differently (`G1` / `G11`). A frame also stops being a function of "the declarations plus whatever the wall clock said to each object" - the host does not have to know anything to ask for its context, and two consecutive reads answer the same value.
- Boundary: the value is a **read**, and the clock advances exactly once per frame, at the head of `frame()` after the readiness checks - so `elapsed_seconds` is one frame BEHIND the frame being prepared, which is what "how fast is this loop running" means and the price of keeping `frameContext()` free of side effects (an accessor that mutated would violate the accessor/command rule). The interval is WALL-CLOCK: a host that needs a fixed step or a replay keeps its own clock and reads the other fields, which are exact. Sizes stay in the units the host announced, and the device-pixel answer uses the engine's one rounding rule (`std::lround(logical * ratio)`, the same call `EngineCompile` lays a frame out with).
- Added 2026-11-18 on an explicit go, ahead of its trigger (the backlog's criterion was "the first feature needing two of the fields"): the argument and the rejected shapes (`update(double dt)`, a host-supplied clock, a fourth narrow lease on the engine) are `../decisions/ADR-2026-11-18-the-frame-carries-its-context.md`; the migration note is `BC-25`, which covers both halves: every caller of `SceneView::update` / `PassChain::update` passes `engine->frameContext()`, and every implementer of the hook renames `update()` to `onFrame()` (the reaction verb of `graphics-architecture.md` 2.14 - the drivers keep `update`).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Snapshot publication | a copy-on-write row table in chunks + material values + a spatial index + the light set, swapped in one pointer exchange | landed: `ScenePublisher::publish` compiles a `detail::SceneSnapshot`, and frame compilation makes `SnapshotSource` answer each draw through `PublicationStore::sourceFor` (`EngineCompile.cpp`); an unpublished scene is refused with `engine.compile.unpublished_scene` and the live tree is not read back |
| F2 | Pure derivation `select` | `selectSnapshot(snapshot, view)` equals the live collection item by item | landed: each view's draw list is the one `selectSnapshot` run `SnapshotSource` performs (`ScenePublisher.cpp`: there is no second walk over live rows); `T8` proves it pure (serial / concurrent / after the content is cleared, the same result) |
| F3 | Content identity and generation | `Handle` + typed id + `ContentRef`; a stale handle answers gone | landed |
| F4 | The engine table keeps residency by `(id, revision)` | the fact tables get a row per ref; an old frame reads the old version | landed |
| F5 | Row-level epoch reclamation | a row is reclaimed at "the last frame a plan hit it"; an entry leaves when "the host does not want it ∧ no row names it" | landed |
| F6 | A plan is a value | a by-name difference at a frame boundary + plan validation with stable codes | landed: `Plan` + `validatePlan` + `applyPlan` (session and scope forms, `PlanResult` three states) + `plan()` / `planFor(owner)`; `Target` is still an explicit `RenderTarget` handle |
| F7 | Preparation entry point | `prepare()` answers what is needed; after it, recording's `builds` delta is 0 | landed: `drawNeeds` is the only key builder; `RenderEngine::prepare()` → `prepareFrame(value)` pre-builds halves / sets / uploads / pipeline identity from the same answer |
| F9 | `publish` returns a result | an explicit "not published" inside a batch, no null | landed: the public `Scene::publish()` has three states (`Posted`/`Unchanged`/`InsideBatch`); the old four-state `PublishOutcome` is gone |
| F10 | Prototypes and instances | `Instance` references a prototype; fold once; acyclicity and the row budget | not done |
| F11 | Total-order sort key | a documented total-order value (layer included), two transparent segments | landed: `detail::SortKey` (drawOrder → layer → opaque/transparent segment → quantised camera depth → row/primitive index); the representative point is the world box centre |
| F12 | Frame result and query | `FrameResult` carries snapshot / view / poses / diagnostics / present | partial: `FrameResult` + `FrameQuery` (snapshot / view / passes / draws / diagnostics / present) have landed; `poses` waits on F10 |

### 3.2 F1 Snapshot publication

- **Behaviour**: `publish` turns the mutable scene into one immutable Snapshot: the row table is copied on write in 32-row chunks (only changed chunks are touched),
  material values are taken by `(id, revision)`, and the spatial index and light set are published together with the rows.
- **What "changed" means for a chunk** (`G11`): two questions, because the two kinds of change are invisible to each other. The row's node **content epoch** answers "did this
  node's own facts announce" — and an edit bumps the edited node's and its ANCESTORS' counters, never its descendants'. The row's **fold serial** (`Row::resolved_serial`,
  moved only by the one function that stores the fold's answer) answers "did the fold write something else into this row" — which is what an edit does to the whole subtree
  below it. A chunk is shared only when no row in it is new on either count.
- **Constraints** (`C1` / `C2`): anything the content side changes after publication leaves in-flight frames untouched.
- **Requirements** (`R9` / `R10` / `R13`): one atomic pointer swap; cost O(changed chunks + changed material values); the snapshot can be exported as a text summary.
- **Acceptance** (`T1` / `T2` / `T3`): `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` (≤ 250 µs, RelWithDebInfo);
  the in-flight frame is unchanged item by item.
- **Caveat** (`P2`): the metadata table (`id → revision`) must be shared while it is unchanged, or `K11` exposes a whole-table copy.
- **Caveat** (measured 2026-11-19): the edit path shares; a table REBUILT from scratch (a child added or removed, a state or geometry change) reports every row as moved, because a
  fresh row cannot say "the fold produced the same answer as the last publication". That is the conservative half of the rule: a full rebuild copies all chunks, an incremental
  edit copies the ones it touched. Restoring sharing across a rebuild would need the previous publication's per-row serials carried into the new table, and nothing asks for it yet (`G8`).

### 3.3 F2 Pure derivation

- **Behaviour**: `select(snapshot, view) → DrawList`; the view-independent fold is computed once per frame, and each view pays only culling and sorting.
- **A row is three parts** (`G1` / `G4` / `G12`): `detail::RowLinks` (where the row sits in the walk-ordered table: parent, subtree end, child count),
  `detail::RowOwn` (what the node states, as two named groups - `Fold`: placement, opacity, layers, draw order, view placement; `Body`: state
  contribution, material, program, own bounds, geometry, draws) and `detail::ResolvedRow` (what the fold derived). `RowOwn` is the edit path's
  classification, not decoration: a refresh whose `Body` is unchanged re-derives the row's subtree in place (`RowRefresh::RefoldSubtree`), while a
  `Body` change is `RowRefresh::RebuildTable`. Both groups compare as a whole (defaulted equality), so the classification carries no hand-written field list.
- **The select is one call** (`G4`): `detail::selectRows` (one template over a row accessor, so the flat `SceneBake` and the chunked
  `SceneSnapshot` run the same algorithm) returns a `detail::Selection` - the commands already in draw order (`sortCollected` runs inside) and
  `viewport_used` for the memo's key.
- **The fold's one rule** (`G1` / `G12`): a row stores what the fold derived for it in one value (`detail::ResolvedRow`), and one function stores it
  (`storeResolved`, which moves the row's serial only when the answer changed). What a row hands its children is that value — including the state, material and program it
  inherited — so a re-fold below it starts from what the whole chain above says, whether or not the row itself draws. Storing only for drawing rows is precisely how an edited
  subtree used to lose the state, material and program its ancestors contributed.
- **The edit path** re-reads the rows whose epoch moved (`refreshRow`), then derives the edited row's SUBTREE, then one row per ancestor: a box is a union, so the chain
  above a moved subtree has to be recomputed or a cull prunes content that moved into view.
- **Constraints** (`C8`): value types that cross a layer carry no virtual functions or inheritance.
- **Requirements** (`R14` / `R15` / `R16`): the same input gives the same items; the fold count is 1; view-dependent rows are recomputed per view.
- **Acceptance** (`T8` / `T9` / `T24`): `SelectTest.SameInputSameList` (the same snapshot repeated, other views interleaved, after the scene is cleared, and 8 threads concurrently — item by item equal to serial), `SelectTest.TheSortKeyIsATotalOrder`, `SelectTest.OneChunkCullsEachRowOnItsOwnBox` (a chunk is the unit of sharing, not the unit of culling); `SnapshotTest.AnEditRefoldsEveryDescendantAcrossChunkBoundaries` and `SnapshotTest.AnEditBelowAStateNodeKeepsWhatTheStateNodeContributes` for the edit path, computed from the edit rather than from the snapshot.
- **Caveat** (`P5`): a pure function is not free — parallelism happens on pure values only (`I6`), and the device side is still one thread.

### 3.4 F3 Content identity

- **Behaviour**: a `Node` has its `NodeId` at construction; `Material` / `Program` / `Texture` each have a typed id; `ContentRef{id, revision}` is the cross-layer key.
- **Constraints** (`C4` / `C5`): only the registry can mint an id; a generation wrap retires the slot.
- **Requirements** (`R1` / `R31` / `R32`): identity is independent of being in a tree; a reference carries a revision; a query returns a position identity.
- **Acceptance** (`T5` / `T17`): `ContentRegistryTest.AStaleHandleAnswersGone` (implemented); an item-by-item diff of the old frame.
- **Caveat** (`P1`): **a reference that carries only an id is not an immutable value** — it pushes the version question to the engine table and breaks `R9` outright.

### 3.5 F4 The engine table and F5 row-level reclamation

- **Behaviour**: the fact tables (geometry / material / program) get a row per `(id, revision)`; a row is reclaimed at "the last frame a plan hit it", and an
  entry's arena / upload / texture residency is answered by the declaration index.
- **Constraints** (`C3`): only the engine thread builds and reclaims tables.
- **Requirements** (`R22` / `R23`): residency ⟺ a live snapshot reference; predictable after preparation.
- **Acceptance** (`T6` / `T25`): `ResidencyTableTest.TheCollectableSetIsEnumerable`;
  holding an old snapshot outside the Session and submitting again does not UAF.
- **Caveat** (`P6`): a row's lifetime and a GPU resource's lifetime are not the same number — a row may leave a frame later than the resource (an in-flight plan still reads it).

### 3.6 F6 A plan is a value

- **Behaviour**: `Plan` is a value (an ordered `PassDesc` list; a pass' **name** is its identity inside the plan);
  `applyPlan(plan)` takes the difference by name: a new name is added, a same-named one is updated **in place** (the node and the backend state survive), a name that
  disappeared is removed; `plan()` reads the current shape back into the same type (`applyPlan(plan())` is idempotent). Validation yields stable `PlanProblemCode`s
  (`EmptyPassName` / `DuplicatePassName` / `UnknownInput` / `DependencyCycle` / `TooManyPasses`) and accepts the whole plan or rejects the whole plan. The return value is
  the three-state `PlanResult`: `Applied` / `Invalid` + a problem list / `Refused` + `PlanRefusal` (`Busy` = a frame is recording in the engine, `EngineGone` = the
  `PassRegistry`'s engine is gone). A kind change rebuilds the built-in node; a plan expresses `PassKind` only, and a pass subclass is no longer an expressible shape.
- **Constraints** (`C7`): by name, not by pointer (in the scope form it is `(owner, name)`).
- **Requirements** (`R21` / `R38`): adding or removing a pass calls no add/remove; each validation has one code; while a frame is recording the answer is
  `Refused{Busy}` **before validation** (shape-related state is not read), not a repair afterwards. `SessionBusy` is not a plan problem code — it is a runtime
  precondition, not a shape defect.
- **Acceptance** (`T7` / `T30`): `PlanTest.APassIsAddedWithoutAnyAddCall`, `PlanTest.AChangedTargetUpdatesTheSameNodeInPlace` (`P7`),
  `PlanTest.*IsRefusedWithItsCode` (the four kinds of code), `PlanTest.TheCodeSpellingsAreStable`,
  `PlanTest.ABusyEngineRefusesThePlanInsteadOfBlamingItsShape`.
- **Caveat** (`P7`): the difference must also hold for **same name, different content** (changing a pass' target is not "remove one, add one").
- **Who waits, and where**: the refusal is the engine's, so the party that changes a shape between frames does the waiting. `RenderControl`/`SurfaceWindow` do it before an attach, `SceneView::applySurfaceGeometry` does it before it hands a surface size to the chain (a chain RESIZE restates the passes it re-lays out, so a resize is a session change like any other), and a host that states its own rows does it before `applyPlan`. The wait is bounded (250 ms) on the application thread: a frame that never answers must not freeze the UI, and the engine's own default is five seconds. Pinned by `RenderEngineTest.AViewResizeWaitsForTheFrameBeforeItRestatesItsChain`.
  A pass the plan does not name is removed (whole-session form; the scope form only upserts) — that is the direct consequence of "the plan is the shape".
  A pass subclass is no longer an expressible shape: customisation lives in the **data** (program override, viewport, depth range), and there is no custom-kind registry.

### 3.7 F7 Preparation entry point

- **Behaviour**: `prepare()` answers "which halves / descriptor sets / uploads this frame needs" and **builds them all**, without submitting a frame.
  The public entry point is `RenderEngine::prepare()` (the engine compiles a `FrameDescription` first, the same compilation `frame()` uses); the backend entry point is
  `RenderFrameExecutor::prepareFrame(value, present_to)` — one line, two verbs, no new nouns; the answer is a value (`PrepareReport`: the outcome + how many passes /
  draws it walked + how many halves / sets / uploads / pipeline identities this call built). The snapshot entering that input is the one §3.2 publishes.
- **Constraints** (`C3`): preparation runs on the engine thread; it submits no **frame** and acquires no swap-chain image (see `P11`).
  It may record and submit **one** command buffer for its own **transfer-only** work (texture copies, the 1×1 stand-in) and wait for it before returning —
  that batch has no frame, no render pass, no draw and no present, so `FrameProgress`'s `submitted` / `presented` do not move
  (`D13` / `../decisions/ADR-2026-11-14-prepare-may-submit-transfers.md`).
- **Requirements** (`R23` / `R36`): a frame recorded after preparation shows a `builds` delta of 0 (both of `FrameUploads`' kinds, streams and textures); a missing optional capability must appear in the report.
- **Acceptance** (`T7` / `T10` / `T32` / `T33` / `T35`): the `builds` delta assertions; the key set `prepare()` reports equals the key set this frame actually builds;
  the reported `uploads` cover both streams and textures.
- **Caveat** (`P8` / `P11`): preparation is a **query**, not "compile everything"; and it **cannot** be implemented as "run a frame without submitting" (see `P11`).
  The transfer batch is not a variant of that rejected shape: it must be "the copies the frame would have recorded before its first render pass",
  and no copies means no submission (preparing the same frame a second time submits nothing).
  **The seam**: `drawNeeds(compiled pass, draw)` is the only key builder — `prepare` builds with it and `ContentPass::record` looks up with it, so the two cannot drift (`G1`).
  **Where it lives**: `src/plugins/gfx_backend_vsg/{include/vine/backend_vsg/internal,src/internal}/DrawNeeds.{hpp,cpp}`
  (`drawNeeds`' two overloads + `DrawNeeds::keyFor`); `CompiledPrograms::halvesFor`, `ContentPass::recordCommand` /
  `recordScreenDraw` and `describeMissingContentHalf` read it and nothing else, so the recording path no longer assembles its own key.
  **Where it lives**: `sdk/vine/graphics/PrepareReport.hpp`, `RenderFrameExecutor::prepareFrame`,
  `RenderEngine::prepare()`, `VsgBackend::prepareFrame`. The backend reuses the frame's own two halves — `collectFrame` (value → collection) and
  `recordContent(..., /*prepare_only*/)` (collection → halves / sets / sampled input sets / uploads) — and the two verbs have one branch point inside `ContentPass`
  (`Mode::Prepare` stops at "identity ensured": it builds no command graph and writes no block). **Preparation writes no block and opens no frame ring**:
  `FrameRing`'s slab rotation belongs to frames, and letting preparation take one extra slot would make the next frame write into the slab the oldest in-flight frame still reads
  (that extra slot in `core::FrameRing` exists for exactly this) — `T33` pins this with `BlockStorage::writes()` / `frames()`.
  It does not call `Session::beginFrame()` (a window would acquire an image); it reads the shape from `WindowTarget::facts()` instead.
  `growBlockStorageIfNeeded` / `growMaterialArenaIfNeeded` run inside preparation, so the first frame does not have to swap storage and repoint every set it just built.

### 3.8 Several windows: one engine drives one surface

- **Behaviour**: `RenderEngine::createArea()` creates its **one and only** area; the frame's default framebuffer is that area's target (with no area it is the
  device's own target, see `initialize`). Several windows = **one `RenderDevice` serving several engines**, one engine / one area / one `Viewer` per window.
- **Constraints** (`I6` / `R27`): only one engine thread touches the device, so one device does not have two engine threads — it serves several **engines**,
  not several areas of one engine.
- **Requirements** (`R21` / `R27`): adding or removing a pass calls no add/remove; one frame has one surface, so `FrameResult.present` /
  `PresentRecord` is a single value.
- **Acceptance**: `RenderAreaTest.ASecondAreaIsAConstructionFailureNotASecondObject` (a second area is refused),
  `HeadlessBackendTest.OneDeviceServesSeveralAreasWithoutCrossingThem` (one device, two engines).
- **Caveat** (`P4`): a surface event is applied at the frame head only; the swap chain is rebuilt only inside its own engine's frame.
- **Rejected**: "one engine driving several Surfaces" (graphics-next F8 / `R37` / `T29` / `K10`) — no consumer, `FrameResult` would change shape a second time, and appfw ownership would have to be rewritten; see §7 D12 and `../decisions/ADR-2026-11-13-one-engine-one-surface.md`.

### 3.9 F9 `publish` returns a result

- **Behaviour**: one publication answers one of three states (`Posted` / `Unchanged` / `InsideBatch`) together with "the one that is the answer after this call"
  (`ScenePublishResult::snapshot`); `InsideBatch` is an explicit "not published" inside a batch, and the answer is still the one from the batch's start.
  The frame after the outermost `endEdit` publishes the whole batch, and `publishCount` advances by 1 only. There is no `Refused`: publication has nowhere to be
  refused — it does not queue.
- **Constraints** (`C2`): one Scene has one writer at a time.
- **Requirements** (`R11` / `R39`): `publishCount` advances by 1 only; there is no null, and no single `bool` that means both "unchanged" and "inside a batch".
- **Acceptance** (`T3` / `T31`): `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` (the three states + the handle's revision walk),
  `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture`.
- **Caveat** (`P9`): a batch is not a transaction — it only delimits "what one frame can see". The public `Scene::publish()` arrived with the snapshot: before that, the public face
  had no type able to carry "the previous snapshot", and publishing it early would only have added a version number nobody could use.

### 3.10 F10 Prototypes and instances

- **Behaviour**: an `Instance` references a prototype; the assembled DAG is flattened into the row tree at publication; folding inside a prototype is computed once.
- **Constraints** (`C6`): prototype containment must be acyclic.
- **Requirements** (`R2` / `R3` / `R4` / `R7` / `R8` / `R33` / `R34`): many positions referencing one prototype, fold once, a stated lifetime, acyclicity, the row budget,
  one writer domain, and no dangling `NodeId` entering a publication.
- **Acceptance** (`T4` / `T21` / `T22` / `T23`): the fold-count case; publishing a shared prototype; instances surviving the prototype's destruction; the instantiation cost and `K11`.
- **Caveat** (`P3`): v1's placement cost is N × the prototype's row count, not N — do not write it up as "instancing is free".
  The A/B procedure is `T23`'s; the recorded numbers are in `../memory/graphics.md`, and B becomes the default only once they meet the line.

### 3.11 F11 / F12 The sort key and the frame result

- **Behaviour**: the sort key is `detail::SortKey` (`DrawListBake.hpp`) — a documented total-order value: `draw_order` → layer (the smallest layer number) →
  opaque/transparent → quantised camera-space depth → row/primitive index (the `tie`). Two segments, opaque and transparent; the representative point is the **world box
  centre** (no box falls back to the matrix origin), and the depth is the `view`-transformed -z quantised to 1/4096 world units.
  `FrameResult` (`FrameResult.hpp`) carries which Snapshot / camera value was presented, the pass / draw counts, the diagnostics delta and the present;
  `RenderEngine::frameResult()` is written once when the frame is answered, and `query` is the value picking / anchors / fit must aim at.
- **Constraints** (`C8`): the key and the result are values, comparable and copyable, carrying no live object.
- **Requirements** (`R17` / `R18` / `R19` / `R30`): the order is identical across machines (quantisation + the total-order tie); a query takes `FrameResult.query`; it returns a position identity.
- **Acceptance** (`T9` / `T36`): `SelectTest.TheSortKeyIsATotalOrder` (any permutation gives the same result) and `SelectTest.TheKey*`;
  `RenderEngineTest.TheFrameResultIsThePictureThatWasAnswered`. `T18`–`T20` (several instances / poses / picking with the frame camera) wait on F10.
- **Caveat** (`P10`): the View inside `query` is **that frame's** value; querying with the live camera aims at the wrong frame — it is not "refreshing".

## 4. Public Interface

> Each signature is the contract; once it lands, the header is the authority.
>
> **The migration APIs are closed**: `applyPlan` / `plan` / `bindSnapshot` / `prepare` are host contracts;
> `Plan` is the only pass shape, `FrameTable` is the compiled frame table, `frame()` answers Ran / Submitted only, and `applyPlan` has three states.
> `bindSnapshot` is still an **override** (a frame takes `Scene::publishedSnapshot()` by default), and the production path is
> `SceneView::publishContent` + `applyPlan`.

### 4.1 The content side

| Interface | Contract | Notes |
|---|---|---|
| `Scene::publish() → Result<SnapshotRef>` | publishes one snapshot; inside a batch it answers an explicit "not published" | the contract signature, see `Scene.hpp` |
| `Scene::beginEdit()` / `endEdit()` | the batch boundaries; only the outermost releases a publication | exists |
| `Node::id() → NodeId` | valid from construction | exists |
| `Node::setVisible(bool)` | hiding is compiled by publication into "no rows" | exists |
| `ContentRegistry::allocate<Tag>() / release(handle) / alive(handle)` | the only source of ids; a stale handle answers gone | exists (`ContentId.hpp`) |
| `ContentRef{Handle id, uint64 revision}` | the cross-layer key; `contentRefOf()` is the only construction | exists |
| `Geometry::description()` / `MaterialValues::description()` | immutable values cached by revision | exists |
| `ShaderProgram::description() → ShaderProgramDescriptionPtr` | program text + id + revision; cached by revision | landed |
| `Light::value() → LightValue` | the light's value half; `Scene::lightSet()` assembles them into **one immutable set** (`LightSet`) and shares it into `DrawCall::light_values` when a pass describes itself (one set per frame, not one `vector` per pass) | landed |

### 4.2 The derivation side

| Interface | Contract | Notes |
|---|---|---|
| `detail::selectSnapshot(const SceneSnapshot&, const View&)` | a pure function over a publication; answers a `detail::Selection` (commands in draw order + `viewport_used`) | exists (`SceneSnapshot.hpp`) |
| the sort key | a total-order value + two segments; the comparison rules are documented | the target, see §3.11 |

### 4.3 The session side

| Interface | Contract | Notes |
|---|---|---|
| `RenderEngine::prepare()` → `PrepareReport` | builds without submitting; answers the halves/sets/uploads needed (`NotReady` + `EngineNotReadyReason`) | landed (the backend gets the same `FrameDescription` `executeFrame()` gets) |
| `RenderEngine::applyPlan() → PlanResult` / `plan()` | three states: `Applied` / `Invalid` + `problems` / `Refused` + `PlanRefusal` | landed |
| `RenderEngine::frame() → FrameOutcome` | `Ran` / `Submitted`; `NotReady` + `EngineNotReadyReason` | landed |
| `RenderEngine::frameResult() → FrameResult` | every fact of the previous frame when it was answered: `query` (snapshot / view) + passes / draws / diagnostics / present | landed (`poses` waits on F10) |
| `RenderEngine::pass(name)` / `passFor(name, owner)` | reads a registered pass node back by name (`owner` is the scope's contributor) | landed |
| `Session::frame(FrameRequest) → FrameResult` | submits one frame; `FrameRequest` carries a `View` (one surface per frame) | the target (`executeFrame` + `presentWindow` today) |
| `FrameResult{ frame, present, query, passes, draws, diagnostics }` | the value the host reads (`query` = snapshot + view) | landed (F12; `poses` waits on F10) |
| `Plan` / `Pass` / `Target` | the frame graph declaration (the only shape; `FrameTable` is the compiled frame table) | landed |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Everything that crosses a layer is immutable; mutable objects live on the content side only. |
| C2 | One Scene has one writer at a time; handing the writer role over needs a happens-before. |
| C2b | A prototype and every Scene referencing it belong to one writer domain; sharing a mutable prototype across threads is not supported. |
| C3 | One engine thread touches the device; parallelism happens in the derivation layer only. |
| C4 | Identity is minted by the registry only, with no second entry point. |
| C5 | When `generation` wraps, the slot is retired, not reused. |
| C6 | Prototype containment is acyclic. |
| C7 | A plan is diffed by stable names, not by pointers. |
| C8 | A value type that crosses a layer must carry no virtual functions or inheritance. |
| C9 | Diagnostics go through one route, with episodes. |
| C10 | No new third-party dependency. |
| C11 | The public API only grows during migration; it does not change. |
| C12 | Every step must be killable on its own (thresholds in §8 and ADR-2026-11-12). |

### 5.2 Requirements (R#)

| # | Requirement | Criterion |
|---|---|---|
| R1 | a `Node`'s identity exists at construction | a `NodeId` is available right after construction, the same before and after insertion |
| R2 | a prototype can be referenced from any number of positions | the same `NodeId` in two places; editing the prototype changes both |
| R3 | folding inside a prototype is computed once; v1's placement cost is N × the prototype's row count | `T4` counts them separately |
| R4 | the tree stores positions, not node bodies; changing the parent does not change identity | `T22` |
| R5 | hiding = no rows | a hidden subtree has no rows; the authoring API is unchanged |
| R6 | the four kinds of fact are values; folding is a value operation | the folding function has no side effects |
| R7 | prototype containment is acyclic | self-containment is refused and reported |
| R8 | the row budget is observable | one episode above the budget |
| R9 | publication is one atomic pointer swap; an old frame reads old values | `T2` / `T17` |
| R10 | publication costs O(changed chunks + material values) | `T1` / `N2` / `K11` |
| R11 | nothing is published inside a batch; one batch = one publication | `T3` / `T31` |
| R12 | a material change needs no notification from the caller | compared by `id → revision` at publication |
| R13 | a snapshot can be exported as a text summary | one command prints it and can be asserted on |
| R14 | `select` is a pure function | `T8` |
| R15 | the view-independent fold runs once per frame | the `T8` count |
| R16 | view-dependent rows are recomputed per view | the mixed case |
| R17 | the sort key is a documented total-order value | `T9` |
| R18 | a query takes `FrameResult.query` | `T19` / `T20` |
| R19 | picking returns a position identity, no pointers | `T18` |
| R20 | a backend recording in parallel yields a deterministic merged result | parallel = serial item by item |
| R21 | the session shape is a declaration diffed by name; one engine, one surface (several windows = several engines sharing a device) | `T7` |
| R22 | residency ⟺ a live snapshot reference; a snapshot the host holds itself pins nothing | `T25` |
| R23 | the pipeline key is a value; preparation is a first-class entry point | after preparation the `builds` delta is 0 |
| R24 | zero allocation outside the frame arena during a frame | allocation counts |
| R25 | capabilities are required or optional, negotiated at creation and reported | `T10` |
| R26 | shutdown has an explicit phase and is reentrant | `T11` |
| R27 | only one engine thread touches the device; one device serves several engines and one engine drives one surface | a second area is refused + `OneDeviceServesSeveralAreasWithoutCrossingThem` |
| R28 | diagnostics carry stable codes and structured fields | asserted by code |
| R29 | units and colour space are types / values | passing the wrong one does not compile |
| R30 | a frame value replays into the same command sequence | `T12` |
| R31 | a content reference carries a revision | `T17` / `K7` |
| R32 | the identity picking / diagnostics report is a position | `T18` |
| R33 | a shared prototype belongs to one writer domain; publication notices a prototype revision change | `T21` / `K8` |
| R34 | an `Instance` is responsible for its prototype's liveness; no dangling `NodeId` | `T22` |
| R35 | `PoseSet` = an instance's local overrides + subtree propagation | `T26–T28` / `K9` |
| R36 | preparation is a first-class session entry point; a missing optional capability appears in the report | `T7` / `T10` |
| R38 | plan validation has stable codes | `T30` |
| R39 | `publish` returns no null inside a batch | `T31` |

### 5.3 Invariants (I#) and measurable thresholds (N#)

| # | Invariant | Which class of bug it kills |
|---|---|---|
| I1 | everything that crosses a layer is an immutable value; mutable objects live on the content side only | a live object crossing a frame |
| I2 | every handle carries a generation; a stale handle answers "gone" | UAF / address reuse |
| I3 | one Scene has one writer at a time; publication is one pointer swap | a leak mid-batch, concurrent writes |
| I4 | a GPU resource lives ⟺ an accepted or in-flight Snapshot references it | guessing residency, the `kept` safety net |
| I5 | derivation is pure: same input ⇒ the same DrawList item by item | "why do these two frames differ" |
| I6 | only one engine thread touches the device | races at the device layer |
| I7 | a screen-space query takes the View value inside `FrameResult` | aiming at the wrong frame |

| # | Threshold | How it is measured |
|---|---|---|
| N1 | collect with a moving camera ≤ today's 2160 µs × 1.2 | `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` |
| N2 | publishing a one-row edit ≤ 250 µs @ 2000 rows | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` |
| N3 | a resident snapshot ≤ 2× the row table's bytes | `alloc_trace.c` / RSS |
| N4 | TSan reports 0 warnings | `scripts/tsan_check.sh` |
| N5 | semantic-assertion changes in existing cases ≤ 5% | counted directly |
| N6 | 0 allocations outside the frame arena during a frame | allocation counts; the frame-head storage reuse is pinned first: one shared light set per `Scene::lightSet` (`ValueChannelTest.EveryPassOfOneFrameHoldsTheSameLightSetUntilALightMoves`) + vk's `VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage` (proves the head's storage only, and does not claim a whole frame allocates nothing) |
| N7 | header dependencies do not grow | `check_include_hygiene.py` |
| N8 | one code base for Windows and Linux | CI |
| N9 | no third-party dependency | review + `_vcpkg.json` |
| N10 | a per-frame printable summary costing < 0.1% | timing |

## 6. Caveats and Traps

### 6.1 P1 A reference that carries only an id

- How it fails: an old frame resolves the id to a new value, the picture is half old and half new, and nothing refuses it.
- How to detect it: `T17` diffs an in-flight frame's commands item by item.
- How to prevent it: the key must carry a revision (`R31`); `ContentRef` is the only construction.

### 6.2 P2 The metadata table is copied whole

- How it fails: the row CoW is pretty, but publication cost grows linearly with the row count and `K11` fails.
- How to detect it: the publish time and bytes of one edit over 5000 prototypes.
- How to prevent it: share `shared_ptr<const Table>` while the table is unchanged, and replace only the affected tables when it changes (`R10`).

### 6.3 P3 Treating instancing as free

- How it fails: a 5000-row prototype inserted 10 times is written up as 10 folds, but it is 50000 placement multiplications.
- How to detect it: `T4` counts "fold count" and "placement multiplications" separately.
- How to prevent it: v1 states N × the prototype's row count explicitly; option B (sharing local rows) waits for `T23`'s measurement.

### 6.4 P4 A surface event inside a frame

- How it fails: the swap chain is rebuilt mid-recording and in-flight commands reference destroyed views.
- How to detect it: the several-surfaces + resize cases; TSan.
- How to prevent it: a surface event is applied at the frame head only (an existing rule, promoted to part of the design).

### 6.5 P5 Reading "pure function" as "parallelise at will"

- How it fails: record is parallelised too, and two threads touch `vsg::Context` at once.
- How to detect it: the `I6` code review + TSan.
- How to prevent it: parallelism lives in the derivation layer only; a backend that records in parallel must prove its own determinism (`R20`).

### 6.6 P6 Conflating a row's lifetime with a resource's

- How it fails: a row still answers (an in-flight plan reads it) while the resource has already been released.
- How to detect it: `T25` (hold an old snapshot outside the Session and submit).
- How to prevent it: the residency rule looks only at "live snapshot references" (`I4`); a row only decides "is the fact still here".

### 6.7 P7 The plan difference fails on "same name, different content"

- How it fails: changing a pass' target is taken as "remove one, add one", and the pipeline and target are rebuilt every frame.
- How to detect it: `T7`'s `builds` delta assertion.
- How to prevent it: the difference compares the whole `Pass` value (including `Target` / `inputs` / state overrides); the name is only identity.

### 6.8 P8 Preparation becomes "compile everything"

- How it fails: every variant is compiled during loading and cold start explodes.
- How to detect it: the `builds` delta after preparation and the preparation's own duration.
- How to prevent it: `prepare()` answers only what this input needs (`R23`).

### 6.9 P9 Treating a batch as a transaction

- How it fails: the caller expects a failed batch to roll back, but a batch only delimits visibility.
- How to detect it: the semantics of `T31`.
- How to prevent it: the documentation says "a batch is not a transaction" (the existing comment on `Scene::beginEdit`).

### 6.10 P10 Querying with the live camera

- How it fails: the camera moves between frames and the query answers the previous frame's picture with the new camera.
- How to detect it: `T20`.
- How to prevent it: the query takes the View value of `FrameResult.query` (`I7`).

### 6.11 P11 Implementing preparation as "run a frame without submitting"

- How it fails: `Session::beginFrame()` for a window frame calls `viewer->advanceToNextFrame()`, which **acquires** a swap-chain image; without a submission there is
  never an image to acquire again (the next frame starves), and with a submission it is no longer preparation. And a pass that draws into a window can only draw into
  that image, so "record once and throw it away" does not even cover it — preparation would leave an acquire starvation behind under pixel cases that look green.
- How to detect it: the `builds` delta assertion does not hold, or the second frame's `beginFrame()` answers an empty token / an acquire-timeout VUID appears.
- How to prevent it: preparation does the **value** side only (halves / sets / uploads) and the keys come from `drawNeeds`; the window target's format is answered by
  `WindowTarget::facts()` (const, no acquire needed). The implementation lives in `VsgBackend::prepareFrame`: it goes through `collectFrame` + `recordContent`, and
  **never** calls `Session::beginFrame()` / `PresentTarget::present()`, so the frame progress watermark (`submitted` / `presented`) does not move — which is exactly what `T33` asserts.
- **Boundary** (ADR-2026-11-14): `P11` is about a **frame** — `beginFrame`'s acquire, submission, drawing into the swap-chain image, present. Preparation's own
  **transfer-only** batch does not follow that rule: it has no frame, no swap-chain image and no present, and the frame progress does not move; its difference from
  "run a frame without submitting" is observable (`T35`'s `submitted` / `presented` assertions).

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | making the content model itself immutable (a fully persistent scene graph) | authors need mutability; it pushes the complexity onto every user (`G7`) | ADR-2026-11-12 |
| D2 | multi-threaded writing on the content side (locking the scene graph) | zero benefit, very high error cost; one writer + happens-before instead | ibid. |
| D3 | allowing a `Node` several parents (an OSG-style DAG) | state accumulates along several paths, traversal grows exponentially per instance, and row identity could only be a path | ibid. |
| D4 | keeping "the address is the identity" | saves one registry and sells a whole class of UAF | ibid. |
| D5 | ids allocated by the Session | there is no Session when content is built externally; sharing across windows must be the same id | ibid. |
| D6 | bindless / GPU-driven by default | no second backend, no measured bottleneck (`G8`) | ibid. |
| D7 | an automatic-barrier render graph in this layer | synchronisation belongs to the backend (`G2`) | ibid. |
| D8 | keeping the `addPass` family + a runtime refusal | once a plan is a value, misuse cannot be expressed; the refusal path is itself complexity | ibid. |
| D9 | content references carrying an id without a revision | an old frame would resolve to a new value, breaking `R9` / `I1` | ibid. |
| D10 | building only the landed identity / lifetime layer and leaving the rest as a proposal | "publication = one pointer swap" would not hold and mirror / delta / memo would stay on the track | ADR-2026-11-12 §option D |
| D11 | implementing preparation as "run a frame without submitting" | a window frame's `beginFrame` acquires an image: without a submission it starves, with one it is not preparation; and a pass drawing into a window cannot even be covered (`P11`) | ADR-2026-11-12 |
| D12 | one engine driving several Surfaces (graphics-next F8) | no consumer (`G8`); `FrameResult` / `PresentRecord` would change to per-surface right after landing (`G10`); appfw ownership would have to be rewritten; consistent with architecture D2 | [ADR-2026-11-13](ADR-2026-11-13-one-engine-one-surface.md) |
| D13 | adding a second entry point to preparation (`flushUploads()` / `uploadNow()`) | two entry points answer the same question ("build what this frame needs") and the caller still has to know when to call them: complexity moved to the caller (`G1` / `G7`), and no consumer today asks to split building from transferring (`G8`) | [ADR-2026-11-14](ADR-2026-11-14-prepare-may-submit-transfers.md) |

## 8. Tests and Acceptance

| # | Criterion | Case / command | Status |
|---|---|---|---|
| T1 | publication is O(changed) | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` | implemented |
| T2 | an in-flight frame is untouched by content edits | `RenderEngineTest.AMaterialEditLandsOnTheNextFrame...` + the scene version | implemented |
| T3 | a batch = one publication | `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` + `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` | implemented |
| T4 | a prototype folds once and placement is counted separately | `SnapshotTest.APrototypeFoldsOnceAndIsPlacedNTimes` | not done |
| T5 | a stale handle answers gone | `ContentRegistryTest.AStaleHandleAnswersGone` | implemented |
| T6 | residency = live snapshot references | `ResidencyTableTest.TheCollectableSetIsEnumerable` | implemented |
| T7 | the plan difference | `PlanTest.APassIsAddedWithoutAnyAddCall` | implemented |
| T8 | derivation is pure | `SelectTest.SameInputSameList` (the same snapshot repeated, other views interleaved, after the scene is cleared, and 8 threads × 32 rounds concurrently item by item equal to serial); `R15`'s fold count is pinned by `GraphicsTest`'s `contentCollect*` cases | implemented |
| T9 | the sort is a total order | `SelectTest.TheSortKeyIsATotalOrder` + `SelectTest.TheKeyReadsDrawOrderThenLayerThenSegmentThenDepth` + `SelectTest.TheKeySortsByTheBoxCentreNotTheNodeOrigin` | implemented |
| T10 | capability negotiation | `SessionTest.AMissingRequiredCapabilityIsRefusedWithACode` | not done |
| T11 | the shutdown phase | `SessionTest.ShutdownIsIdempotentInEveryPhase` | not done |
| T12 | replay | `ReplayTest.ARecordedFrameReplaysToTheSameCommands` | not done |
| T13 | concurrency | `scripts/tsan_check.sh` | run at the end of every step |
| T14 | existing behaviour unchanged | `ctest` (30 suites) | 30/30 |
| T15 | documentation gates | the eight `scripts/check_*.py` + `check_vsg_content_seam.cmake` | all clear |
| T16 | quantify "pass / fail" | the measurements in `../memory/graphics.md` | recorded step by step |
| T17 | an old frame reads the old revision | change material / geometry / program with two frames in flight and diff item by item | implemented (`SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith`) |
| T18 | picking several instances | two instances of one prototype | not done |
| T19 | picking after a pose | a parent joint poses and its child is picked | not done |
| T20 | picking with the frame camera | press after the camera moved | not done |
| T21 | publishing a shared prototype | two Scenes sharing a prototype | not done |
| T22 | prototype lifetime | instances survive the prototype's destruction | not done |
| T23 | the instancing cost (A/B) | a 5000-row prototype × 10 instances; report `snapshot_rows` / `draw_rows` / `placement_multiplications` / publish / select / residency; procedure in `../memory/graphics.md` | not done |
| T24 | culling per row, not per chunk | one chunk with two segments under different parent transforms, the first out of view: `SelectTest.OneChunkCullsEachRowOnItsOwnBox` (two boxes, one in and one out; they swap when the camera crosses, and culling stays on) | implemented |
| T25 | an old snapshot after reclamation | hold an old snapshot outside the Session and submit | not done |
| T26 | pose subtree propagation | a parent joint poses | not done |
| T27 | pose picking | pick a child after a pose | not done |
| T28 | pose culling | part of a posed subtree leaves the view | not done |
| T30 | plan validation | `PlanTest.*IsRefusedWithItsCode` (the four kinds) + `PlanTest.TheCodeSpellingsAreStable` + `PlanTest.AKindChangeOnACustomPassIsRefusedWithItsCode` | implemented |
| T31 | publish inside a batch | `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` (`Posted` → `Unchanged` → `InsideBatch` → `Posted`) + `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` | implemented |
| T32 | preparation's answer = the frame's builds | `CompiledProgramsTest.TheReportedNeedsAreWhatTheFrameBuilds` (the key sets are equal, device independent) + the key-set assertion in `ContentPassTest.AMultiLayoutScopeServesEveryHalfItWasBuiltFor` (the recording side) | implemented |
| T33 | after preparation the recording's `builds` delta is 0, and preparation neither submits nor presents | `VsgBackendTest.PrepareBuildsEveryIdentityAndTheNextFrameThenBuildsNothing` (`halves().builds` / `sets().builds` / `uploads().uploads` / `inputSetBuilds` / `pool.created` all 0, `progress->submitted` does not move, and the pixels are still drawn) + `HeadlessBackendTest.PrepareHandsOverTheSameFrameTheNextFrameRunsAndRunsNothing` (the engine side: the same value, nothing executed, no frame number taken) | implemented |
| T34 | the snapshot source equals a direct collection item by item | `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames` (600 frames, transform / opacity / material / placement edits, diffed item by item) + `FrameExpandTest.AFrameExpandsTheSamePictureTheSceneWould` (the frame-level seam after binding) | implemented |
| T36 | the frame result is that frame's value | `RenderEngineTest.TheFrameResultIsThePictureThatWasAnswered` (snapshot / camera / counts / diagnostics / present; a publication landing between frames does not change the answered frame) | implemented |
| T35 | preparation's upload batch is a real upload and is not a frame | `VkPrepareUploadTest.APreparedTexturedFrameUploadsNothingWhenItRuns` (preparation reports 2 streams + 1 texture; the following `executeFrame`'s `stream_uploads` / `texture_uploads` are both 0 and the pixel is the texture's colour) + `VkPrepareUploadTest.ASecondPrepareSubmitsNothing` (`uploads == 0`, `progress->submitted` / `presented` do not move) + `PrepareUploadTest.APreparedTexturedFrameCopiesItsTextureAndUploadsNothingWhenItRuns` / `ASecondPrepareCopiesNothing` (`vsg`: the same procedure, `MaterialImages::staged() == 0` proves the host bytes were released with the copy) | implemented |

### 8.1 Kill thresholds (`K#`)

Each step's **acceptance threshold and rollback condition** (ADR-2026-11-12 decided that `K1–K11` mean this from then on, not "reject the proposal").
A step that breaks its own `K` ⇒ that step is rolled back and the conclusion is written back into the ADR, **without reopening "should we adopt this"**.

| # | Threshold (breaking it = roll the step back) | How it is measured |
|---|---|---|
| K1 | a 2000-row scene with 1 row edited, `publish` > 500 µs (CoW degenerating into a whole-table copy) | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` |
| K2 | a resident snapshot > 3× the row table's bytes | `alloc_trace.c` / RSS |
| K3 | collect with a moving camera > 1.2 × 2160 µs | `SceneTest.MeasureWhatOneCollectionCosts*` |
| K4 | more than 5% of the cases present when the substep started need a **semantic assertion** changed to pass (pure renames / mechanical migrations do not count; the procedure is below) | counted directly |
| K5 | the line count grows instead of falling (bridge + lifetime layer > 100% of today's, by the same 2519-line measure) | `wc -l` |
| K6 | TSan reports any **new** data race | `scripts/tsan_check.sh` |
| K7 | an in-flight frame's old content is changed by a new publication (material / geometry / program) | `T17`'s item-by-item diff |
| K8 | a data race when two Scenes share a prototype, or a cross-thread case cannot be refused | `T21` + TSan |
| K9 | picking / culling after a pose disagrees with the picture | `T19` / `T27` / `T28` |
| K11 | a 5000-prototype, one-edit publish over budget (a whole metadata copy showing through) | `N19` ≤ 50 µs or the byte ceiling |

**`K4`'s counting procedure** (5a / 5b-1 / 5b-2a / 5b-2b / 5b-2c each apply it on their own):

- **The denominator**: the union of the cases `--gtest_list_tests` lists for `test_graphics` + `test_vsg` when the substep starts; `DISABLED_` is not counted, a runtime
  SKIP is; the `TEST` macros in the sources are not counted (`test_vsg` has 544 source macros and 542 in the executable).
- **The numerator**: cases that only pass after an expected value / observable behaviour is changed; renames, aliases, mechanical signature / type migrations and file moves do not count.
- A case that only pinned a deleted mechanism and has no successor semantics is deleted with the mechanism, but each must name the new case carrying the same observable
  behaviour; one that cannot name a successor counts as 1.
- **5a**'s changes to existing semantic assertions must be **0** (a shadow path; one single change rolls 5a back and 5b-1 is not entered);
  **5b-1 / 5b-2a / 5b-2b / 5b-2c** each re-take their denominator when they start and do not carry the previous one over.
- The last measurement was 572 + 542 = **1114**, so the 5% line is **55 cases** (the 56th crosses it); that number is an initial reading, not a hard-coded one.

## 9. Status and Evolution

### 9.1 Current status

- **Landed**: F1 (snapshot publication), F2 (pure derivation), F3 (identity), F4 (the engine table by ref), F5 (row-level epoch reclamation),
  F6 (a plan is a value), F7 (the preparation entry point: the answer + the pre-build), F9 (publication's three-state answer + the snapshot handle), F11 (the total-order sort key).
  `SnapshotSource` answers every draw of Published mode, and `Scene::collectDrawItems` is Direct mode's direct path;
  the plan lives in `Plan.hpp` / `Plan.cpp` / `EngineCompile.cpp` (`applyPlan`); preparation lives in `DrawNeeds.*` and
  `RenderEngine::prepare()` → `RenderFrameExecutor::prepareFrame` → `VsgBackend::prepareFrame`.
- **The plan's value shapes**: `PassDesc::target` is a `Target` value (`Window | Explicit | Transient`, §2.8) and `PassDesc::output`
  is an `Output` value (name + whole_target + image); a value whose formats disagree is refused by `validatePlan` (`InvalidTarget` /
  `InvalidOutput`). `View` and `RenderArea` are values as well.
- **Not done**: F10 (the prototype and instance layer) and F12's `poses` half (which waits on F10); `FrameResult` / `query` themselves are in.
- **Nothing is published implicitly inside a frame**: publication is the content writer's verb, and a frame takes `Scene::publishedSnapshot()` at build time; `bindSnapshot`
  has only the meaning "override" (`graphics-threading.md` §2.13).
- **Relation to the threading contract**: `graphics-threading.md` is the current threading contract (`C1–C15` / `N1–N14` are retired numbers, not reused);
  `graphics-scene.md`'s description of collection still carries mirror-era wording and will be aligned when it is next rewritten.

### 9.2 Known gaps

- `N2`'s 50 µs is not in this round: it needs a dirty notification from the content side, which directly conflicts with "a Node has no back pointer to its Scene" (`C2`).
  The current gate is ≤ 250 µs (203 µs measured on the prototype); the 50 µs target is in `../backlog/graphics.md` (trigger-gated).
- Poses (`PoseSet`), the post-processing dependency graph and the replay backend's algorithm details are not designed yet: they get a
  dated research proposal when a consumer appears, rather than being built up front (`G8`).

### 9.3 Compatibility / migration

The takeover is complete: the mirror / delta generation of shapes is gone, publication took over collection, and the plan is the only shape; the `addPass` family is gone
and `applyPlan` is the only entry point. The division into steps, and each step's criteria and rollback conditions, belong to the decision record — see
`../decisions/ADR-2026-11-12-adopt-graphics-next.md` and `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`.
Only F10 (prototypes and instances, a separate experiment) is unlanded: it is a new capability, gated on measurement as §9.2's gap says, and not built up front.
**The shape of a pass declaration**: `PassChain` collects declarations only and submits them in one `apply()` (validating the whole plan), and the host may name the
scope (`PassChain::build(engine, desc, owner)`) so that a rebuild updates in place; `DemoScene`, the chain presets and the tests all go through `applyPlan`.

## 10. References

### 10.1 Code

- `src/viz/graphics/sdk/vine/graphics/Plan.hpp` / `src/viz/graphics/src/Plan.cpp` (the plan's contract, validation, `describe`)
- `src/viz/graphics/src/EngineCompile.cpp` (`applyPlan` / `applyPlanRows` / `plan` / `makePassNode`)
- `src/viz/graphics/sdk/vine/graphics/PrepareReport.hpp` (the preparation entry point's answer) +
  `src/viz/graphics/src/RenderEngine.cpp` (`RenderEngine::prepare()`: compile + expand, then hand over to `prepareFrame`)
- `tests/test_graphics/PlanTest.cpp` (all of `T7` / `T30`'s cases)
- `tests/test_graphics/SnapshotShadowTest.cpp` (`SnapshotSourceTest`: 600 frames, the old picture inside a batch, T17's old snapshot and the empty-snapshot mutation)
- `src/viz/graphics/sdk/vine/graphics/ContentId.hpp` (identity, `ContentRef`, the registry)
- `src/viz/graphics/src/SceneSnapshot.hpp` / `SceneSnapshot.cpp` (the snapshot prototype, `selectSnapshot`)
- `src/viz/graphics/src/SnapshotSource.hpp` / `SnapshotSource.cpp` (the snapshot's `DrawListSource` face: the production source)
- `src/viz/graphics/src/DrawListDiff.hpp` / `DrawListDiff.cpp` (the shadow path's field-level comparison)
- `src/viz/graphics/src/SceneRows.cpp` (the publication's cache side)
- `src/viz/graphics/src/DrawListBake.cpp` (the live collection and sorting)
- `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/PinnedResources.hpp` (residency by key)
- `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/DrawNeeds.hpp` / `src/internal/DrawNeeds.cpp` (`drawNeeds`: the only key builder preparation and recording share)
- `src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp` (fact rows and epoch reclamation)

### 10.2 Documents and ADRs

- `../decisions/ADR-2026-11-12-adopt-graphics-next.md` (the adoption decision, the rejected options)
- `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md` (the split into a shadow path, a source flip, the public entry point and the deletion, and their order)
- `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` (where `I4` comes from)
- `../decisions/ADR-2026-11-11-content-identity-is-a-handle.md` (where `I2` / `R31` come from)
- `../design/graphics-threading.md` (the threading contract), `../design/graphics-scene.md` (the scene and collection in use)
- `../memory/graphics.md`: the measurements behind `T16` / `T23`
- `R31–R39` and `K7–K11` came from the 2026-11-11 proposal and its two audit rounds; those documents were dated and
  non-authoritative, and are deleted now that this page states their results

### 10.3 External material

- None (no third party is introduced; every conclusion is backed by this repository's measurements and cases).
