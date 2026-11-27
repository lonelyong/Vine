# graphics review 19: the machinery rounds and the picture half - twenty-three rounds, one question each

> **Status**: draft, non-authoritative - a working record. §1 is the plan this round executes; §2-§16 are the machinery
> rounds in the order the plan states, §17-§20 are the four picture-half objects the plan deferred, §22-§26 are the five
> areas those named next (the memory tier, `Device.cpp` outside the pass walk, the threading half, the appfw surface, the
> tests as oracles); §27 is the central registry of every finding (`Z#`) and the proposed order.
> **Last verified**: 2026-11-27, tree `012cb142` on `dev`.
> **Scope**: review 11 walked the shading chain, review 17 the picking and the picture, review 18 the navigation and the
> three-dimensional vocabulary - and states in its own header that it does NOT walk "the frame machinery, the threading
> model or the backend's memory tiers". Those three are this round's core. The trigger is not a defect report: review
> 18's landings (`Y1`-`Y12`) all landed in one day, so the machinery that receives them is the part no round has read
> since review 16.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/*.hpp` (102), `src/viz/graphics/src/*.{cpp,hpp}` (120),
> `src/plugins/gfx_backend_vk/**`, `tests/test_graphics`, `tests/test_vk`, `.ai/design/graphics-*.md`,
> `.ai/decisions/ADR-2026-11-2*.md`.
> **Backlog**: `../backlog/graphics.md` - this round adds no entry; §27 lists the candidates, for approval.
> **Numbering**: `Z#` is this page's own set (review 11 `V#`, review 17 `W#`, review 18 `Y#`). The module's `F#` / `C#` /
> `R#` / `N#` / `P#` / `D#` / `T#` / `G#` remain the design pages'.
> **Rule of evidence**: every finding names a `file:line`, a test, or a number produced by a command quoted beside it.
> A peer statement is about that peer's public source; no peer is cited for a behaviour its source does not state.

## 1. The plan

### 1.1 Why these ten rounds, and why in this order

The module is 102 SDK headers and 120 private files, and a review that reads them alphabetically answers no question.
Each round therefore takes **one object family and one question**, and the answer is a verdict per finding, not a walk
report. The families and the order are the ones a host meets a picture through: it states a pass table (R1), hands over
content (R2), the content becomes GPU resources (R3), a camera frames it (R4), state and shaders shade it (R5), a
backend executes it (R6), a surface shows it (R7), a diagnostic explains it when it does not (R8), a test proves it
(R9), and a page describes it (R10).

The order is also the order in which a defect is cheapest to see: a wrong verb in R1 makes every later round's evidence
ambiguous, so the entry vocabulary is settled first; a wrong publication in R2 is invisible in pixels, so it is settled
before the rounds that judge pixels.

### 1.2 The rounds

| # | Objects | The one question | Peer reference (public source) |
|---|---|---|---|
| **R1** | `RenderEngine` (59 public signatures), `EngineFrame`, `EngineCompile`, `FramePump`, `FrameRunner`, `FrameResources`, `FrameTable`, `Plan`, `SessionShape`, `RenderPass`, `RenderPassExecute`, `PassScheduler`, `PassRegistry`, `PassChain` | Does every public verb either take the frame interval or state why it need not - and does one entry answer one fact? | Filament `Engine` / `Renderer::render` + `FrameGraph`; O3DE `RPI::FrameGraph`; Unreal `FSceneRenderer` + `FRenderCommandFence` |
| **R2** | `Node`, `Group`, `MatrixTransform`, `PositionAttitudeTransform`, `AutoTransform`, `StateNode`, `Geometry`, `Scene`, `SceneRows`, `SceneSnapshot`, `ScenePublisher`, `PublicationStore`, `DrawListBake`, `DrawListDiff`, `SnapshotSource`, `ContentRegistry`, `MeshAppend` | Is a publication a whole value, and does an edit re-fold exactly the rows whose answer moved? | Filament `RenderableManager` + dirty components; three.js `matrixWorldNeedsUpdate`; O3DE transform dirty propagation |
| **R3** | `Target`, `TargetPlan`, `Residency`, `ResidencyTable`, `RetirementQueue`, `DeviceClock`, `FrameTimeline`, `SurfaceLedger`, `ImageRef`, `Texture`, `ShaderProgram`, `Geometry` values | Does every signature say who owns what, and does every release name the evidence that makes it safe? | Filament `ResourceList` / `Fence`; Vulkan timeline semaphores; WebGPU `GPUDevice.destroy` |
| **R4** | `Camera`, `CameraSnapshot`, `CameraMirror`, `CameraManipulator`, `OrbitCameraManipulator`, `RayIntersection`, `Frustum`, `ViewPlacement`, `DepthRange`, `DepthPolicy`, `PixelGeometry` | Do review 18's landings (`Y2`-`Y6`) hold on the tree, and did the landing leave a second spelling of the same fact? | three.js `OrbitControls` / `OrthographicCamera`; Filament `Camera` |
| **R5** | `RenderState`, `StateResolution`, `Material*`, `Light*`, `LightAbi`, `MaterialAbi`, `DrawAbi`, `ShaderAbi`, `BuiltinShaders`, `ProgramVariant`, `ShadingMode`, `ChainBuilder`, `ChainPresets`, `shaders/**` | Does every shading rule have exactly one home, and does each ABI width state its unit? | Filament `MaterialInstance` + `PipelineStateCache`; three.js `Material`; O3DE `Material` |
| **R6** | `Device`, `TargetTable`, `TargetObjects`, `WindowTarget`, `WindowSystem`, `DynamicRendering`, `BarrierPlan`, `ContentDraw`, `ContentImages`, `ContentCaches`, `Readback`, `ShaderCompiler`, `StateMapping`, `FrameHead`, `PassScheduler` (vk) | Does the backend have one classification per question, one memory tier per resource kind, and a stated barrier provenance? | Filament `VulkanDriver` (`PipelineCache`, `DescriptorSet`); Vulkan validation layer as the oracle |
| **R7** | `SceneView`, `InputRouter`, `Output`, `OutputRegistry`, `FpsOverlay`, `AxesMarker`, `AxisGizmo`, `LightRig`, `Primitive`, `SceneBuild`, `RenderArea`, `Viewport`, `Target`, `WindowPresentTarget`, appfw render surface/view, `app_shell`, `demo_plugin`, `model_viewer` | Does every host verb map to one engine fact, and does any host callback outlive the object it captures? | O3DE viewport / `ViewPane` stack; Qt RHI; Filament `View` |
| **R8** | `Diagnostics`, `DiagnosticDraft`, `ReportOnce`, `RenderDiagnostic`, `BackendAbi`, `FrameReport`, `PrepareReport`, `FrameResult`, `Residency` | Is a code an identity, is a refusal one shape, and is every number's scope stated? | Filament `utils::Status`; Unreal `FMessageLog`; Vulkan `VkResult` classification |
| **R9** | `tests/test_graphics`, `tests/test_vk` | Which asserted fact is computed independently of the value it asserts - and which of review 18's landings has no mutation-proof witness? | mutation-testing practice (a case must fail when the rule is reverted) |
| **R10** | `.ai/design` (10 graphics pages), `.ai/decisions`, `docs/guides/graphics*.md`, `scripts/check_*.py` | Does every normative sentence still describe the tree, and does every number name the command that produced it? | this repository's own gate scripts |

R11-R14 (§13-§16), R15-R18 (§17-§20) and R19-R23 (§22-§26) were added as the earlier rounds named the objects they had
not read; each states its own objects and its one question at the head of its section, and the order they were taken in is
the order those rounds' own "what these rounds did not cover" paragraphs named.

### 1.3 What each round may NOT do

- No code change without an explicit go; a fix is described, never applied.
- No finding that is only "this is not how engine X does it": a peer is cited to name a missing fact, never to rank taste.
- No finding without a location. A suspicion that cannot be pinned to a line, a test, or a quoted command is recorded in
  §27 as an open question, not as a finding.
- No re-litigating a decision an ADR already carries unless the tree contradicts it.

## 2. R1 - the engine's verbs and the frame interval

*The question*: does every public verb either take the frame interval or state why it need not, and does one entry
answer one fact?

*The peer shape*: Filament splits the same problem into `Engine` (ownership), `Renderer` (one submit per frame) and
`FrameGraph` (compile), and every mutating call is documented as "not thread-safe, call from one thread"; O3DE's
`RPI::FrameGraph` compiles per frame and refuses attachment changes mid-frame. This module is stricter than both (it
has one interval with two named refusals), which is why a verb outside the rule is a finding rather than a style
difference.

### 2.1 What the tree states

The interval is `detail::FramePump`: `beginFrame()` / `beginBuild()` claim it (CAS on a compile flag under
`phase_mutex_`), `phaseGate()` + `recording()` is the gate a shape change uses, `waitIdle()` / `stop()` finish the
frame thread. Four verbs take it: `frame()` (`RenderEngine.cpp:365`), `prepare()` (`:307`), `applyPlan()`
(`:783`, through `beginShapeChange`), and `setFrameThreading()` (`:392`, through `FramePump::setThreaded`). Two more
finish a frame in flight instead of refusing: `setDevice()` (`:97`, `pump_->waitIdle()`) and `shutdown()` (`:241`,
`pump_->stop()`).

### 2.2 Z1 (threading / high): three public verbs write or read the compile's own tables without the interval, and one of
them says in its own doc that it writes "the same one storage a plan writes"

**Evidence.**

- `RenderEngine::publish(name, target)` → `resources_->publish(...)` (`RenderEngine.cpp:893-899`);
  `RenderEngine::unpublish()` → `resources_->unpublish(name)` (`:908-913`); `RenderEngine::resolve()` →
  `resources_->resolve(name)` (`:901-906`); `RenderEngine::transientTarget()` → `resources_->transient(...)` (`:856-859`),
  `transientTargetCount()` → `resources_->transientCount()` (`:851-854`).
- `detail::FrameResources` states the opposite contract about itself: "All methods are called from the engine's single
  frame thread (see `RenderFrameExecutor`'s threading contract), so **no synchronisation is needed**"
  (`src/viz/graphics/src/FrameResources.hpp:47-48`). It holds a `std::map<String, intrusive_ptr<RenderTarget>>` and two `std::set`s, and
  none of its methods locks (`grep -n "mutex" src/FrameResources.*` - no hit).
- The compile walks those tables every frame: `SessionShape::compile` calls `resources_.beginFrame()` /
  `seedFromHostBindings()` / `recordPublication` / `resolveTransient` / `retireUndeclared`
  (`EngineCompile.cpp:678`, `SessionShape.cpp`).
- The public header states no thread rule for `publish` / `unpublish` / `resolve` / `transientTarget`
  (`RenderEngine.hpp`, the five declarations) - and `publish`'s own doc describes the host as re-publishing "when it
  swaps what it offers", i.e. at a time of the host's choosing.

**Consequence.** A host that keeps a standing binding and swaps it (the documented use) from the thread that is not
driving `frame()` - which is the thread `publish` is written for, because the engine's frame path is explicitly
single-threaded - mutates a `std::map` while `seedFromHostBindings()` copies it: UB, not a refusal. The same holds for
`transientTarget()`, whose doc tells the owner to "preview it, read it back" with no hint that the table is the
compile's.

**The gap is local.** `PublicationStore` solves exactly this shape one class over: it takes `publications_mutex_` for
"a lookup, an insert" (`PublicationStore.hpp:36-40`, `PublicationStore.cpp:20-88`). `FrameResources` was written with
one caller in mind and kept its public doors after the engine grew a frame thread.

**Choice for the owner.** (a) give `FrameResources` the store's own mutex (four methods, one lock each) - the cheap
answer and the one that keeps the host's "swap a binding whenever" promise; or (b) state the rule in the five public
docs and refuse outside the interval like `applyPlan` does. (a) matches what the class above already does.
**code + doc (+ test: two threads, one publishing, one framing, TSan).**

**Fixed 2026-11-27, and option (a) was not enough.** `FrameResources` now takes ONE lock over every table a host verb can
reach - not four named methods - because the accessor those verbs shared (`hostBindings()`) handed out a reference to a
locked container, i.e. one door that made the lock decorative. The compile reads a VALUE taken at the frame head
(`beginFrame` copies the host's bindings; `frameBindings()` is the compile's copy, `hostBindingNames()` is the copy a
plan validation between frames needs), so a publish landing mid-compile answers on the NEXT frame - which is what stops
one frame from seeding, checking and resolving against two different sets of bindings (`G1`). The public docs now state
the rule where a host reads it (`RenderEngine::publish` / `resolve` / `unpublish` / `transientTarget` and
`OutputRegistry`), including what a borrowed answer means. Lock cost: one plain `std::mutex` per verb, 2.8 ns
uncontended, a handful of verbs per frame. Rule: `graphics-threading.md` C23 / 2.18;
`ADR-2026-11-27-a-door-is-gated-or-locked.md`.

### 2.3 Z2 (threading / medium-high): `bindPassContent` writes the declaration a plan writes and the compile reads, and
is the one shape-change verb that does not take the gate

**Evidence.**

- `RenderEngine::bindPassContent()` → `shape_->bindPassContent(...)` (`RenderEngine.cpp:821-824`) with no
  `beginShapeChange` call.
- `SessionShape::bindPassContent` iterates `slots_` and `adopt`s a new `PassDesc` into the pass node
  (`SessionShape.cpp:262-273`), i.e. it rewrites the very storage the compile reads.
- Its own public doc says so: "The binding is written into the pass' declaration (`PassDesc::content`) - **the same one
  storage a plan writes and the compile reads**" (`RenderEngine.hpp`, `bindPassContent`).
- `applyPlan` refuses with `PlanRefusal::Busy` inside the interval and reports `engine.pass.changed_during_frame`
  (`RenderEngine.cpp:783-796`, `:740-765`). `bindPassContent` has neither.

**Consequence.** The two doors onto one storage disagree: a host that re-binds a pass' content at the moment a frame is
being compiled gets a silent race, while the same host re-stating the plan gets a value back. The header's own argument
for the gate ("the pass slots ... are what a compile walks") applies verbatim.

**Choice.** Route `bindPassContent` through `beginShapeChange` and answer a `bool`-shaped refusal, or - if re-binding
content is meant to be cheap - make it a declaration diff with the gate's lease and the same episode. It cannot stay the
unstated exception. **code + doc + test (a bind during a frame).**

**Fixed 2026-11-27, with the third answer the `bool` could not carry.** `bindPassContent` takes the same
`beginShapeChange` gate `applyPlan` takes and answers `ContentBindOutcome` - `Bound` / `NotRegistered` / `Refused` - so
"no such pass" and "retry between frames" stop being one `false` (`G4`); the refusal reports the same
`engine.pass.changed_during_frame` episode. The type deliberately has no `operator bool`: the compiler made every
existing call site decide, which is the gate this defect needed. The one in-tree host arrival,
`SceneView::setScene`, takes the remedy (a bounded wait, then a second attempt, then
`engine.view.content_not_bound` rather than letting the chain's content check accuse the wrong cause). Rule:
C20 / 2.18; case: `PlanTest.AFrameRefusesAContentBindFromInsideItsOwnCompile`.

### 2.4 Z3 (entry vocabulary / medium-low): the one entry that starts or stops a thread returns `void`, while the four
verbs it shares its refusals with return a value

**Evidence.** `setFrameThreading(FrameThreading)` is `void` (`RenderEngine.hpp`); its refusals -
`engine.threading.refused_from_frame`, `engine.threading.refused_in_flight` - are reported once per episode
(`RenderEngine.cpp:392-419`). Its siblings answer `FrameCallOutcome`, `PrepareReport`, `PlanResult{Refused, Busy}` from
the same `FramePump::Build::Refusal` (`RenderEngine.cpp:307-329`, `:365-391`, `:783-796`). `frameThreading()` reads the
mode back, so the fact is observable, but only by polling and only after the report.

**Consequence.** A host that starts a frame thread to decouple its UI has no local answer to "did it start": it must read
`frameThreading()` and correlate a diagnostic string. The header's own rule for this class is "a call that cannot be
resolved by waiting arrives as a VALUE instead of a race" (`frame()`'s doc).

**Choice.** Return the same `Build::Refusal` shape (or a small `ThreadingResult`) and keep the episode for the log.
**code + doc.**

**Fixed 2026-11-27, as the small result the review named.** `setFrameThreading` answers `FrameThreadingOutcome` -
`Applied` / `RefusedFromFrame` / `RefusedInFlight` - and the episode still governs the report, so a caller that made the
mistake twice still gets its one answer per call. The two rejected shapes are `D14` in `graphics-threading.md`: a `bool`
folds the two mistakes into one answer, and `EngineNotReadyReason` means "this engine cannot drive a frame" (it carries
`NoDevice` / `NotInitialized`, which a mode change cannot produce). The appfw convenience verb keeps forwarding and
returning nothing - a control without an engine has no mode to state, and a fourth state would put a GUI concern in the
SDK's enum. Rule: 2.17 / `D14` / `T40`; `ADR-2026-11-27-the-last-interval-verb-answers-a-value.md`.

### 2.5 Z4 (documentation / low): `frameContext()` claims one answer per frame and reads three fields live

**Evidence.** The header says "A PURE READ, AND THE SAME ANSWER ALL FRAME: the frame id, the announced size and the
ratio are **read live**, so a host that asks twice gets the same context; only `elapsed_seconds` advances"
(`RenderEngine.hpp`, `frameContext`). The implementation copies four fields and then overwrites three from live state,
including `frameNumber() + 1` (`RenderEngine.cpp:445-456`).

**Consequence.** "Read live" and "the same answer" are opposites: a frame built between two asks changes
`frame_number`, and a resize changes the size. The sentence is the one a caller relies on to decide whether it may cache
the value.

**Choice.** Say which is which: "the three facts are the engine's current ones; take the value once per frame" - or
return the frame's own context (the one `buildContentHalf` hands the passes). **doc.**

**Fixed 2026-11-27, as a sentence that states what the call does.** The three facts are named as the engine's CURRENT
ones (`frameNumber() + 1`, the announced size, the ratio), and the guarantee is moved to the place it actually holds:
a host asks once at the head of its update and hands that one value to the whole update. `elapsed_seconds` remains the
one field the frame clock moves, once per `frame()`. The implementation was already what this sentence says; the review's
second branch (return the frame's own context) would have made a cheap read carry state a query does not own.

### 2.6 R1 verdict

The interval itself is sound and unusually well specified: one claim point, two named refusals, a gate that makes
"decide and change" one step, and a frame answered exactly once on every path including a throw
(`answerEngineHalf`, `RenderEngine.cpp:631-666`). The defects are at the boundary: two verbs (`publish` family,
`bindPassContent`) touch the same tables as `applyPlan` and did not receive the rule, and one verb (`setFrameThreading`)
is the only one of the interval family that answers nothing. None of the three is a design question - each is a door
that was added later than the gate.

## 3. R2 - the content half and the publication

*The question*: is a publication a whole value, and does an edit re-fold exactly the rows whose answer moved?

*The peer shape*: Filament re-derives a renderable's world transform from its component dirty flags at the frame head and
keeps the transform hierarchy out of the shading path; three.js sets `matrixWorldNeedsUpdate` on a child and updates
ancestors only. This module is stricter: it publishes a whole row table with a per-row serial. Review 6's two defects
(`P0-1` chunk share keyed by the node epoch, `P0-2` a container row dropping its inherited state) are **fixed** and the
fixes are the shape described below - `storeResolved` compares before it stores (`DrawListBake.cpp:83-95`), and the fold
writes state/material/program for every row that owns state, not only for rows with draws (`:97-140`).

### 3.1 What the tree states

- The fold's output is one value with one writer: `ResolvedRow::operator==` is defaulted (`DrawListBake.hpp:129`), so
  "did the fold change" cannot fall out of step with the fields.
- The publication shares a chunk only when, for every row in it, `identity`, `links`, the node epoch **and**
  `resolved_serial` are unchanged (`SceneSnapshot.cpp:55-68`), and a box/value refresh disables sharing entirely
  (`:47-49`).
- A publish with nothing moved returns the previous value (`ScenePublisher.cpp:112-117`); a batch holds the publication
  back (`:105-107`).

### 3.2 Z5 (correctness argument / low): the share test compares the fold's output, but the select reads three `own`
fields, and one of them (`placement`) is outside the equality it relies on

**Evidence.**

- `selectRows` reads `row.own.fold.placement`, `row.own.fold.local` and `row.own.body.draws`
  (`DrawListBake.hpp:549-566`, `:585-600`).
- `makeSceneSnapshot` compares `identity`, `links`, the node epoch and `resolved_serial` (`SceneSnapshot.cpp:65-67`) -
  `own` takes no part.
- `local` is safe by derivation (`resolved.world` is a function of it) and `draws` is safe because the command holds
  shares of the live objects (`Row`'s doc, `DrawListBake.hpp` "LIFETIME"). `placement` is safe only because
  `AutoTransform::viewPlacement()` builds a **new** object per read (`AutoTransform.cpp:477-483`) and `ResolvedRow`
  records only `view_dependent` + the conservative box (`DrawListBake.hpp:110-116`) - so two different placements with
  the same bound compare equal.
- The thing that actually prevents a stale placement from being served is the **node epoch**: any edit that rebuilds a
  placement also announces (`AutoTransform.cpp:337-458` all call it through the node). That invariant is what makes the
  share sound, and `Node::announceContentChange`'s own list of "what a collected command list depends on" is
  "(transforms, visibility, layers, opacity, draw order, material, program, state)" (`Node.hpp:296`) - the placement is
  not in it.

**Consequence.** Today the behaviour is right and the argument is invisible. The next node type that answers a
`viewPlacement()` without announcing - the case the doc invites, because the list it publishes does not include
placements - is a silently stale chunk, i.e. exactly review 6's `P0-1` shape one level down.

**Choice.** Add `placement` to the sentence (and to the share test, by comparing the `ViewPlacementPtr` in `own` - one
pointer comparison per row, and then the epoch is a second line of defence rather than the only one). **doc (+ code).**

**Fixed 2026-11-27, at the identity the comparison needs.** `AutoTransform` caches its placement and rebuilds it in the one
`recompose()` every mutator of a carried fact reaches, so an unchanged node answers the SAME object and a changed one a
fresh value; `Node::viewPlacement` states that contract and `Node::announceContentChange`'s dependency list now names
per-view placements. The share test compares `RowOwn::Fold` whole beside identity, links, epoch and serial (the whole
group, with its defaulted equality, rather than a hand-written pointer field); `RowOwn::Body` is deliberately not
compared - a body change rebuilds the table. The review's first answer (a bare pointer comparison) was rejected on its
own: with a fresh object per read it would stop every billboard chunk sharing for no change (`D14` in
`graphics-frame-pipeline.md`). Cases: `SnapshotTest.APlacementThatMovedOutsideTheFoldIsNotShared` (red under the old
test) and `AutoTransformTest.ThePlacementKeepsItsIdentityUntilItsOwnFactsChange`;
`ADR-2026-11-27-a-share-compares-the-input-the-select-reads.md`.

### 3.3 Z6 (API semantics / medium): `bindSnapshot` cannot be withdrawn, and its "empty handle" reads as a withdrawal
but means "draw nothing" - so a host that binds once never sees the scene's later publications again

> **Fixed 2026-11-27.** `Scene` carries a `SceneId` (a `Handle`, as every other content object does), the store keys both
> its tables by it, `RenderEngine::unbindSnapshot` is the withdrawal, an empty handle keeps its one meaning (an empty
> picture), and `beginFrame` drops a binding whose scene answers gone - `graphics-threading.md` 2.13 / 2.15 / N19 /
> `ADR-2026-11-27-a-binding-is-keyed-by-identity-and-can-be-withdrawn`.

**Evidence.**

- `PublicationStore::bindSnapshot` always stores (`PublicationStore.cpp:15-28`); the store has no erase path for
  `bindings_` (`grep -n "bindings_" src/PublicationStore.cpp` - three hits: the find loop, the push, and the read in
  `bindingOfLocked`).
- `publicationFor` returns the binding **whenever one exists**, and only falls back to `scene.publishedSnapshot()` when
  the scene has none (`:31-41`).
- The public doc says "An EMPTY handle clears the binding: every view then answers an empty list, which is the same
  answer as a scene whose content draws nothing" (`RenderEngine.hpp`, `bindSnapshot`) - so "clears" here means "the
  value is empty", and the fallback is never reached again.

**Consequence.** Two distinct facts share one spelling: "draw this picture instead" and "go back to the scene's latest".
A host that binds an older publication for an offscreen bake (the documented use) can never return the engine to the
live content without destroying it. Second, because a binding is keyed by the scene's **address** and outlives it, a
scene destroyed while bound leaves an entry that any new `Scene` allocated at the same address inherits - the new scene
then draws the old scene's picture, silently.

**Choice.** (a) state that a bind is permanent (then the empty-handle sentence must stop using "clears"), or (b) add the
missing verb (`unbindSnapshot(scene)`, or make an empty handle mean "no override" and give "draw nothing" its own
spelling). (b) is the honest shape: an override with no withdrawal is a one-way door in a class that otherwise offers
`unpublish` for every other binding. Whichever is chosen, the binding's key should not be a raw address that outlives
its owner. **code + doc (+ test: bind, unbind, publish, the frame draws the new publication).**

**Fixed 2026-11-27, redesigned rather than patched.** The withdrawal is a verb of its own (`unbindSnapshot`) and the
empty handle keeps the meaning it always had - an empty picture - because the review's alternative (give the empty
handle the withdrawal meaning) would have deleted a real value. The key is not fixed by holding the scene alive (that
would make the engine an owner of host content with no observable end) nor by a private ticket (a second identity for
content): `Scene` now carries a `SceneId`, the token `ContentId.hpp` exists for, and every row of the publication
channel keys by it. The row's end is the scene's end, noticed by asking the identity at the frame open - the one place
the store already settles what it holds.

## 4. R3 - lifecycle and the ownership vocabulary

*The question*: does every signature say who owns what, and does every release name the evidence that makes it safe?

*The peer shape*: Filament's `ResourceList` defers destruction to a fence the caller can name; WebGPU requires an explicit
`destroy()` per resource; Vulkan timeline semaphores are the evidence this module's `RetirementQueue` uses.

### 4.1 What the tree states

- The public surface's ownership vocabulary is the repo's four spellings: an audit of
  `src/viz/graphics/sdk/vine/graphics/*.hpp` finds **no mutable `std::shared_ptr<T>` in any public signature**; every
  shared value is `std::shared_ptr<const T>` (`DrawListSource::collectDrawItems`, `FrameDescription::commands`,
  `LightSet::create`, `Scene::lightSet`) and the only mutable `std::shared_ptr` is the engine's private lease cell
  (`RenderEngine.hpp:1158`).
- The release path is evidence-based: `RetirementQueue::ReleaseFn` takes the `FrameTimeline*`, `DeviceClock` proves
  completion per submission, and `releaseContent` parks what it drops (review 16's landing; `graphics-threading.md` 2.6).
- `FrameResources`, `PublicationStore` and the pump each state their own lock scope.

### 4.2 Z7 (threading / high, latent): the "describe once per revision" cache exists three times and only `Geometry`'s is
guarded - and the design names `Geometry`'s lock as the reason content sharing is safe

**Evidence.**

- `Geometry::description()` takes `description_mutex_` (a shared lock for readers, a unique lock for the one builder) and
  its comment records why: "TWO ENGINES MAY DESCRIBE ONE GEOMETRY AT THE SAME TIME ... (the TSan gate found the
  unguarded cache, 2026-11-16)" (`Geometry.cpp:425-450`).
- `ShaderProgram::description()` is the same cache with no lock and no atomic
  (`ShaderProgram.cpp:45-54`; `mutable ShaderProgramDescriptionPtr description_`, `ShaderProgram.hpp:222`; `revision_`
  is a plain `std::uint64_t`, `:220`). Its comment defers to the sibling: "The cache key IS the revision (see
  Geometry::description)".
- `MaterialValues::description()` is the third copy, also unguarded, with `mutable` members and a `mutable` revision
  (`Material.cpp:39-52`, `MaterialValues.hpp:404-406`).
- The design says shaders are part of what is shared and must stay thread-safe: "geometry / shader / texture resources
  are shared, frames are not; sharing must stay thread-safe; two engines may frame at the same time"
  (`graphics-sessions.md:18`), and `graphics-threading.md` 2.16 names **only** `Geometry::description()` as the proof
  that "content sharing already holds" (`:311-316`).
- A frame built for one scene calls both caches: the row's `material_description` / `program_description` are taken at
  the publish boundary (`DrawListBake.hpp:163-165`, `refreshValues`). Two engines drawing one scene (the documented
  editor + offscreen-bake shape, `graphics-threading.md:259`) do that from two threads.

**Consequence.** The two caches the design's own concurrency claim depends on are the two that never got the lock. The
failure is not a wrong picture but `intrusive_ptr` refcount tearing and two identities for one revision (the exact
symptom `Geometry`'s comment describes). TSan has already reported a `ShaderProgram::description()` race once and it was
dismissed as a test artifact (`.ai/memory/graphics.md`, 2026-11-24 entry) - this is the reading that dismissal may have
hidden.

**Choice (as proposed).** Copy `Geometry`'s shape verbatim into both (`shared_mutex` for the cache, `std::atomic<std::uint64_t>` for
`revision_` as `Buffer` / `Texture` already do), and make §2.16 name the rule instead of one example. **code + doc + test**
(two engines, one program and one material, TSan).

**Fixed 2026-11-27, at the cause instead of at the copies.** "Copy the shape verbatim into both" IS the defect: a rule written
three times is three chances to write it wrong, and two of the three already were. The rule now has one owner - `vn::graphics::detail::DescriptionCache<Description>` - with `get(measure, build)` doing measure + compare + build in ONE critical section and `peek(measure)` as the read-only face; `Geometry`'s `shared_mutex` pair and `MaterialValues`' `described_`/`described_revision_` trio are deleted, and `ShaderProgram`'s cache is the same object. Two refinements the proposal above did not have: the key is measured INSIDE the lock (a material instance's `revision()` resolves, so a shared fast path would race two readers inside `resolved()`), and the lock is the PLAIN kind - 2.8 ns uncontended against 13.2 ns for a shared lock (measured 2026-11-27), so the geometry cache got cheaper. Making `revision_` atomic instead (the proposal's other half) would have fixed the key and left both buckets writing concurrently - rejected. `graphics-threading.md` 2.4 / 2.14 / 2.16 / C22 / T41; `ADR-2026-11-27-a-description-is-built-once-behind-one-lock`; `DescriptionCacheTest` (4 cases, 691/691, TSan 0 warnings, ASan PASS).

### 4.3 R3 verdict

The ownership vocabulary itself is clean - this is the one public surface in the module where the review found no fifth
spelling. The defect is in the implementation of the value types the vocabulary names, not in the vocabulary.

## 5. R4 - the navigation landings, re-read

*The question*: do review 18's landings (`Y2`-`Y6`) hold on the tree, and did the landing leave a second spelling of the
same fact?

### 5.1 Landed and correct

- The wheel is one operation: `onScroll` picks the anchor and calls `dolly(factor)` (`OrbitCameraManipulator.cpp:475-495`);
  the per-notch factor, the `setDollyToCursor` policy and the projection's own scale all live in `dolly()`
  (`:169-230`, `:806`). `Y2`/`Y3`/`Y4` are one implementation, and the switch now has a reader on the wheel path.
- The picture's scale has a name: `Camera::orthographicHeight()` (`Camera.hpp:183`), read by `pan` (`:171`) and by the
  orthographic window (`:778`); `apply()` writes `lookAt` and nothing else (`:400-418`), so an authored off-centre window
  survives gestures. `Y5` landed as stated.
- The first-person **mouse** look turns about the eye (`lookRotate`, `:449-455`, which ends with `syncFromCamera()` so the
  orbit state stays coherent), and `Mode` is `{Orbit, FirstPerson}` (`CameraManipulator.hpp`).

### 5.2 Z8 (behaviour + corpus / medium): the first-person **keyboard** look still calls `orbit()`, and the design page
says the arrows travel (**fixed 2026-11-27**: the gaze is one verb, `look()`, and a device only converts into it -
`graphics-navigation.md` 2.10 / F2 / F5 / R5 / P7 / T11)

**Evidence.**

- `onKeyDown` maps Left/Right/Up/Down to `orbit(...)` (`OrbitCameraManipulator.cpp:459-463`) in `Mode::FirstPerson`,
  while the mouse drag in the same mode calls `lookRotate` (`:449-455`).
- `orbit()` only moves `yaw_`/`pitch_` and calls `apply()`, and `apply()` puts the eye at `center + dir * distance_`
  (`:407-415`) - so a keyboard look carries the eye round an arc of the orbit radius, which is the very thing
  `graphics-navigation.md:22` records as the defect review 18 fixed ("first-person drag called `orbit()`, carrying the eye
  around an arc ... while the user 'looked'").
- `graphics-navigation.md:123` (F5) says the keyboard entry is "`moveForward` / `moveRight` / `moveUp`, and WASD/**arrows**
  in `FirstPerson`" - but in the code the arrows look, and travel on the vertical axis is Q/E (`:461-462`).

**Consequence.** Two input devices in one mode behave differently, and neither the header nor `F5` describes the code. A
walkthrough host that binds arrows to look (what the code does) reads a page that says they travel.

**Choice.** One look path for both devices (the arrows call `lookRotate`-equivalent, i.e. a yaw/pitch step about the eye),
and `F5` states what the arrows do - or the arrows travel and look moves to the mouse only. Either way the page and the
code state one rule. **code + doc + test** (a FirstPerson arrow key moves the direction and not the eye).

> **Fixed 2026-11-27.** The first branch, redesigned rather than patched. The cause was a missing VERB, not a wrong
> sign: travel had three public verbs of its own while the gaze had none, so the keyboard reached for the nearest
> angular verb and got the other metaphor's. The gaze now has one implementation and it is public -
> `OrbitCameraManipulator::look(deltaYaw, deltaPitch)`, angles of the VIEW's rotation (positive yaw turns left, positive
> pitch looks up) - and the drag (`lookRotate`) converts pixels into it while an arrow key converts one `rotate_step_`
> into it. `orbit()` keeps its own sense (the EYE's angles about the centre) and says so in the header. `F2` owns the
> look's devices and `F5` the travel keys (WASD horizontally, Q/E vertically), so the page no longer states a third
> thing; `P7` records the trap. `CameraManipulatorTest.AFirstPersonArrowKeyLooksAboutTheEyeWithoutTravelling` fails on
> the old mapping at its first assertion. See `ADR-2026-11-27-a-look-is-one-verb-whatever-the-device`.
>
> **Rejected.** The review's second branch (the arrows travel, look on the mouse only): it gives `moveForward` two key
> sets and the gaze none, and a keyboard-only walkthrough loses its look - `graphics-navigation.md` `D7`. Also rejected:
> pointing the arrows at a private look (leaves hosts with `orbit()` only - the same defect offered to them), and a
> second look implementation for the keyboard (two implementations of one rotation is the finding).

## 6. R5 - state, material, light and the shading ABI

*The question*: does every shading rule have exactly one home, and does each ABI width state its unit?

### 6.1 What the tree states

- **Coverage has one home** (review 18 `Y7` landed): `vine_alpha.glsl` declares the base-colour slot and owns
  `vnAlphaMode` / `vnAlphaFactor` / `vnAlphaCoverage` / `vnSurfaceCoverage`; the four callers are the forward stage, the
  G-buffer stage, the depth stage and the sky (`builtin_forward.frag:83`, `builtin_gbuffer.frag:66`,
  `builtin_depth.frag:50-52`, `vine_alpha.glsl:88-155`). A grep for a second threshold spelling finds none.
- **What casts is the sort's answer**: `PassDesc::selection` (`PassSelection::Opaque`), applied per command, and
  `graphics-rendering.md` 2.7 states both halves (a `Mask` caster cuts its silhouette; `Blend` and opacity < 1 cast
  nothing) with the cost in fetches.
- **The push range is the host contract** and the ABI's clip convention has one constant (`kFarClipZ = 0`,
  `ShaderAbi.hpp:143`), which the sky stage is the only reader of.

### 6.2 Carried, still open

`Y8` was not landed: `VineDrawBlock::model` is still described as "Object -> world", still packed for every draw, and
still read by no stage (`grep -rn "vn_draw\.model" src tests` - no hit; `ShaderAbi.hpp:158`). Review 18's choice (remove
it with `V2`'s normal matrix, or state the contract) is still the open decision, and `V2` (the normal matrix is
`mat3(vn_model)`) is still open with it.

### 6.3 R5 verdict

The shading side is the healthiest part of the module after review 18: one rule per fact, stated costs, and a page that
matches. The only open item is the unread ABI member, and it is already registered.

## 7. R6 - the Vulkan backend's internals

*The question*: does the backend have one classification per question, one memory tier per resource kind, and a stated
barrier provenance?

### 7.1 Verified

- **One barrier entry**: `vkCmdPipelineBarrier2` appears exactly once, in `BarrierPlan::recordBarrier`
  (`BarrierPlan.cpp:121`); every other site goes through it.
- **One memory-type decision per usage**: `MemoryAllocator` asks `DEVICE_LOCAL` or `HOST_VISIBLE|HOST_COHERENT` per
  `MemoryUsage`, and never silently mixes them (`MemoryAllocator.cpp:40-56`).
- **Device loss is classified at every queue point**: acquire, both submits, the staging submit and its fence wait,
  present, and the allocator all reach `noteDeviceLost` (`Device.cpp:1569`, `:2205`, `:2506`, `:4003-4004`, `:2308`,
  `:3564`); after a loss the frame path answers without touching the device (`:1804`, `:1866`).
- **One readback answer**: `ReadbackStatus` + the picture in one value; "the device never can" is `Unsupported`, "this
  target has no such image" is `Invalid`.

### 7.2 Carried, still open

- `T67`: content **streams** are filled into host-visible coherent memory while images and attachments are device-local
  (`CreateHostBuffer` path), so every measurement so far was taken where the two are the same memory. The fix shape
  already exists one unit over (`ContentImages` staging + `vkCmdCopyBuffer`).
- `T17`: three build-admission spellings (geometry claim, texture pending batch, program `failed` flag) and a cold build
  under the device-wide cache lock.

### 7.3 R6 verdict

No new defect found at this depth. The backend's invariants are stronger than the SDK's in exactly the places review 16
worked on, and the two remaining items are already in the backlog with triggers.

## 8. R7 - the host surface and the appfw bridge

*The question*: does every host verb map to one engine fact, and does any host callback outlive the object it captures?

### 8.1 Verified

- The engine owns its one area, the area owns the target, and the host holds handles that never outlive the engine
  (`createArea`'s doc; `RenderArea` borrows the target).
- The Qt bridge obeys the callback-lifetime rule in the instructions: `RenderControl` installs two `[this]` callbacks on
  the window and **clears both in its destructor**, with the reason written beside it (`RenderControl.cpp:87-111`) - the
  window outlives the control's body by a moment, which is exactly the shape that rule exists for.
- The host's resize path waits for the frame before restating its chain (review 17's `B8b`), which is what keeps
  `applyPlan` from answering `Busy` on a user resize.

### 8.2 R7 verdict

No new finding. The two observations that would have been findings - a callback outliving its capturer, and a host verb
that silently disagrees with the engine - are both already answered in the code and the docs.

## 9. R8 - diagnostics and reports

*The question*: is a code an identity, is a refusal one shape, and is every number's scope stated?

### 9.1 Verified

- Every code in the tree is registered (the `check_diagnostic_formats` gate is green), and the episode rules are per
  condition with an explicit re-arm.
- The readiness shape is one enum (`EngineNotReadyReason`), shared by `frame()` and `prepare()`, and the two new
  threading refusals answer it (`Reentrant` / `Concurrent`).
- A frame is always answered: a throwing sink is swallowed after counting (`Diagnostics::report`), and a throwing engine
  half becomes `PresentOutcome::Failed` with `engine.frame.failed` (`answerEngineHalf`, `:631-666`).
- Report parts are `?`-shaped: a device that cannot measure leaves a part disengaged instead of reporting zero
  (`FrameReport`'s contract; `RenderEngine::frameReport` returns an empty value with no device).

### 9.2 Observation (not a finding)

Three public accessors answer one published value: `frameResult()`, `presentRecord()` (`frame` + `present`) and
`presentedCamera()` (`query.view.camera`). Each is documented as a subset of `frameResult()`, so there is one owner and
two conveniences - acceptable, and named here only so a later round does not re-open it.

### 9.3 R8 verdict

No new finding. The diagnostics side of this module is the part that has converged.

## 10. R9 - the tests as oracles

*The question*: which asserted fact is computed independently of the value it asserts - and which landing has no
mutation-proof witness?

### 10.1 Verified

- Review 18's three behavioural landings have independent witnesses: `VkContentDrawTest` reads the fragment's own
  `gl_FrontFacing` in pixels and compares a mirrored quad with its unmirrored twin; `VkShadowTest` reads the **map's
  texels** (covered half 0.5, cut-away 0.0, plus an `Opaque` control) rather than the pass' declaration; `VkBackgroundTest`
  puts a quad 50 units away in front of the unit box and checks the camera at 5 units still sees the map.
- The review-6 tautology (`RenderWorldTest` comparing two reads of the same snapshot) was replaced by the serial-based
  share and by cases that build the expectation from the scene.

### 10.2 Z9 (verification / medium): the engine's own suite cannot be built under ASan on Linux, so the module's ownership
graph has no address-safety witness

**Evidence.**

- `tests/test_graphics/CMakeLists.txt` adds twelve **private** translation units to the test binary unconditionally
  (`target_sources(...)` at `:51-59`), including `SnapshotSource.cpp`, while the same target also links `vn::Graphics`
  (`:38`). The comment above them states the reason as a **Windows** one ("on Windows a DLL exports only what the SDK
  declares, so the private translation units ... are compiled INTO this binary") but no `if(WIN32)` guards them.
- The consequence is recorded in the memory ledger: "`test_graphics` 在本树里跑不了 ASan（加载即 ODR violation：目标自己
  编译 `SnapshotSource.cpp` 又链接 `libvnGraphics`）" (`.ai/memory/graphics.md`, review 16 entry).
- `scripts/asan_check.sh` therefore defaults to `test_gui`, and the backend half is covered through `test_vk`; the
  engine's half - `Scene`, `Node`, `PublicationStore`, `ScenePublisher`, `FrameResources` - is covered by TSan only.

**Consequence.** The one sanitizer that finds use-after-free and leaks does not run over the code that owns the scene
graph and the publication channel. Every finding in §2-§3 of this page is in exactly that code. The cause is a build
detail, not a property of the tests, so the gap is cheap to close and expensive to keep.

**Choice.** Guard the private `target_sources` with `if(WIN32)` (the Linux build gets those symbols from the shared
object, which the comment already says), then add `VN_ASAN_TARGET=test_graphics` to the recorded sanitizer commands.
**build + doc.**

**Fixed 2026-11-27, at the cause instead of at the guard.** The guard would have left the SAME defect on Windows - a
second copy of every private class beside the DLL's, invisible instead of absent - so the module now compiles once
into an object set and hands it to its own test: `vn_add_library(... OBJECT_TARGET GraphicsObjects)` makes the shared
library a shell around those objects, `test_graphics` links `GraphicsObjects` and no longer names a private
source, and the export table is untouched. Measured: `test_graphics` 687/687 in the normal tree, `ldd
build/bin/test_graphics` names no graphics library, `VN_ASAN_TARGET=test_graphics VN_ASAN_FILTER='*'
scripts/asan_check.sh` → `RESULT: PASS` (687 cases, no ODR report), `scripts/tsan_check.sh` → 687/687 with 0 warnings.
Argument and rejected options: [ADR-2026-11-27](../decisions/ADR-2026-11-27-a-module-test-links-the-modules-objects.md);
the rule is `graphics-architecture.md` 2.19 / `C6` / `D9`, test `T5`.

### 10.3 R9 verdict

The new witnesses are the strongest the module has had - each one reads a fact from the device rather than from the
declaration it is testing. The gap is coverage, not quality: one suite is outside ASan for a build reason.

## 11. R10 - the corpus and the gates

*The question*: does every normative sentence still describe the tree, and does every number name the command that
produced it?

### 11.1 Baseline measured this round

```
python3 scripts/measure_graphics_claims.py
  RenderEngine.hpp: 1244 lines; public signatures 59; included by 25 files
  SDK headers: 102; SDK bool returns: 156
  test_graphics: 687 cases / 85 suites; test_vk: 229 cases / 47 suites; ctest suites: 31
```

Gates: `check_design_docs`, `check_cross_refs`, `check_doc_symbols`, `check_api_style`,
`check_include_hygiene`, `check_public_docs`, `check_diagnostic_formats`, `check_export_annotations` and
`check_geom_layering` all exit 0; `check_ai_docs --strict` reports exactly one finding - this page's own index entry,
added with it.

### 11.2 Verified

- The seam list still matches the tree: `check_include_hygiene.py`'s `SEAM_SHARED_HEADERS` is 38 headers, computed from
  what `gfx_backend_vk` reaches, and its docstring now says "the shipped backend" rather than "both backends" - the one
  place where the retirement was folded into a rule rather than into a sentence.
- The two surviving "both backends" sentences are historical (`BC-33`, marked `<!-- drift-ok -->`) or belong to the
  retired backend's own page (`vsg-backend.md` F7), which is where history is allowed to live.
- The shadow policy has one home in prose as it does in code (`graphics-rendering.md:46-47`).

### 11.3 Z10 (corpus / low, part of `Z8`'s fix)

`graphics-navigation.md:123` (F5) says the arrows travel in `FirstPerson`; the code makes them look. This is the same
defect as `Z8` seen from the page side, and it is listed separately only so the fix batch closes the sentence as well as
the mapping.

> **Fixed 2026-11-27.** `F5` now states the TRAVEL keys (WASD horizontally, Q/E vertically) and `F2` the look's devices
> (the left-drag and the arrow keys), each key set in one place - and the code makes the arrows look, so the page and the
> mapping state one rule. See `graphics-navigation.md` 2.10 and `ADR-2026-11-27-a-look-is-one-verb-whatever-the-device`.

### 11.4 R10 verdict

The corpus is in the best state of any round: numbers either name their producer or are absent, the seam's rule survived
the retirement, and the one contradiction found is the navigation sentence `Z8` already names.

## 13. R11 - the compile and the pass-driven executor

*The question*: does the compile derive one frame from the declarations in one pass, and does a pass a host drives itself
reach the backend as the same value a whole frame reaches it as?

### 13.1 What the compile is, in one pass

`SessionShape::compile` (`EngineCompile.cpp:103-115`) borrows the frame and the device, runs `compileFrame`, reports
what it found once per episode, and drops both. `compileFrame` (`:117-200`) then does six things in an order that is
itself a dependency: transient targets, the declaration-only checks, the per-pass cost table, the schedule, one walk
over the run order that resolves each row's inputs and publishes its output name, and finally the frame's value. Nothing in it
publishes a scene (`buildFrameValue`'s note), so a publication that lands after the frame was built cannot change what
it draws - the property `Z7` and `Z2` depend on holding.

### 13.2 Z11 (medium-high) - "does this pass write its target" has three answers, and two of them are wrong (**fixed 2026-11-27**: `RenderPass::writesTarget()` is the one answer, read by the scheduler, the produced table and the wiring's filler check; a clear is a write - 2.16 / C9 / `ADR-2026-11-27-a-pass-says-whether-it-writes-its-target.md`)

The same question is asked in three places, and only one of them is the physical fact:

- What the pass **announces**: `RenderPass::describe` (`RenderPass.cpp:146-158`) returns `false` only for a pass with no
  scene; a pass with a scene and **no camera** fills the pass facts (`:153`) - target and attachment policy included -
  and returns `true` with no packet ("the pass announces what it is and where, and issues no draw", `:155`).
- What the **device** does: `claimPasses` skips only an entry whose `draw` is `nullopt` (`Device.cpp:1524-1525`), so
  such a pass is claimed and `target.begin(...)` is recorded with its load ops (`:2063`) - it **clears the target**, and
  the file says so in its own words at `:1530` ("a clear is the pass' own load-op, so a clear-only pass is a real pass
  with no draws").
- What the **scheduler** believes: `scheduleRows`' `writes` lambda (`EngineCompile.cpp:226-231`) answers
  `camera != nullptr && (content != nullptr || isClearEnabled())`, i.e. **false** for exactly that pass - and its comment
  states the opposite rule ("a clear is part of a draw, not a substitute for one").

The consequence is an order, not a sentence: a clearing pass adds no producer edge, while the structural check and
`recordProduced` both count its target as filled (`EngineCompile.cpp:925`, `:186-188`). A consumer that samples that
target can therefore be scheduled **before** the clear - the compile has already declared the target filled, so nothing
is reported - and it reads whatever the previous frame left. The reverse side of the same predicate: a pass with a
target, content and no camera clears with the **default** clear colour, because a scene pass defaults to clearing
(`RenderPass.cpp:83-86`), while the SDK's own sentence says such a pass "draws nothing" (`EngineCompile.cpp:428-430`).

Three fixes are each self-consistent and only one is true; the honest one is to make `writes` the announcement's
predicate (`describe` returns `true`) and to keep the clearing statement where it already is - or to make a camera-less
pass announce nothing, which changes `RenderPass::describe` and the SDK sentence together.

**Fixed 2026-11-27.** The announcement IS the physical fact, so it won: `RenderPass::writesTarget()` answers "does the
entry this pass announces change its target" for the declaration (a draw packet, or an announced attachment policy),
`passHasEffect` answered the same question of the value all along, and the two are compared shape by shape
(`PassScheduleTest.TheWriterPredicateIsTheShapeOfTheAnnouncement`). The scheduler's lambda and the fill checks'
`renderTarget() != nullptr` shortcut are deleted; a pass that announces nothing no longer fills its declared target, so
its reader is reported instead of reading last frame's picture.

### 13.3 Verified in this round

- The cost table is indexed by **slot**, not by run order (`:156-166` resets it, `:397-400` adds to it), so a pass the
  scheduler skipped stays at `0` and a pass can still be attributed after a plan change re-orders rows.
- A compile problem is re-emitted **whole** - code, category and subject survive the episode rule
  (`reportPlanProblems`, `:657-675`), which is review 8's "every problem became `ContentSkipped`" defect, fixed.
- `OutputIdentity::operator<` compares addresses through `std::less` (`:773-800`), so the keys are ordered without
  comparing unrelated pointers with the built-in `<`.
- The frame value's pass entries are reused (`:350-370`): the input list is assigned into, not rebuilt. One cost note,
  not a defect: a pass that **alternates** between issuing and issuing nothing frees its draw list (`entry.draw.reset()`,
  `:368`) and re-allocates it next frame, so the "must not free and re-allocate them 60 times a second" promise holds for
  a steady shape and not for an alternating one.

### 13.4 `RenderPass::execute` - one pass is a frame with one pass in it

`RenderPassExecute.cpp:11-39` publishes the scene (a pass driven by its owner publishes too), binds the publication into
the description, expands, and hands `singlePassFrame(...)` to the executor. The store is a local, and the only reader
that outlives the bind is `expandPass` - which runs before `executeFrame`, one synchronous call later. Verified: nothing
downstream of `executeFrame` reads the source's rows.

### 13.5 R11 verdict

The compile reads as one pass and its episode rule is honest. The finding is not in its structure but in **one predicate
stated three times**, and it is the only finding so far whose wrong answer is a picture rather than a report.

## 14. R12 - the scheduler's sort

*The question*: is the order a declaration's answer - dependencies first, announced order as the tie-break - and is a
component nobody can order left out without taking the rest of the frame with it?

### 14.1 Verified

- **Tarjan, iterative** (`PassScheduler::findCycles`, `PassScheduler.cpp:120-211`): the pass count is the host's number,
  so the DFS has an explicit stack (`dfs_stack_`) instead of the call stack; a tree edge updates the parent's low link at
  the pop, and a successor still on the stack relaxes through `index[next]` - the two rules a textbook version has.
- **A skipped component takes only itself out**: `orderAcyclic` (`:213-286`) counts in-degrees over the usable nodes
  only, so an edge into a skipped node is dropped rather than waited on - one cycle does not turn into a silently
  missing frame, which is the comment's stated rule and the code's behaviour.
- **Stable, not merely valid**: the ready set is a heap of `(announced order, registration index)`, which is the
  contract's stacking rule; two passes writing one target with no read relationship between them therefore keep
  registration order rather than an arbitrary one.
- **The feedback pattern is not a cycle**: `add_edges` skips `producer == consumer` (`EngineCompile.cpp:235-240`), so a
  pass reading what it draws into does not enter its own component.
- **Nothing allocates on a steady frame**: every table is a member filled in place, and the cycle path (a configuration
  error) may allocate while the ordinary frame does not.
- The cyclic component is reported once per episode with its members' names (`EngineCompile.cpp:290-306`).

### 14.2 R12 verdict

The sort is sound, and it is the one place in the module where an iterative rewrite of a recursive algorithm is not an
optimisation but a correctness constraint (a host may register a thousand passes). `Z11` is **upstream** of it: the graph
it sorts is missing an edge, and no sort can recover an edge the caller never added.

## 15. R13 - the content caches' fill path

*The question*: is a fill published only once it is provably complete, is a refusal remembered, and is one key filled
once?

### 15.1 Verified - the three-part answer

- **Three answers, three fills** (`ensureGeometry`, `ContentCaches.cpp:456-522`): `Use` fills the holes a release or a
  revision left, `Build` fills the cache's entry and keeps the ticket, `Copy` fills a detached entry the frame owns.
- **A refused build is never published**: the fill happens before the claim is recorded in `FrameBuilds`, so a failure
  leaves the ticket with the decision and the key returns to `Absent` at once instead of staying `Uploading`
  (`:494-510`) - `P1`'s prevention, not its cleanup.
- **Published on completion evidence, withdrawn otherwise**: `publishAll` runs after the fence reported completion
  (`Device.cpp:2276-2280`), and the destructor withdraws every claim that never got there (`ContentCaches.cpp:1529`).
- **Replace, never mutate**: `detachStaleStreams` (`:655-718`) and `failGeometry` (`:720-770`) move the entry out of the
  table and park the previous share, which is what keeps the park's `use_count` probe counting frames rather than the
  table's own share - the file states the reason, and the code follows it.
- **A program key cannot be reused by different text**: the entry holds `ShaderProgramDescriptionPtr` (`:380`), the same
  keep-alive the geometry entry uses (`ContentCaches.hpp:147`), so a recycled address cannot inherit a compiled module.
- **A failed compile is remembered** (`:424-428`), and a second ask answers `nullptr` without compiling again.

### 15.2 Z12 (medium) - an instance stream is keyed by a borrowed address with a counter that restarts (**fixed 2026-11-27**: the entry holds the `Buffer` its key is the address of, and is withdrawn at the frame end where that share is the last one - `vk-backend.md` 2.5 / N11, `ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end.md`)

`InstanceStream` is the one entry of the four that holds **no identity at all**
(`ContentCaches.hpp:155-163`): the key is the `Buffer`'s address (`ensureInstanceStream`,
`ContentCaches.cpp:318-350`), and the only version it compares is `Buffer::revision()` - which every fresh `Buffer`
starts at `0` (`Buffer.hpp:385`). Two facts follow:

* the entry **outlives the object it is keyed by** and nothing withdraws it (`instance_streams` loses an entry only on an
  explicit `releaseStreams(&buffer)`, `:2152-2162`), so a `Buffer<float>` allocated at a recycled address with the same
  revision is served the previous object's GPU bytes - silently, because the check `slot->revision == revision` passes;
* the entries that are never announced accumulate: one `VkBuffer` and one allocator slice per address the host stopped
  using.

The geometry path does not have this shape, and that is the proof it is a choice rather than a constraint: a vertex
stream's `source` is a `Buffer` the entry keeps alive **through the description it holds**
(`GeometryEntry::description`, `ContentCaches.hpp:147`), so its address cannot be recycled underneath it. The class
header calls the address "the identity the SDK releases by" (`ContentCaches.hpp:156-157`) - which is true, and is
exactly the assumption `Z6` shows the module cannot enforce on a host's behalf.

**Fixed 2026-11-27.** The instance stream was the one table that had not followed
`ADR-2026-11-09-address-keys-hold-the-object`, and it now does: `InstanceStream::source` holds the buffer its key is the
address of, so the key cannot be handed to a later object and `revision` compares versions of the object the bytes came
from (`ensureInstanceStream` takes the command's `BufferSlice`, so the share is given by its owner, not invented from a
borrowed reference). The review's second fact - the entries that were never announced - is why the fix could not stop
there: holding makes the cache an OWNER of the host's bytes, so an entry now ends two ways, the host's announcement and
the frame end at which its share is the last one (`sweepUnheldStreams`). The review's own prescription ("hold the
`Buffer` it was built from") was therefore not enough by itself: hold-only would have traded a stale `VkBuffer` for a
stale `VkBuffer` plus the host's memory, and the host pattern this table serves - rebuild the instance buffer each
frame - has no verb with which to announce. A geometry entry keeps the announcement-only rule, and `N11` says why: scene
content's residency is the host's decision and its rows leave the tree, while a `Buffer` has no row to say it is gone.

### 15.3 Z13 (low) - two of the three fills remember a refusal and the third does not

`ensureProgram` (`:371-430`) and `ensureGeometry` (`:494-497`) both keep a failed entry so the same broken content costs
one report and no second attempt; `ensureInstanceStream` (`:318-350`) keeps nothing, so a refused instance upload is
attempted - allocation, copy and refusal - on every frame that draws the command, and the report is only saved by the
episode rule at the device. Same question, two spellings.

**Fixed 2026-11-27, by making the third fill the same as the other two.** `InstanceStream::failed` records the revision
an upload failed at, so a broken stream answers null from then on - one report, no second allocation or copy - and a NEW
revision clears the flag because the bytes that failed are no longer the bytes under the key. The empty-view refusal is
remembered the same way. Case: `VkResidenceWireTest.ARefusedInstanceUploadIsRememberedInsteadOfRetriedEveryFrame` (with
block creation refused, the second frame does not reach `vkAllocateMemory`; a bumped revision retries); rule: 2.5 /
`D11` / `T13` in `../design/vk-backend.md`.

### 15.4 R13 verdict

The residency machinery is the most carefully argued part of the backend and the three answers survive reading line by
line. Both findings are about the **one** entry that does not hold what it is keyed by - the shape `Z6` already names on
the engine's side, met twice in one module.

## 16. R14 - the window's swapchain machine

*The question*: can every state the swapchain can be in be entered and left exactly once - acquire, record, present, and
the failure between them - without a stale semaphore or a stale layout surviving?

### 16.1 Verified

- **The record and the barrier are one call**: `transitionImage` takes the record **by reference** and writes it
  (`BarrierPlan.cpp:171-188`), so `recordPresentTransition`'s promise ("the acquired image is then in the present
  layout") is kept by the callee, and `beginPass` cannot start from a layout the image is not in.
- **One present-wait semaphore per image** (`WindowTarget.cpp:92-107`): signalling one semaphore again while the
  presentation engine still holds it is `VUID-vkQueueSubmit-pSignalSemaphores-00067`, and the per-image vector is the
  repair the comment says was measured in the app.
- **The acquire semaphore is rebuilt with the chain** (`:123-143`), which is what makes `abandonFrame`'s "give the image
  back without presenting it" a real repair: a signalled semaphore is never handed to `vkAcquireNextImageKHR` again
  (`VUID-vkAcquireNextImageKHR-semaphore-01286`).
- **An out-of-date acquire rebuilds once per call, and a lost device never rebuilds** (`acquire`, `:509-575`): the two
  answers are classified apart, and the target only remembers `device_lost` - the session-level sentence is the device's.
- **Every teardown goes through the idle wait** (`destroySwapchainWhenIdle`, `:244-260`), so a host verb and a session
  teardown share one proof instead of two.
- The `PresentOutcome` contract already carries "out of date" inside `Lost` (`PresentTarget.hpp:26-36`), so the swapchain
  answering `Lost` on `VK_ERROR_OUT_OF_DATE_KHR` is the stated meaning, not a misclassification.

### 16.2 Z14 (low) - one sentence, two causes, and the false one names the host

`claimPasses` reports `vk.window.presenter_missing` for `window == nullptr || !window->ready()` (`Device.cpp:1550-1560`)
with the sentence "a window pass names no presenter this device handed out". The second condition is not that: a window
the target itself failed to rebuild (a zero-extent surface, a refused `vkCreateSwapchainKHR`) leaves `ready()` false
while the presenter is exactly the one the device handed out (`WindowTarget.cpp:494-497`). The window's own `why` is
never reported on this path - `acquire` is not even attempted - so a host is told it passed a foreign presenter when the
picture is missing for a reason the target knows and does not say.

**Fixed 2026-11-27, by making the window answer its own state.** `WindowTarget::presentationState()` is the window's own
value (`Ready` / `Detached` / `NotAttached` / `NoSwapchain`), and the frame's window branch has three answers instead of
two: no presenter keeps `vk.window.presenter_missing`, an unready presenter reports `vk.window.not_ready` with the state
it names, and a live chain re-arms both episodes. The cause of the missing chain (if any) was already reported where it
happened (`attach_failed` / `acquire_failed`), so the new sentence says so instead of repeating it. Case:
`VkWindowPresentTest.ADetachedPresenterAnswersItsOwnReasonInsteadOfAccusingTheHost` (a detached target reports
`not_ready`, never `presenter_missing`); rule: F3 / F14 / `D12` / `T14` in `../design/vk-backend.md`.

### 16.3 R14 verdict

The state machine is sound, including the two paths that are easiest to get wrong (a failed acquire and a failed
present), and it is the first round whose only finding is a sentence rather than a mechanism.

## 17. R15 - the image cache's table and its staging batches

*The question*: does a texture become resident only when its copy is proven, does a released texture leave nothing
behind, and can one texture's upload ever be mistaken for another's?

### 17.1 What the cache is, in one pass

`ContentImages` (291 header lines + 925) holds four facts and one ledger per driver:

- The **table** (`textures`) is keyed by the `Texture`'s address and each entry holds an `intrusive_ptr<const Texture>`
  (`ContentImages.cpp:110`), so the key cannot be recycled by a later object. This is the shape `Z12` reports missing on
  the instance stream, and it is present here.
- The **upload** runs `viewFor` (`:688-756`) -> `createTextureImage` (`:268-354`) -> `recordUpload` (`:356-428`): one
  barrier into `TRANSFER_DST` (`:372-373`), one staging buffer per LAYER created inside the copy loop (`:375-402`), one
  copy per layer and mip (`:421`), then one barrier into `SHADER_READ_ONLY` (`:746-747`). The entry joins the batch's
  `pending` (`:754`) and serves only the driver that recorded it (`:707-712`).
- The **publication** is `batchCompleted` (`:588-633`): pending entries are promoted, the resident entry a promotion
  replaces is parked, the batch's staging buffers are destroyed, and the retired entries are parked.
- The **withdrawal** is `batchAbandoned` (`:635-674`): pending entries, the stand-in and the staging buffers are parked,
  and the resident revision a promotion would have replaced stays in the table.
- The **host release** is `releaseTexture` (`:547-586`): the resident entry and any pending copy of that texture are
  parked, so a promotion cannot resurrect a texture the host let go.

### 17.2 Verified in this round

- A recycled address cannot inherit bytes: the entry holds the object, so the key outlives every other reference.
- `bytes_total` moves exactly once per promotion, including two pending revisions of one texture in one batch (the
  second promotion subtracts what the first one added, `:600-618`).
- `batchAbandoned` publishes nothing, so the class of defect `T66` records cannot recur for a failed submit, and the
  resident entry a promotion would have replaced remains the last proven revision.
- The `batches` map cannot grow: `endFrame(batch, completed)` is called on every path that records - command buffer end
  refused (`Device.cpp:2155`), submitted (`:2283`), warm-up (`:2541`) - and `uploadBatch()` is `clock_window + 1`
  (`Device.cpp:549-552`), unique for as long as the driver lives.
- The stand-in's failure is not shared: a batch that never ran drops it (`:657-663`), which is what keeps a black
  stand-in from outliving the frame that failed to fill it.

### 17.3 Z15 (medium) - a mid-record failure destroys an image the command buffer already names

`recordUpload` allocates each layer's staging buffer INSIDE the copy loop, so the first layers are already recorded when
a later one fails (`:375-402`, with the copy at `:421`). The failure returns false, and `viewFor` answers with

    impl->destroyEntry(*entry);   // :731-733

which is `vkDestroyImage` **immediately** - while the caller's command buffer still holds the barrier into
`TRANSFER_DST` (`:372-373`) and the copies of layers `0..k-1`. `ensureFallback` has the same shape with one more step:
the barrier, the copy and the second barrier are recorded (`:878-891`) before the view is created, and a refused
`vkCreateImageView` (`:901`) destroys the image those commands name (`:903`).

The frame then ends and submits that command buffer, so the submission names a destroyed image. Every other release in
this cache goes through `releaseLater` (`:235-243`), which is what `P17`'s premise - "a submit that failed may still
have executed part of the command buffer" - exists for; these two paths are the only ones that destroy an object out of
hand.

The failure needs an allocation refusal, which is why it is a defect of the failure path and not of the happy one - and
why it is cheap in the two ways the file already shows: create every layer's staging buffer BEFORE recording any copy,
or park the entry against the batch (`releaseLater`) instead of destroying it.

**Fixed 2026-11-27, at the cause rather than at either branch.** The interleaving WAS the defect, so the upload now has
two halves with opposite contracts: `Impl::prepareUpload` builds the image's bytes - ONE host-visible buffer holding
every layer and mip plus the region array, one allocation and one mapping - and records nothing, so its refusal destroys
what it built and leaves the caller's command buffer untouched; `Impl::recordUpload` states the two transitions and ONE
`vkCmdCopyBufferToImage` carrying every region, hands the staging buffer to the batch, and answers `void`, because a
command the driver already holds cannot be taken back. The stand-in takes the same road (its view is created BEFORE the
copy, which is legal because a view does not need content), so the second half of the finding - a refused
`vkCreateImageView` destroying the image the three recorded commands name - is gone with the first.

The review's parking branch was rejected as the next defect: it keeps `recordUpload` reading as "may fail halfway" and
leaves the batch holding staging for a copy whose destination nobody publishes (`D13`). The upload's barriers also carry
the frame's `RecordingTally` now, which is what lets the case read "a refused upload records nothing" as a number rather
than as a comment. The validation layer turned out not to be the witness either: installed later the same day
(1.4.341), the mutation produces no message at all - the destroy precedes the submission, and the layer checks only
submitted references. Rule: `vk-backend.md` 2.23 / C16 / P21 / T15;
`ADR-2026-11-27-an-upload-is-prepared-then-recorded.md`.

### 17.4 No second finding, one caveat

- `samplers` (`:758-797`) has no eviction: one `VkSampler` per distinct `Sampler` value, kept for the cache's life. A
  `Sampler` carries `min_lod` / `max_lod` / `lod_bias`, so a host that animates a bias creates one sampler - and one
  linear scan - per frame. Its growth is bounded by how a host uses it rather than by a reused key, so it is a trigger
  ("a host that animates a sampler's lod bias") rather than a finding.

## 18. R16 - dynamic rendering and the target record

*The question*: does one pass' attachment set reach the command buffer as the values the record states, and does every
layout record get written by the one command that moved the image?

### 18.1 What the pair is, in one pass

`PassAttachmentSet` (`DynamicRendering.hpp:171-190`) states the images of one instance as four roles - written, resolved
into, depth attachment, depth resolve - each with the layout its record holds; `beginRendering`
(`DynamicRendering.cpp:100-193`) records every transition the pass needs as ONE `VkDependencyInfo`, then states the
attachment infos and opens the instance. `TargetObjects` (`576` header lines + 538) is the record that owns the images,
their memory and their views, and is the *producer* of those roles: `beginPass` (`TargetObjects.cpp:401-476`) decides
which image plays which role from two shape facts, calls `beginRendering`, and then writes the records the call just
followed.

### 18.2 Verified in this round

- The bookkeeping cannot disagree with the barriers: the roles' `from` spans ARE the record's vectors
  (`TargetObjects.cpp:410-411`), and the writes that follow `beginRendering` (`:466-475`) happen strictly after it read
  them, so one command moved an image and one assignment recorded it.
- A rebuild never overwrites a live handle: `TargetTable::ensure`'s `Rebuild` branch calls `destroyTarget` before
  `buildTarget` (`TargetTable.cpp:436-445`), and `ResizeInPlace` goes through `resizeTarget`, which strips the
  extents-bound half first (`:423-435`).
- The depth picture's record is single-homed: `setDepthAttachmentLayout` / `setDepthPictureLayout` (`TargetObjects.hpp`)
  write through the borrow indirection, so `resetLayouts` (`TargetObjects.cpp:479-497`) rewinds the LENDER's record for
  the lender's image instead of a field nobody reads - and it does not touch a lender's colour attachments.
- The store downgrade is derived where the resolve is known, not restated by callers: `colour_resolved` /
  `depth_resolved` (`DynamicRendering.cpp:161-163`), which is why a caller states what its images are worth and cannot
  get the resolve case wrong.
- The depth clear is the reverse-Z far plane and the SDK states exactly one convention: `DepthPolicy` is test/write
  only (`DepthPolicy.hpp:23-28`), so there is no second clear value to pick from.
- The window path is a second producer of the same value and keeps the same rule: `WindowTarget::beginPass`
  (`WindowTarget.cpp:571-609`) forces `Store` on the presented attachment and states one-entry roles.

### 18.3 No finding; two caveats

- `DynamicRendering.cpp:107` sizes the barrier batch at `kMaxColorAttachments * 2U + 2U` = 18, which is exactly the
  largest possible append count (8 colour + 8 picture + depth + depth picture), and `append` (`:109-117`) does not
  check `barrier_count`. It is correct today because `TargetTable::ensure` refuses more than eight colour attachments
  (`TargetTable.cpp:340-353`) and `picture.size() == color.size()`; one more role would silently overflow a stack
  array. A bound check costs one comparison per append.
- The two roles answer a short span differently: `PassRole::viewAt` clamps (`DynamicRendering.hpp:151-154`) and
  `beginRendering` indexes `attachments.color.views[index]` directly (`DynamicRendering.cpp:153`). Both producers size
  the three spans together, so this is an unwritten precondition in a free function rather than a live defect - but it
  is the kind of precondition a second producer is entitled to see enforced.

## 19. R17 - shader compilation and its reflection

*The question*: is a compiled stage's answer about what it reads correct, and is a program the compiler could not build
refused rather than cached as built?

### 19.1 What it is, in one pass

`compileStage` (`ShaderCompiler.cpp:267-275`) runs one GLSL stage through the in-tree glslang: one process-wide
initialization, a preamble of `#define <name> 1` for the variant's names, the Vulkan 1.0 / SPIR-V 1.0 environment, and
the optimizer deliberately off. `describeInputLocations` (`:67-170`) then reads the words back and answers which vertex
input locations the stage declares; `ensureProgram` (`ContentCaches.cpp:376-431`) compiles every stage of a program,
creates the modules, keeps the vertex answer, and caches the entry.

### 19.2 Verified: the reflection is correct, and conservative in the safe direction

- The opcode table is right (`ShaderCompiler.cpp:73-97`): bool/int/float/vector occupy one location, a matrix occupies
  one per column (`:123-133`), an input variable names a pointer to its type so the indirection is followed
  (`:119-122`, `:152-156`), and `Location` decorations are collected independently of instruction order (`:108-111`).
- Every unreadable shape leaves `input_locations_known == false` rather than a partial answer: a truncated module
  (`:103-106`), an input whose type this function cannot size (`:157-160`). `UNKNOWN MEANS YES`
  (`ShaderCompiler.hpp:39`) is then the caller's rule.
- The one consumer narrows only on a proven answer (`ContentCaches.cpp:1075`, `:1096`), and the retained bindings keep
  the geometry channel's own index, which is the index the draw binds (`ContentDraw.cpp:558-569`).
- glslang's `setEntryPoint` copies its argument (it reaches `TIntermediate::setEntryPointName`, which assigns to a
  `std::string`: `build/_deps/glslang-src/glslang/MachineIndependent/localintermediate.h:482-487`), so the temporary
  from `as_std_str()` at `ShaderCompiler.cpp:214` is safe.

### 19.3 Z16 (medium-low) - a program the backend compiled nothing for is cached as compiled (**fixed 2026-11-27**: the requirement moved to `buildPipeline`, the missing stage has its own code, and a compute stage is refused instead of skipped - `vk-backend.md` 2.22)

`ensureProgram` walks the description's stages and `continue`s on a compute stage (`ContentCaches.cpp:383-386`, with the
comment "M2 has no compute path"). A description with no vertex and no fragment module therefore leaves the loop without
a failure, is placed in `programs` (`:424-427`) and is returned as usable (`:429`).

Nothing else refuses it: the pipeline build states both module handles unconditionally (`:852`, `:856`), and
`vkCreateGraphicsPipelines` requires a valid module for every stage it is given. The inputs are the SDK's own verbs -
`ShaderProgram::clearStages` (`ShaderProgram.cpp:77-81`) leaves zero stages, `addStage` accepts a compute stage - and
nothing between the SDK and `ensureProgram` asks whether a graphics program has a vertex stage: the only reader of
`stages()` in the whole tree is `ContentCaches.cpp:383`.

So a program that is empty, fragment-only or compute-only compiles "successfully", and the next draw hands
`VK_NULL_HANDLE` to the driver - an invalid-handle call rather than a refusal, which is the one thing the module's
`ContentRefusal` vocabulary exists to prevent. The fix belongs where the entry is decided: a cached entry with a null
module is `failed` with a refusal code, and the existing episode rule says it once.

> **Fixed 2026-11-27, and the second half of the finding was wrong.** `ensureContentPipeline` and
> `ensureFullscreenPipeline` had refused a pair with a null module since before this review, so no invalid handle ever
> reached the driver; the reviewer read `buildPipeline` and did not see the two guards one level up. What WAS wrong is
> what the batch fixed: (1) the requirement was stated twice, in two `ensure*` functions, nowhere at the one place that
> states modules to the API, and neither sentence named the absent stage; (2) the refusal arrived as
> `vk.content.pipeline_refused` - "vkCreateGraphicsPipelines refused the state the draw resolves to" - which is false and
> sends a host to blend/depth state; (3) a `Compute` stage was `continue`d past, so a compute-only program was cached as
> compiled with no modules and reported as a missing GRAPHICS stage.
>
> The rule now lives in `buildPipeline` (the only site that hands modules to `vkCreateGraphicsPipelines`), which refuses
> a stage pair with a hole, names the stage, and answers which failure it was - `ProgramStageMissing` or
> `DriverRefused` - so a program fault keeps its own code (`vk.content.program_stage_missing`) and a state the driver
> refused keeps `pipeline_refused` / `screen_pipeline_refused`. A compute stage is refused at the entry. A program stays a
> VALUE: nothing about stage completeness is cached on `ProgramEntry`, because a fragment-only program is a whole
> full-screen program.
>
> **The finding's own remedy was rejected.** Failing the program entry would refuse a legal full-screen draw for the rest
> of the session (that path states its own vertex stage), and a verdict per (program, need) would be two answers under
> one key. See `ADR-2026-11-27-a-graphics-pipeline-states-two-stages`.

### 19.4 Two caveats

- `setPreamble` stores the **pointer** it is given (`build/_deps/glslang-src/glslang/Public/ShaderLang.h:478`), so what
  keeps the preamble alive across `parse` is that `compileWithDefines` declares its `std::string preamble`
  (`ShaderCompiler.cpp:221-227`) in the same scope. Nothing states that; moving `parse` out of the scope would dangle.
- `describeInputLocations` runs for every stage, so a FRAGMENT module's `input_locations` holds its varyings' locations
  while the field is documented as "every vertex-input location the module DECLARES" (`ShaderCompiler.hpp:29`). Only the
  vertex answer is kept (`ContentCaches.cpp:405-409`), so it is a name/doc mismatch rather than a live defect.

## 20. R18 - the host half

*The question*: does the host run one per-frame order at every entry point that drives a frame, and can a state the
surface owns get stuck on a transition it never sees?

### 20.1 The per-frame order, in one pass

`SurfaceWindow::renderFrame` (`:867-927`) is the tree's only `engine->frame()` call site (grep: one hit). Its order is
observe -> update -> publish -> build: `observePresentRecord()` (`:897`) reads the PREVIOUS frame's answer and points the
view's queries at the camera the user can see; `primary->update(d->engine->frameContext())` (`:905`);
`primary->publishContent()` (`:906`); `d->engine->frame()` (`:909`). A session's first picture is waited for and
`Presenting` is published on an ANSWERED frame (`:911-922`).

Every other driver funnels through here: the continuous timer (`:508`), `QEvent::UpdateRequest` (`:1015-1018`), and the
three input handlers (`:595`, `:604`, `:613`). So "update, then publish, then build" holds at every entry point, and
there is no second route to a frame that could skip the prefix - which is the property the demo's own warm-up had to be
taught (`DemoScene.cpp:1741` publishes before its first `prepare()` for exactly this reason).

### 20.2 Z17 (medium-low) - the drag capture has no release it cannot see

A press records the capture (`SurfaceWindow.cpp:566-571`) and the manipulator records the drag
(`OrbitCameraManipulator.cpp:424-430`). Only a release clears them (`SurfaceWindow.cpp:586-596`,
`OrbitCameraManipulator.cpp:469-473`), and the router sends every button-less event to `onMouseMove`
(`InputRouter.cpp:13-15`) regardless of what the manipulator believes.

Nothing else clears a held button: `handleDestroyed` (`:616-...)` does not, `event()`'s switch (`:998-1024`) has no
focus or deactivate case, `init()` does not, and `held_buttons` / `captured_views` survive a recreated platform window.

So a release the surface never sees - a window deactivated mid-drag, or a platform window recreated mid-drag, which this
file documents as normal (a layout change, a screen change, a reparent) and deliberately triggers through the
`VN_RECREATE_SURFACE_MS` hatch (`:735-737`) - leaves `held_buttons` set and `drag_` non-None for the rest of the
session: every later hover repaints (`:595`) and rotates or pans the camera.

The failure is exactly the one the per-button capture exists to prevent, one indirection further out: the layout comment
says "a view that never saw its release would keep steering its camera on pure hover afterwards", and per-button capture
only covers a release that ARRIVES. The fix is a synthesised release for every held button on the two transitions the
surface already handles - `handleDestroyed`, and a focus-out / deactivate case in `event()`.

**Declined 2026-11-27** by the owner: not taken (see §27.2 item 14). The finding stands as described below; what changes
is that the round does not act on it.

### 20.3 Z18 (low) - three application-thread waits, and only one of them is bounded

`SceneView::applySurfaceGeometry` bounds its wait at 250 ms (`SceneView.cpp:163`) and states why: "this runs on the
application thread: a frame that never answers must not freeze it for the engine's default five seconds".

The same thread waits with no bound at `SurfaceWindow.cpp:744` (attach) and `:915` (a session's first picture), both
discarded with `(void)` and both taking the default 5000 ms (`RenderEngine.hpp:543`, `:550`). A hung device or a wedged
frame thread therefore freezes the GUI for five seconds per attach and per first frame, and the `(void)` hides it - the
one rule the view states is not taken by the two sites that run most often.

**Declined 2026-11-27** by the owner: not taken (see §27.2 item 14). The finding stands as described; the design page
no longer generalises the bound, so the corpus stops claiming a rule these two sites never took.

### 20.4 What the host already gets right

- The presented camera is a SNAPSHOT of the last answered frame, not the live one (`SurfaceWindow.cpp:930-940`), which
  is the design's `N17` and the reason a low frame rate cannot make a click aim at a pose nobody has seen.
- `frameContext()` is called ONCE and handed to the whole update (`:905`), which is the order the overlays need - and it
  is also the evidence that `Z4` is a sentence rather than a mechanism: the only shipped consumer already treats the
  value as a snapshot.

## 22. R19 - the memory tier and the eight small private units

*The question*: does every slice have one owner, one granularity and one release - and does a unit that can refuse answer
with a value rather than a sentence?

### 22.1 What the tier is, in one pass

`MemoryAllocator` (`MemoryAllocator.cpp:112-602`) is the backend's only owner of `VkDeviceMemory`: a block per (memory
type, usage) and, inside a block, one list of ranges sorted by offset - and therefore always alternating live and free -
so a release looks at two neighbours and merges, and an allocation is a first fit with no search (`:126-158`,
`:500-566`). A caller states a PURPOSE (`MemoryUsage`) and a KIND (`MemoryKind`), never property bits: the purpose is
what the code knows, and the type that serves it is the device's answer. `placeAt` (`:219-254`) applies the resource's
alignment and the `bufferImageGranularity` rule against a neighbour of the *other* kind only, and `takeAt` (`:256`)
restores the alternation in its three shapes. `createBlock` (`:283`) caps its first-fill size at an eighth of the heap,
sizes an oversized request its own block, and is the unit's only device call - behind the injectable
`MemoryBlockCreator`. `close` (`:357`) unmaps, frees and zeroes the books. `FrameScratch` is the per-driver ring and
descriptor pool; `ReadbackStaging` is one copy's own command pool, fence, buffer and mapping; `ResidencyClaims` is the
two-driver table; and the four small units are `PipelineCacheFile` (magic + format + key + exact blob length,
`PipelineCacheFile.cpp:95-142`, `.tmp` + rename on write, `:172`), `ContentRefusal` (an enum plus a switch without
`default`, so a new cause does not compile until it has a code), `StateMapping` (a pure table whose two non-obvious rows
state the reverse-Z comparison and the winding convention, the latter *measured* by
`VkContentDrawTest.TheFragmentStageSeesTheSdksFrontFace`) and `WindowSystem` (one xcb connection and one device-owned
X11 window per session, both `dlopen`-ed, every failure answered with a reason, `WindowSystem.cpp:127`, `:194`).

### 22.2 Verified in this round

- `MemoryAllocator`'s placement rule is the spec's, not a heuristic: the granularity check exists *only* between ranges
  of different kind (`:219-254`), which is the one case `VkPhysicalDeviceLimits::bufferImageGranularity` forbids sharing;
  a block is destroyed only when it is entirely free AND another of its type remains (`:546-562`), so "build, release,
  build" does not allocate twice and a one-off large resource does not hold memory for the session.
- Refusals are classified where they are made: `refusalOf` (`:98`) splits a lost device from an exhausted heap, the raw
  `VkResult` travels in `MemoryRefusalReport`, and the sink is the ONE consumer (`:422-433`) - so no allocation site can
  drop the reason, which is the defect the wiring removes.
- `ResidencyClaims` is a compare-and-set under one small lock, its tokens are monotonic and never reused, a stale
  publish/withdraw is counted rather than ignored, and `Claim`'s destructor withdraws (no `Uploading` entry can be left
  behind). Three real orderings - a claim whose frame died after `forget`, a claim already withdrawn, a newer claim after
  either - are distinguishable, which is what makes "publish or withdraw, no third path" true rather than aspirational.
- `FrameScratch` grows the ring only between frames (`ensureRing` at `:364`, called from `create` and `beginFrame`), drops
  the set cache before replacing the buffer, and keys a cached set by views AND samplers AND the shadow pair AND which
  layout it was allocated against (`:476`) - a key that omitted the samplers used to serve one material's sampler to
  another, silently.
- `ReadbackStaging` keeps the three results apart (`StagingRun`), because a lost device on the submit is a session fact
  and an `end` that failed never reached the queue; the submit takes the device's lock and the wait does not
  (`:120-131`); `Unsupported` (no coherent type) is not `Failed`.
- `PipelineCacheFile` refuses a foreign device, a cut-off blob and trailing bytes with three different reasons; native
  byte order is refused rather than guessed, which is the only safe answer for a blob the driver owns.
- `WindowSystem`'s close order is the one its header states - window targets, then the own window, then the connection -
  and `Device::shutdown` obeys it: the targets drain and are destroyed (`Device.cpp:3442-3448`), the window targets go with
  the session's own objects, and the window system closes last (`:3492-3497`).

### 22.3 No finding; five caveats and two mangled lines

The memory tier is the second mechanism in this page whose comments and code agree at every point checked. What is left
is five caveats, none of which is a live defect today:

- **The header denies the lock the code takes.** `MemoryAllocator.hpp:210-211` warns "Not thread-safe: one frame at a
  time", while every verb takes `impl->lock` (`MemoryAllocator.cpp:112-115`) and that lock's own comment says two frame
  drivers allocate from the pool on two threads (S6c-2). The code is the right one; a reader who trusts the warning adds
  a mutex of their own, and one who trusts the code is right. One of the two sentences should go.
- **The failure sink runs under the pool's mutex.** `allocate` calls it inside the lock (`:422-433`), and the device's
  sink ends in `reportDiagnostic` and possibly `noteDeviceLost` (`Device.cpp:3546-3617`) - host code, under the pool's
  lock. What makes that safe is that every device verb a sink could call takes `body_gate` exclusively and refuses on the
  frame thread (`Device.cpp:4326-4335`), i.e. a rule stated nowhere in the sink's contract (`setFailureSink`'s note
  says only "must outlive this allocator's allocations").
- **`map` admits any HOST_VISIBLE type, and the unit never flushes.** The guard is `HOST_VISIBLE` alone
  (`:583`) while the coherence the caller relies on comes from the *usage* `MemoryUsage::HostVisible` requires. All five
  call sites allocate with that usage, so every mapping made today is coherent by construction; a future caller that maps
  a `DeviceLocal` slice on a device whose device-local type is host-visible but not coherent gets a pointer whose writes
  the GPU may never see.
- **A second `open` on a readback leaks the first.** `ReadbackStaging::open` assigns its borrowed handles and then creates
  into `pool_` / `fence_` / `buffer_` without releasing what an earlier call left (`ReadbackStaging.cpp:12-30`). Both
  call sites construct a fresh object (`Device.cpp:4112`, `:4229`), and the header documents "safe after a failed open"
  but never "call once".
- **The set cache has a ceiling and no eviction on demand.** The pool holds `kMaxCachedSets = 1024` sets
  (`ContentAbi.hpp:41`) and `sweepSets` is time-based only - every 120 frames, freeing what nothing used for 600
  (`FrameScratch.cpp:427-434`). A host with more than 1024 distinct (views, samplers, shadow) keys inside that window
  loses those draws, reported per draw as `DescriptorSetMissing` (a `ContentSkipped`), and does not recover while the keys
  stay in use. That is a stated policy ("hundreds of materials still fit"), not an accident - but it is a cap with a
  trigger, which is what §17.4 records for the sampler table and this page does not record here.
- **Two mangled lines in one file.** `MemoryAllocator.cpp:112` begins `///\n    /// THE POOL'S LOCK` - a literal
  backslash-n written into a comment instead of a line break - and `:317` reads `}        blocks.push_back(...)`, two
  statements on one line. They are the backend's only two (`grep -rn '\\n' src/plugins/gfx_backend_vk | grep -v '"'`
  answers one hit), and one whitespace edit fixes both.

## 23. R20 - Device.cpp outside the pass walk

*The question*: does every device-level verb answer once, from one place - and when a frame fails, is the answer about
that frame?

### 23.1 What was read, and what it is for

The 4,400 lines outside the pass walk are four jobs, each with one home: **submit and present** (`executeFrame`,
`Device.cpp:1752-2314`), **the warm-up** (`prepareFrame`, `:2317-2560`), **one device's life** (`initialize`,
`:2664-3388`, and `shutdown`, `:3390-3502`), and **the host verbs that touch the device alone** (`createFrameDriver`,
`releaseFrameDriver`, `setRenderTarget`, `releaseRenderTarget`, `createSurfaceTarget`, `releaseContent`, the two
readbacks, `:3653-4256`). The pass walk inside `executeFrame` was read in §13; what this round added is everything
before the first `vkCmdPipelineBarrier2` and after the last `vkCmdEndRendering`.

### 23.2 Verified in this round

- **Submit, wait, reset.** One `vkQueueSubmit2` under `queue_submit`, the submission's number minted inside the same lock
  (`:2192-2209`), the fence waited outside it (`:2219`), and the number proven at once when the queue refused the buffer
  (`:2212-2219`) - so the park queue's watermark can never wait for a submission that does not exist. The one
  classification of a lost device is `noteDeviceLost` (`:3522-3544`), which reports once per session and survives its own
  diagnostic sink.
- **A frame that never reached the queue moves nothing.** `rewindUnrunFrame` (`:1969-1987`) puts the two kinds of record
  back by their own rule: this frame's attachment sets to `Undefined` (the honest and safe statement) and someone else's
  pictures to exactly what they said - and it gives the acquired window image back, because an acquire with no present is
  a leak of the presentation engine's image.
- **Shutdown is one order, and it is the order the objects state.** `vkDeviceWaitIdle`; unset the park seam then run the
  queue out of turn; write the pipeline cache file while the cache object still exists; destroy the extra drivers, the
  device's scratch, the caches, then drain the target table and destroy each record, then the window targets, then the own
  driver, then the allocator, then the device, the messenger, the instance and the window system (`:3390-3502`). The one
  place that could destroy a record twice does not: `TargetTable::drain()` *returns* its records and clears the claims
  (`TargetTable.cpp:511-528`), so `shutdown` is the single destroyer.
- **`initialize` is idempotent and its failures unwind.** A device already up answers true without tearing anything down
  (`:2670-2674`), a lost session is shut down first (`:2668`), and every refusal after the instance exists ends in
  `shutdown()` - including the two drivers' bring-up, which is why an engine that attached before the device still gets
  its command machinery (`:3360-3382`).
- **A readback is a snapshot, then an authoritative look.** `readColorImage` refuses everything a snapshot can answer
  *before* it makes the device wait (build, index, freshness, texel width, host-visible support), takes the gate, counts
  one device stop, and then re-checks the same facts under the gate against `target_table.lookup` - so a target rebuilt
  while the verb waited cannot be read from a half-invalid record (`:4010-4131`). The depth half has the same shape and
  refuses a *borrowed* depth as `Invalid` rather than "this device cannot read depth" (`:4135-4256`).

### 23.3 Z19 (medium-low) - the frame that did not run cannot answer `Lost`

`executeFrame` ends by presenting: `outcome = completed ? recording.window->present() : PresentOutcome::Lost`
(`:2304`), inside `if (recording.window_acquired && recording.window != nullptr)` (`:2303`). That `Lost` branch is
**unreachable**, and the give-back is what makes it so:

1. `!completed` runs `rewindUnrunFrame()` (`:2291`), which calls `giveBackWindowImage()` (`:1889-1893`).
2. `giveBackWindowImage` sets `recording.window_acquired = false` (`:1892`).
3. The present block's condition is therefore false exactly on the path that would take the `Lost` branch.

So a window frame whose submit or fence failed answers `Skipped`. Two consumers act only on `Lost`: the engine
republishes the extent so the swapchain is rebuilt (`RenderEngine.cpp:620-622`) and the surface logs the lost picture
once (`SurfaceWindow.cpp:941-957`). Neither runs for the one failure a host must act on; the diagnostic and
`initialized() == false` still tell the truth, so the host is not blind - but the answer designed for it is never
produced, and the line that expresses it is dead code that reads as if it worked.

The fix is one captured fact: read `recording.window_acquired` into a local before `rewindUnrunFrame()` and answer
`Lost` from it, so the give-back releases the image without erasing the fact that a picture was expected.

### 23.4 Z20 (low) - a frame that did not complete keeps the previous frame's device time

`books.timing` is reset on exactly two paths, and both sit inside `if (completed)` (`:2222`): a sampled frame that read
the query pairs, and a frame the clock did not sample (`:2266`, `:2270`). The sentence right beside them is explicit:
"A frame the clock did not sample answers DISENGAGED - never the previous frame's value" (`:2231-2233`).

A frame that fails its submit or fence never enters that block, so `closeFrameBooks` (`:1431-1457`) copies whatever
`books.timing` still holds into `answered_books` - and `frameReport` hands the previous sampled frame's GPU time and
per-pass costs to a host asking about the frame that just failed (`:2565-2580`). `books` is cleared only by
`FrameDriver::destroy()` (`:1398`), so nothing else empties it. The fix is the same one line on the `!completed` path, or
a reset at frame start the way `recording.clear()` resets the frame's own state.

### 23.5 Three more caveats

- **Every device-side wait is unbounded.** The frame's fence (`:2219`), the warm-up's (`:2509`) and the readback's
  (`ReadbackStaging.cpp:130`) all pass `UINT64_MAX`, and the readback's runs on the application thread under the gate. A
  device that never signals blocks the caller for ever, with no report - the same family as `Z18`, one layer lower, and
  the only waits in the tree that no timeout reaches.
- **The warm-up's comment denies the retry the code gives.** `prepareFrame` says of a batch whose submit failed "They
  cannot buy a re-upload, though - the entries were already made resident with what the copies were going to write"
  (`:2523-2528`). `ContentCaches::endFrame` with `completed == false` calls `batchAbandoned`, which parks and drops the
  pending entries (`ContentImages.cpp:635-690`), and the next frame uploads them again - which is what
  `VkPrepareUploadTest.APrepareWhoseSubmitFailsPublishesNothingAndTheNextFrameUploadsAgain` asserts. The report's
  sentence ("the images it was going to fill were never written") is right; the reason given for it is wrong.
- **The frame watermarks restart with a rebuild.** `FrameProgress` is documented lifetime-cumulative - "a watermark that
  could go backwards would be a lie" (`FrameReport.hpp:57-58`) - while `FrameDriver::destroy()` resets the books
  (`Device.cpp:1398`), and `shutdown()` -> `initialize()` destroys the driver's own. A host that waits for a *number*
  across a rebuild sees it fall to zero. No shipped consumer reads it that way today (`SurfaceWindow` waits on
  `presentRecord().frame`), so this is a contract gap rather than a live break.

## 24. R21 - the engine's threading half

*The question*: can a frame, a prepare and a shape change be in the wrong order - and what happens to the pump when the
thread it published is gone?

### 24.1 In one pass

`FramePump` owns the two `EngineFrame` values, the frame number, the interval, the completion counter and the last answer.
The interval is one `atomic<bool> compiling_` taken under `phase_mutex_` by compare-and-set
(`FramePump.cpp:54-63`), plus a `thread_local` naming the pump THIS thread is inside - the idiom that answers "is the
holder me?" without an `atomic<std::thread::id>` that may spin. `takeInterval` (`:65-80`) separates the two refusals a
caller can act on (`Reentrant` vs `Concurrent`) and refuses the engine thread a claim that would make it wait for itself.
The `Build` guard is the one place the phase ends (`:32-43`), so an early return and a throwing `describe` end it too.
`publishAnswer` publishes in one order - packed present word, then the answer under `mutex_`, then `answering_ = false`,
then `completed_` (`:259-276`) - which is what lets a host poll without waiting and still never see this frame's picture
beside that frame's numbers. `FrameRunner` runs one job at a time, answers "not answered yet" rather than waiting for
itself on the frame thread (`FrameRunner.cpp:76-92`), and counts a job that threw instead of dying on it (`:140-168`).

### 24.2 Verified in this round

- The interval and the shape change share one lock (`RenderEngine::beginShapeChange`, `RenderEngine.cpp:740-766`), so
  "nothing is recording" and "the phase flips" cannot be decided on opposite sides of each other - the defect C20 records.
  The refusal path releases the gate before reporting, so a sink asking for a shape change becomes a refusal, not a
  self-deadlock.
- `setThreaded` (`FramePump.cpp:226-257`) is an interval verb: it takes the interval, keeps it for the switch, stores the
  new runner BEFORE joining the old one, and ends on an object a waiter may still hold - so a `wait` that took the old
  runner by shared ownership finishes rather than dereferencing a freed one.
- `stop()` from the frame thread only ASKS the loop to leave and returns before clearing `answering_` (`:295-318`), which
  is right: the frame it is inside has not been answered yet.

### 24.3 Z21 (medium-low) - a shutdown from the frame thread leaves a runner that accepts work and never runs it

The pieces above are each correct and the join between them is missing. `RenderEngine::shutdown()` has no interval and no
reentry guard: it calls `pump_->stop()` (`RenderEngine.cpp:245`) and then releases the engine's transient targets and
detaches its surface (`:246-255`), whoever the caller is. Every device verb in this tree refuses that call from a frame
body ("it does nothing instead of waiting for itself"); the engine's own shutdown does not, and a diagnostic sink is host
code that runs *on the frame thread* - which is where the vk backend reports a lost device.

On that thread:

1. `FramePump::stop()` takes the runner, `waitIdle()` answers at once (self), `runner->stop()` sets `stopping_` and
   returns without joining (`FrameRunner.cpp:94-116`, "a thread cannot join itself").
2. `onEngineThread()` is true, so the pump returns **before** `runner_.store(nullptr)` and before `answering_ = false`
   (`FramePump.cpp:303-308`) - the runner stays published.
3. The loop finishes the job it is inside, sees `stopping_`, and exits (`FrameRunner.cpp:140-168`). `live_` is still
   `true`, because `live_.store(false)` only runs after a join from another thread (`:114`).

The runner is now a zombie: `threaded()` answers true, and the next `frame()` from the host thread takes the threaded
branch. `beginFrame`'s `waitIdle()` returns (nothing is in flight), the job is handed over - `request_` set,
`wake_.notify_one()` - and **no thread is waiting for it**, so it never runs. `waitForFrame` times out after its 5 s, and
the frame after that blocks for ever in `beginFrame`'s `waitIdle()` (no timeout, `FrameRunner.hpp`'s own note says why),
because `has_request_` is never cleared. The engine is wedged, and the pump's destructor wedges too.

The fix is two statements where the pump currently returns: the runner must stop advertising a live thread the moment its
loop can no longer run a job (`live_.store(false)` before returning from `stop()`, which is also what the class note
already promises - "after it, the runner behaves as an inline runner"), and the pump must not keep pointing at it. The
engine's `shutdown()` should also take the interval like every other host verb, so a call from a frame body is the
`Reentrant` refusal the rest of the tree gives rather than a half-done teardown.

**Fixed 2026-11-27, at the rule.** The engine half of this is exactly that: `shutdown()` takes the lease `prepare()`
takes and holds it across the whole teardown, so a call from a frame body refuses (`engine.shutdown.refused_from_frame`)
and a call from another host thread mid-build refuses too (`refused_in_flight`) - nothing torn down, one sentence per
episode. The runner half needed more than the two statements: `live_` gated `wait`/`waitIdle` as well, so clearing it
early would report "nothing in flight" while the last job was still inside the device, and clearing `runner_` loses the
only handle that can join the thread. What the defect actually lacked was ONE fact: "will a job handed over now run on
the thread", which is now `FrameRunner::accepting()` (a `State`: Inline / Running / Stopping / Stopped), decided by
`stop()` under the same lock `submit` decides under and published by `FramePump::threaded()`. A job handed over just
before the stop is still run (the loop drains it); a job submitted after it runs on the caller. Rule:
`../design/graphics-threading.md` 2.20 / C25; argument:
`../decisions/ADR-2026-11-27-a-teardown-is-an-interval-entry.md`; acceptance in T45.

## 25. R22 - the appfw surface above the paint loop

*The question*: does the layer above the paint loop own what it damages - a request, a declaration, a view - or does it
leave an area naming something that is gone?

### 25.1 In one pass

`Viewer` owns a REQUEST (`ViewerOptions`) and turns it into a chain on the area's view; it deliberately does not own the
area's presentation, so `isActive()` reads the area instead of remembering an answer (`Viewer.hpp:32-58`). A rebuild
carries the runtime choices of the live chain across (an overlay the user hid stays hidden) and re-states nothing else
(`Viewer.cpp:27-72`). `SurfaceWindow` composes the engine, the view, the area handle and the viewer in that declaration
order - and its comment says the order is the lifetime ("it borrows all three", `SurfaceWindow.cpp:226-236`).
`RenderArea` holds the declared view as a **borrowed** pointer (`RenderArea.cpp:218`), which is why `SceneView`'s
destructor dismisses itself first (`SceneView.cpp:38-40`). `InputRouter` is three lines of mapping in, no state out
(`InputRouter.cpp:7-19`).

### 25.2 Verified in this round

- A rebuilt chain is followed, not replaced twice: the area resolves the view's pass afresh, so `setViewerOptions` does
  not have to re-declare (`Viewer.cpp:120-145`), and `setShadingMode`'s cheap path checks the REQUEST as well as the live
  chain, so "Lit again after a non-Lit mode on a deferred request" rebuilds instead of silently doing nothing
  (`Viewer.cpp:147-170`).
- `takeArea()` leaves an area alone when a host declared its own pass (`Viewer.cpp:201-220`), which is the rule that
  keeps a default chain from being drawn beside a host's picture.
- The overlays' visibility is a switch on the built chain, not a recipe field, and the one thing a rebuild must not lose
  (a hidden overlay) is read from the live chain before the new one is built.

### 25.3 Z22 (medium) - the one shape change that cannot be retried is asked from a destructor

`SceneView::~SceneView` does three things in order: drop its surface handlers, dismiss itself from the area, drop the
chain, and take back the rows the chain stated in its scope - `engine->passes().clearOwnedBy(this)` (`SceneView.cpp:27-50`,
the call at `:48`).

That last call is a SHAPE CHANGE, and it obeys the shape-change gate: `RenderEngine::removePassesOwnedBy` asks
`beginShapeChange("removePass")` (`RenderEngine.cpp:767-776`) and, while a frame is being built or answered, is
**refused** - with a diagnostic telling the caller to "wait for the frame with `RenderEngine::waitForFrame`, or change
passes between frames" (`:757-765`). A destructor can do neither. It cannot wait (the frame may be the one destroying it)
and it cannot retry (there is no later).

So a view destroyed while a frame is in flight leaves its rows in the engine's table: the slots own the `RenderPass`
objects (`SessionShape`'s `Slot::pass` is an `intrusive_ptr`), so they stay alive and stay in the frame with a dead
`owner` pointer and a chain that has already been dropped, and the per-pass backend release `removePass` performs never
runs. This is not hypothetical for the shipped host: in threaded mode (the `SurfaceWindow` default, `:338`)
`frame()` returns as soon as the job is submitted, and `~Impl` destroys the view immediately before the engine
(`SurfaceWindow.cpp:226-231`) - and it is the ENGINE's destructor that stops the frame thread
(`RenderEngine.cpp:60`/`245`). Closing a window during a frame is therefore enough.

The fix has to be structural rather than a retry: either the take-back is a thing the engine does for a dead owner at
the next frame start (a deferred removal, the shape `RetirementQueue` already uses for Vulkan objects), or the view's
destruction is ordered behind the frame the way its surface handlers already are - but "ask, be refused, and drop it" is
the one answer the tree does not already have an idiom for.

**Fixed 2026-11-27, at the rule.** The take-back is now the one shape change the gate never refuses: applied at once
between frames, and DEFERRED to the next compile head (and by `shutdown()`) inside one, so the ask cannot be dropped and
the backend release it performs is not skipped. The difference from this section's first suggestion is where it is
keyed: the request carries the REGISTRATION TICKET current at the instant it was asked (`Slot::registration`), not the
owner address - `operator new` hands the next view the memory the last one freed, so an address-keyed record that
outlives its address by one frame would delete the rows of the object that registered second, which is the defect fixed
by introducing another one. The "order the view's death behind the frame" option was rejected too: a destructor cannot
wait when the frame is the one destroying it, and handing a host object to the engine inverts ownership. Rule stated in
`../design/graphics-threading.md` 2.19 / C24, argument in
`../decisions/ADR-2026-11-27-a-take-back-is-accepted-never-refused.md`, acceptance in T44.

### 25.4 Two caveats

- **`~Viewer` leaves the declaration behind, on purpose.** The header states it ("a viewer owns a request, not a
  declaration") and `release()` is the verb that withdraws. The area keeps a borrowed `SceneView*`, so a host that
  destroys a Viewer and keeps the area is holding an area whose declaration is only as alive as whoever else owns the
  view. The shipped order makes it safe; the contract is not written as a requirement beside `~Viewer`.
- **`foreign_view_reported` never re-arms** (`Viewer.cpp:77-95`): "a view bound to another engine" is reported once per
  viewer for the viewer's whole life, so a host that fixes the binding and breaks it again hears nothing the second time.
  Deliberate ("one condition, not a per-call event"), and the one place in this page where an episode with no end is the
  documented choice.

## 26. R23 - the tests as oracles

*The question*: for the mechanisms §17.2, §18.2, §19.2 and §20.1 verified by READING, is there a test that fails when the
mechanism is broken?

### 26.1 What the corpus already proves

The answer is better than "no mutation proof exists", and the three cases are worth naming because they are the shape the
others should copy:

- **The publication rule has a hostile witness.** `VkPrepareUploadTest.APrepareWhoseSubmitFailsPublishesNothing…`
  (`tests/test_vk/PrepareUploadTest.cpp:333`) uses the injectable `refuseNextSubmit()` door to make a batch record three
  copies and run none, then asserts the next frame's upload numbers - so `batchAbandoned` publishing instead of parking
  would fail it. That is a mutation proof for §17.2, and it exists because the seam exists.
- **The attachment-ops rule is a pure function with its own suite.** `AttachmentOpsTest` calls `resolveAttachmentOps`
  with no device at all (`tests/test_vk/AttachmentOpsTest.cpp:2`, `:26`), which is §18.2's rule.
- **The reflection has a real-program case.** `VertexInputTest` compiles and asks the shader what it declares - §19.2.
- **The host's answer has a consumer-side witness.** `RenderControlTest.ALostPictureIsRepairedByReannouncingTheSize`
  (`tests/test_gui/RenderControlTest.cpp:589`) drives the repair by setting a stub backend's outcome to `Lost`.

### 26.2 The gap is one pattern, and it is precise

Every test above replaces the PRODUCER of the answer with a stub or a seam, and asserts the CONSUMER. That is the right
shape for a consumer test and the wrong one for the producer, and the findings of this batch live exactly in the space
between them:

- `Z19` - the vk backend's decision about when to answer `Lost` is asserted nowhere; the GUI test feeds the answer
  directly. A test that made a real window frame's fence fail would have caught it (the seam exists:
  `Device::refuseNextSubmit`). (**since 2026-11-27** that case exists -
  `WindowPresentTest.AFrameWhoseSubmitIsRefusedAnswersLostAndTheNextOnePresents` refuses the submit of a real window
  frame and requires `Lost`; it read `Skipped` - the defect - until then.)
- `Z21` - no test calls `engine->shutdown()` from a frame-thread sink, so the zombie runner is unobserved (**since
  2026-11-27** `RenderEngineTest.AShutdownFromTheFrameThreadIsRefusedAndTheEngineKeepsRunning` makes the backend report
  from its frame body - the frame-thread sink - and requires the next frame to be answered, and
  `FrameRunnerReentryTest.ARunnerAStopHasLeftRunsTheNextJobOnTheCaller` proves the runner half directly).
- `Z22` - no test destroys a view while a frame is answered (**since 2026-11-27** the RULE has a case -
  `PlanTest.ATakeBackAskedInsideAFrameLandsOnTheNextFrame` drives the take-back from inside the compile through the
  engine's public door; the appfw composition itself, `~SceneView` with a frame in flight, still has no case).
- `Z17`/`Z18` - `TheViewReceivesInputAndADragKeepsSteeringIt` covers a release that ARRIVES; the release that never
  arrives is the untested half (**declined 2026-11-27**: the half stays untested because neither fix is being taken -
  and the attach-side waits have no seam a test could make time out at all).

So the residue is not "the corpus is thin" - it is that the three mechanisms with a seam are proven and the ones without
one are read-only. That is a statement about which seams to add next, not about how many tests to write.

### 26.3 No finding

Nothing in this round is a defect of the tree; the gap it names is the reason `Z19`, `Z21` and `Z22` could exist
unobserved, and it is recorded here so the next round starts from it rather than re-deriving it.

## 27. Registry and proposed order

### 27.1 Every finding

| # | Round | Severity | One line | Evidence |
|---|---|---|---|---|
| `Z1` | R1 | high | `publish` / `unpublish` / `resolve` / `transientTarget` touch `FrameResources`, which says "no synchronisation is needed" because one thread calls it - and the public docs state no thread rule (**fixed 2026-11-27**: one lock over every table a host can reach, the frame head takes the bindings as a value, the public docs state the rule - `graphics-threading.md` C23) | `RenderEngine.cpp:851-913`; `FrameResources.hpp:47-48` |
| `Z2` | R1 | medium-high | `bindPassContent` rewrites "the same one storage a plan writes and the compile reads" without the gate `applyPlan` takes (**fixed 2026-11-27**: the same `beginShapeChange` gate, answering `ContentBindOutcome::Status::Refused` - `graphics-threading.md` C20) | `RenderEngine.cpp:821-824`; `SessionShape.cpp:262-273`; `RenderEngine.cpp:740-796` |
| `Z3` | R1 | medium-low | `setFrameThreading` is the only interval verb that answers `void`; its three siblings answer the same refusal as a value (**fixed 2026-11-27**: `FrameThreadingOutcome` - `Applied` / `RefusedFromFrame` / `RefusedInFlight` - with the episode as the log half) | `RenderEngine.hpp`; `RenderEngine.cpp:392-419` |
| `Z4` | R1 | low | `frameContext()` says "the same answer all frame" and then reads three fields live (**fixed 2026-11-27**: the sentence states the current facts and moves the guarantee to "take the value once per frame") | `RenderEngine.hpp`; `RenderEngine.cpp:445-456` |
| `Z5` | R2 | low | the chunk-share test compares the fold's output; the select also reads `own.fold.placement`, which no comparison covers - the epoch is the only guard, and the epoch's list omits placements (**fixed 2026-11-27**: `RowOwn::Fold` compared whole, the placement keeps its identity until the node changes, the announcement list names placements) | `SceneSnapshot.cpp:55-68`; `DrawListBake.hpp:549-566`; `Node.hpp:296` |
| `Z6` | R2 | medium | `bindSnapshot` is permanent (no withdraw), "empty clears the binding" means "draw nothing", and the binding is keyed by a scene address it outlives (**fixed 2026-11-27**: `Scene` carries a `SceneId`, every row of the publication channel keys by it, `unbindSnapshot` is the withdrawal and the empty handle keeps meaning "an empty picture" - `graphics-threading.md` 2.13 / N19) | `PublicationStore.cpp:15-41`; `RenderEngine.hpp` `bindSnapshot` |
| `Z7` | R3 | high (latent) | the describe-once cache is unguarded in `ShaderProgram` and `MaterialValues`, guarded in `Geometry` - and the design cites `Geometry`'s lock as why shared content is thread-safe (**fixed 2026-11-27**: one `detail::DescriptionCache` behind all three, `graphics-threading.md` C22) | `ShaderProgram.cpp:45-54`; `Material.cpp:39-52`; `Geometry.cpp:425-450`; `graphics-sessions.md:18` |
| `Z8` | R4 | medium | the first-person keyboard look still calls `orbit()` (the eye arcs); the mouse look does not; `F5` says the arrows travel (**fixed 2026-11-27**: the gaze has one implementation, `OrbitCameraManipulator::look()`, and each device only converts its own units into it - `graphics-navigation.md` 2.10 / R5 / P7) | `OrbitCameraManipulator.cpp:449-463`; `graphics-navigation.md:123` |
| `Z9` | R9 | medium | `test_graphics` compiles twelve private TUs unconditionally, so the engine's suite cannot run under ASan on Linux - the ownership graph has no address-safety witness (**fixed 2026-11-27**: the module's object set is now linked instead, `graphics-architecture.md` 2.19) | `tests/test_graphics/CMakeLists.txt:38-59`; `.ai/memory/graphics.md` |
| `Z10` | R10 | low | the navigation page's `F5` sentence is `Z8`'s defect on the corpus side (**fixed 2026-11-27**: `F2` states the look's devices and `F5` the travel keys, each key set once) | `graphics-navigation.md:123` |
| `Z11` | R11 | medium-high | "does this pass write its target" is answered three ways and the scheduler's answer is false for a camera-less clearing pass, so a consumer may be ordered before the clear (**fixed 2026-11-27**: `RenderPass::writesTarget()` is the one answer - a draw packet or an attachment policy - read by the scheduler, the produced table and the wiring's filler check, `graphics-rendering.md` 2.16 / C9) | `RenderPass.cpp:146-158`; `Device.cpp:1524-1530`, `:2063`; `EngineCompile.cpp:226-231`; `EngineCompile.cpp:925` |
| `Z12` | R13 | medium | an instance stream is keyed by a borrowed `Buffer` address with no keep-alive and no identity, and its version (`revision`, starting at 0) is not unique across objects: a recycled address inherits the previous object's bytes (**fixed 2026-11-27**: the entry holds the `Buffer` its key names, and is withdrawn at the frame end where that share is the last one - `vk-backend.md` 2.5 / N11) | `ContentCaches.hpp:155-163`; `ContentCaches.cpp:318-350`; `Buffer.hpp:385` |
| `Z13` | R13 | low | two of the three fills remember a refusal; an instance upload retries, allocates and copies again on every frame (**fixed 2026-11-27**: `InstanceStream::failed` + the revision it failed at, a new revision retries - §2.5) | `ContentCaches.cpp:371-430`, `:494-497` vs `:318-350` |
| `Z14` | R14 | low | `vk.window.presenter_missing` accuses the host of a foreign presenter when the window merely has no live chain - and the window's own reason is never reported there (**fixed 2026-11-27**: `WindowTarget::presentationState()` + `vk.window.not_ready`; `presenter_missing` keeps its one meaning) | `Device.cpp:1550-1560`; `WindowTarget.cpp:494-497` |

| `Z15` | R15 | medium | a texture upload that fails mid-record destroys the image the caller's recording command buffer already holds copies and a barrier for, and the frame then submits that command buffer (**fixed 2026-11-27**: an upload is PREPARED then RECORDED - one staging buffer holding every layer and mip, built before the first command, and a record half that answers nothing - `vk-backend.md` 2.23 / C16 / P21) | `ContentImages.cpp:731-733`, `:903`; `:372-373`, `:421`, `:878-891` |
| `Z16` | R17 | medium-low | a program with no vertex and no fragment module (empty, fragment-only or compute-only) is cached as successfully compiled, and the pipeline build hands `VK_NULL_HANDLE` to `vkCreateGraphicsPipelines` (**fixed 2026-11-27**, and the finding's second half did not hold: both `ensure*Pipeline` refused a moduless pair before the review - the live defects were the wrong code, the requirement stated twice and nowhere at the builder, and a compute stage dropped in silence - `vk-backend.md` 2.22 / C15 / P20) | `ContentCaches.cpp:383-386`, `:424-429`, `:852`, `:856`; `ShaderProgram.cpp:77-81` |
| `Z17` | R18 | medium-low | a mouse release the surface never sees (window deactivated, platform window recreated mid-drag) leaves the button held and the manipulator dragging, so every later hover steers the camera and repaints (**declined 2026-11-27** by the owner: not taken) | `SurfaceWindow.cpp:566-571`, `:586-596`, `:616`, `:998-1024`; `OrbitCameraManipulator.cpp:424-430`, `:469-473`; `InputRouter.cpp:13-15` |
| `Z18` | R18 | low | three waits on the application thread, and only one is bounded: `SceneView` takes 250 ms and states why, the attach and first-picture waits take the default 5000 ms and discard the answer (**declined 2026-11-27** by the owner: not taken; the design page now names the attach-side default instead of generalising the bound) | `SceneView.cpp:163`; `SurfaceWindow.cpp:744`, `:915`; `RenderEngine.hpp:543`, `:550` |
| `Z19` | R20 | medium-low | a window frame whose submit or fence failed answers `Skipped`, never `Lost`: `rewindUnrunFrame` clears `window_acquired` before the present block reads it, so the `Lost` branch is dead - and the swapchain repair and the lost-picture line both key on `Lost` (**fixed 2026-11-27**: the hold and the promise are two fields, the give-back clears only the hold, and every non-presenting exit reads `unrunOutcome()` - which also gave the end-of-buffer failure the answer it never had - `vk-backend.md` 2.24 / C17 / P22) | `Device.cpp:1889-1893`, `:2291`, `:2302-2305`; `RenderEngine.cpp:620-622`; `SurfaceWindow.cpp:941-957` |
| `Z20` | R20 | low | a frame that did not complete answers with the PREVIOUS sampled frame's GPU time and per-pass costs: every `books.timing` reset sits inside `if (completed)`, and `books` is cleared only by `FrameDriver::destroy()` (**fixed 2026-11-27**: the books open disengaged at the frame's start, so only a read of that frame's own pair engages them - `N7` by construction - `vk-backend.md` 2.24 / C17) | `Device.cpp:2222-2271`, `:1431-1457`, `:2565-2580`, `:1398`; `FrameReport.hpp:57-58` |
| `Z21` | R21 | medium-low | `RenderEngine::shutdown()` takes no interval, so a call from the frame thread (a diagnostic sink) half-stops the pump: `FrameRunner::stop()` cannot join itself and leaves `live_` true, the pump returns before clearing `runner_`, and the next frame is handed to a runner whose loop has exited - it never runs, and the frame after blocks for ever in `waitIdle()` (**fixed 2026-11-27**: a teardown is an interval entry that refuses from a frame body, and the runner has one "will this job run" fact the pump publishes; C25 / 2.20) | `RenderEngine.cpp:235-255`; `FramePump.cpp:295-318`; `FrameRunner.cpp:94-116`, `:140-168` |
| `Z22` | R22 | medium | `SceneView::~SceneView` asks for a SHAPE CHANGE (`clearOwnedBy`) that `beginShapeChange` refuses while a frame is answered - and a destructor can neither wait nor retry, so the view's rows stay in the engine's table with a dead owner and a dropped chain, and `removePass`'s backend release never runs (**fixed 2026-11-27**: a take-back is never refused - deferred to the next compile head, keyed by the registration ticket; C24 / 2.19) | `SceneView.cpp:27-50`; `RenderEngine.cpp:767-776`, `:757-765`; `SurfaceWindow.cpp:226-231`, `:338`; `RenderEngine.cpp:60` |

### 27.2 Proposed order (for the owner; no backlog entry added by this round)

1. ~~**`Z7`** - one pattern applied twice, and it closes the gap between `graphics-threading.md` 2.16's claim and the tree.
   The only finding where the design's own promise is what fails.~~ **Done 2026-11-27**, at the cause: the rule got one owner
   type (`detail::DescriptionCache`) instead of two more hand-written copies, and 2.16 now names the rule (C22).
2. ~~**`Z1` + `Z2`** - one batch named "the doors that missed the rule": a mutex for `FrameResources` (the shape
   `PublicationStore` already uses) and the gate for `bindPassContent`.~~ **Done 2026-11-27**, at the rule instead of
   the two call sites: the image tables take one lock in every verb a host can reach AND the frame head takes the host's
   bindings as a value (C23), the content bind takes the shape's gate and answers the three-state
   `ContentBindOutcome`; the rejected shapes are in `ADR-2026-11-27-a-door-is-gated-or-locked.md`.
3. ~~**`Z22`** - the third door, and the only one whose failure is a stale row in the frame: the take-back a destructor
   asks for has to survive the refusal, so it becomes a deferred removal (the `RetirementQueue` shape) or the view's
   death is ordered behind the frame. It is ahead of the picture findings because it leaves a pass nobody owns in a
   frame that still runs.~~ **Done 2026-11-27**, at the rule: a take-back is ACCEPTED, never refused - applied at once
   between frames, deferred to the next compile head (and by `shutdown()`) inside one, keyed by the registration ticket
   so a recycled owner address cannot take a new object's rows (C24 / 2.19; `ADR-2026-11-27-a-take-back-is-accepted-
   never-refused.md`). The review's first suggestion (defer by owner address) was rejected as the next defect.
4. ~~**`Z21`** - the engine's shutdown takes the interval every other host verb takes, and the runner stops advertising a
   thread the moment its loop can no longer run a job. It is the only finding whose failure is a permanently wedged
   engine.~~ **Done 2026-11-27**, at the rule: a teardown is an INTERVAL ENTRY (`beginBuild`'s lease held across the whole
   body, refused from a frame body / another host thread with `engine.shutdown.refused_from_*`, nothing torn down), and
   the runner has ONE life fact (`accepting()`) that `FramePump::threaded()` publishes - so a stop asked from the frame
   thread makes the runner inline BEFORE it returns (C25 / 2.20; `ADR-2026-11-27-a-teardown-is-an-interval-entry.md`).
   The review's two statements were rejected: clearing the "live" flag early also makes `wait`/`waitIdle` answer idle
   while the last job runs, and clearing `runner_` loses the only handle that can join the thread.
5. ~~**`Z11`** - the same family as 2 and the only finding whose wrong answer is a picture: settle the predicate once
   (what `describe` announces) and let the scheduler, `recordProduced` and the SDK sentence read it from there.~~
   **Done 2026-11-27**, at the rule: the pass owns the answer (`RenderPass::writesTarget()` - a draw packet OR an
   attachment policy, so a camera-less pass' clear is a write), the same rule the SDK already asked of the value
   (`passHasEffect`), and the scheduler's edge / the produced table / the wiring's filler check all call it (2.16 /
   C9 / N15 / T10; `ADR-2026-11-27-a-pass-says-whether-it-writes-its-target.md`). The review's "make a camera-less
   pass announce nothing" alternative was rejected: it deletes the clear-only pass the backend already records.
6. ~~**`Z12`** - the instance stream's key: hold the `Buffer` it was built from (the geometry path's own shape) so the
   address cannot be recycled underneath it, and withdraw the entry when the buffer's last reference goes.~~ **Done
   2026-11-27**, at the rule: the entry holds its key (`InstanceStream::source`, and `ensureInstanceStream` takes the
   segment so the share is given rather than invented), and because holding makes the cache an owner the entry ends two
   ways - the host's announcement and the frame end where the cache's share is the last one (`sweepUnheldStreams`).
   Hold-only (the review's own prescription) was rejected: it enlarges the leak it would leave standing, since the entry
   would pin the host's bytes as well and the pattern this table serves rebuilds its buffer per frame (2.5 / N11 /
   D9 / D10 / T11; `ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end.md`).
7. ~~**`Z6`** - the override's withdrawal (one verb, plus the binding key).~~ **Done 2026-11-27**, at the rule: the scene
   carries a content identity (`Scene::id()`), so no row of the publication channel is keyed by an address any more;
   `unbindSnapshot` is the withdrawal, and the empty handle keeps its one meaning. The review's alternative - let the
   empty handle mean "no override" - was rejected: it deletes the value "an empty picture", which a bake with nothing
   in it legitimately binds (2.13 / 2.15 / N19 / T46; `ADR-2026-11-27-a-binding-is-keyed-by-identity-and-can-be-withdrawn.md`).
8. ~~**`Z9`** - the `if(WIN32)` guard and the ASan command in the ledger; it is what lets 1-7 be proven.~~ **Done
   2026-11-27**, at the cause: the module's object set is linked instead of a guard being added, and ASan now runs on
   the whole suite (see 10.2). It moved FIRST in the actual order, because it is what lets 1-7 be proven.
9. ~~**`Z8` + `Z10`** - one look path and one sentence.~~ **Done 2026-11-27**, at the rule: the gaze had no verb of its
   own, so the keyboard borrowed the turntable's - `look()` is now the one implementation, public because a host has the
   same need, and the drag and an arrow key only convert their own units into it (2.10 / F2 / F5 / R5 / P7 / T11;
   `ADR-2026-11-27-a-look-is-one-verb-whatever-the-device.md`). The review's second branch (arrows travel) was rejected
   as `D7`.
10. ~~**`Z16`** - one refusal on the program entry, and the only finding whose failure hands the driver an invalid handle:
    `ensureProgram` already has the `failed` flag and the episode rule, so an entry with no vertex and no fragment module
    becomes that flag instead of a cached success.~~ **Done 2026-11-27, not as prescribed**: the invalid handle never
    reached the driver (both `ensure*Pipeline` guarded it before the review), and failing the PROGRAM entry would refuse a
    legal full-screen draw. The requirement moved to `buildPipeline` - the one place that states modules to the driver -
    with its own code (`vk.content.program_stage_missing`) and a compute stage refused instead of dropped (2.22 / C15 /
    P20 / T12; `ADR-2026-11-27-a-graphics-pipeline-states-two-stages.md`).
11. ~~**`Z13`** + **`Z14`** + **`Z3`** + **`Z4`** + **`Z5`** - the vocabulary round: the third fill remembers its refusal, the
    presenter's sentence names the window's cause, one refusal shape for the last verb, one true sentence about
    `frameContext`, and the placement in the share argument.~~ **Done 2026-11-27**, with one answer per item at the rule
    rather than at the call site: `InstanceStream::failed` records the revision (a new revision retries) and the empty-view
    refusal is remembered the same way (2.5 / `D11` / `T13`); the window answers `presentationState()` and the frame
    reports `vk.window.not_ready` for an unready presenter, keeping `presenter_missing` for "no presenter this device
    handed out" (F3 / F14 / `D12` / `T14`); `setFrameThreading` answers `FrameThreadingOutcome` with the episode as the
    log half (`D14` / `T40`; `ADR-2026-11-27-the-last-interval-verb-answers-a-value.md`); `frameContext`'s sentence now
    states the current facts and moves the guarantee to "take the value once per frame"; and the share test compares
    `RowOwn::Fold` whole while a placement keeps its identity until the node changes (F1 / `D14` / `T37`;
    `ADR-2026-11-27-a-share-compares-the-input-the-select-reads.md`). The review's bare pointer comparison for `Z5` was
    rejected: with a fresh object per read it would stop billboard chunks sharing for no change at all.
12. ~~**`Z15`** - the failure path stops destroying what the command buffer names: build every layer's staging buffer
    before recording, or park the entry against the batch the way every other release in the cache does.~~ **Done
    2026-11-27**, at the rule and not at either branch: an upload is PREPARED (fallible, records no command) then
    RECORDED (`void`, one copy command carrying every region), with ONE staging buffer per upload - so the failure that
    used to arrive mid-record no longer exists, and the stand-in creates its view before its copy. The review's parking
    branch was rejected (`D13`); the upload's barriers now carry the tally, which is what makes the property a number
    (`2.23` / C16 / P21 / T15; `ADR-2026-11-27-an-upload-is-prepared-then-recorded.md`).
13. ~~**`Z19` + `Z20`** - the failed frame's answer: capture that a picture was expected before the give-back erases it,
    and disengage the device time the frame did not produce. Both are the `!completed` path in one function.~~ **Done
    2026-11-27**, at the rule instead of the two branches: the frame's HOLD on the image and its PROMISE to the surface
    are two fields (`window_acquired` / `picture_owed`), the give-back is one method that clears only the first, and
    every exit that does not present reads one answer function (`unrunOutcome`) - which also made the end-of-buffer
    failure answer `Lost`, a third unreachable branch the review did not name. The device time needed no failure path
    at all: the books OPEN disengaged, so `N7` holds by construction. Both assertions are mutation-proven (`2.24` /
    C17 / P22 / N7 / D14 / T16).
14. ~~**`Z17` + `Z18`** - the host half: one synthesised release on the transitions the surface already handles, and one
    bounded wait wherever the application thread waits.~~ **Declined 2026-11-27** by the owner: not taken. Both findings
    stand as described and neither is withdrawn - what changes is that the round does not act on them, and that the
    design page's "who waits, and where" no longer claims the 250 ms bound for the two attach-side waits (same file,
    §3.6), which is the one corpus defect the finding exposed on its own.

Backlog candidates (**not added**): `Z1`, `Z2`, `Z6`, `Z9`, `Z11`, `Z12`, `Z15`, `Z16`, `Z17` are present defects (no
trigger needed); `Z7` is latent with a named trigger (a host that shares a program or a material between two engines -
which the design already promises). `Z12` names its own trigger for a host that drops a `Buffer` without
`releaseContent`, but the entry it leaves behind is also an unbounded allocation, so it is a defect rather than a
trigger. `Z18` is one rule that one site takes and two do not. Both `Z17` and `Z18` are **declined** (§27.2 item 14), so
neither is a backlog candidate: a trigger is what turns a latent item into work, and these two are present defects the
owner has chosen not to take. The image cache's sampler table has its own trigger (a host that animates a sampler's lod
bias); it is recorded in §17.4, not here.

The five new ones, the same way: `Z19`, `Z21` and `Z22` are present defects (no trigger needed) - `Z19` needs only a
device that dies under a fence, `Z21` only a host that shuts its engine down from a diagnostic sink, `Z22` only a window
closed while a frame is answered; `Z20` is one line on the failure path. The descriptor-set ceiling of §22.3 is the one
new item with a TRIGGER rather than a defect: it needs a host with more than 1024 distinct (views, samplers, shadow)
combinations inside a 600-frame window, and until one exists the cap is a stated policy working as written.

### 27.3 What these rounds did not cover

§13-§16 read the four objects the plan deferred; §17-§20 read the four the machinery rounds named next; §22-§26 read the
five areas those named. What no round in this page has read line by line:

- `WindowTarget`'s SWAPCHAIN REBUILD: §18 read `beginPass`, `present`, `abandonFrame` and the acquire path, but the
  `vkCreateSwapchainKHR` + old-chain retirement sequence - the one `Z19`'s repair depends on - has been read only where
  a caller needed its answer.
- `DeviceChoice`'s candidate scan and the extension/feature filter beyond the values `initialize` consumes: whether the
  filter and the enable list are one statement for every field, not only for the four the creation path reads.
- `ContentDraw`'s `recordCallState` shadow-diff and `bindPipeline` beyond the bind loop §17 read: the state the recorder
  believes a NEW command buffer already holds is the one place a wrong diff is silent.
- The tests of the tier itself: `MemoryTest` (12 cases) and `RingTest` (6) were not read as oracles, so §22's placement
  and ring rules are read-verified, not mutation-verified - the same gap §26.2 names.
- The appfw surface BELOW the paint loop: `RenderControl`'s forwarding and the window container's visibility rule were
  read only where `Z17`/`Z18`/`Z19` needed them.

*Query watch for the whole of §13-§27*: §22's memory tier and §24.1's interval are the two mechanisms whose comments and
code agreed at every point checked. Every finding in the page still sits at a *boundary* the object does not own - a
predicate the engine answers for the scheduler (`Z11`), a key the cache borrows from the host (`Z12`, `Z6`), a sentence
the device writes about a window's state (`Z14`), a stage the compiler skipped and the cache remembers as built (`Z16`),
a button the surface tracks and the window never tells it to drop (`Z17`), a give-back that erases the fact it was
cleaning up after (`Z19`), a runner the pump keeps publishing after its loop is gone (`Z21`), and a take-back a
destructor asks for and cannot retry (`Z22`). The next round should read seams of that shape: the swapchains a repair
rebuilds, the candidate filter that must equal the enable list, and the state diff that must equal the command buffer.

