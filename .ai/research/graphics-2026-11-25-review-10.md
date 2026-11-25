# graphics review 10: the design corpus, the concept inventory and the extension surface

> **Status**: draft, non-authoritative - a working record. §2 is the module as the tree states it, §3 is the
> findings, §4 is what the peer engines do that does NOT transfer, §5 proposes an order. This round changed no
> file outside this page; every finding is a decision left to the owner.
> **Last verified**: 2026-11-25, tree `1d817fd4` on `dev` (clean working tree).
> **Scope**: the graphics module read across the three dimensions the nine earlier rounds never walked as a
> set - the **design corpus** (do the 11 normative pages still describe the tree), the **concept inventory**
> (101 public headers, 139 exported types, one concept one owner), and the **framework** (what an extension
> costs today, and which sets are closed on purpose). It does not re-walk implementation quality (7), the
> incremental edit path or the threading model (6), the seam's concepts and capabilities with `vsg` leaving (8),
> or the vk backend as a system (9).
> **Sources**: `src/viz/graphics/sdk/vine/graphics/*.hpp` (101 headers), `src/viz/graphics/src/**`,
> `src/plugins/gfx_backend_vk/**`, `tests/test_graphics/**`, `tests/test_vk/**`, `scripts/*.py` (the gates and
> the measuring script), `.ai/design/graphics-*.md` (11 pages, 5744 lines).
> **Backlog**: `../backlog/graphics.md` - this round adds no entry; §5 lists what it would add, for approval.
> **Numbering**: `L#` is this page's own set (reviews 8 and 9 used `C#` / `K#`). The module's `F#` / `C#` /
> `R#` / `N#` / `P#` / `D#` / `T#` remain the design pages'.

## 1. What this round is

Nine rounds have read the graphics module from nine angles. Between them they found defects in the code (6, 7,
9), in the seam (8), and in the speed of a frame (5, 9). What none of them did is read **the three
artefacts the module's own discipline rests on**: the design corpus that claims to describe it, the concept
inventory that claims one name per thing, and the extension table that claims to say what a new thing costs.
A module can be correct in every line and still be wrong in those three - that is what `G11` ("can a reader
reach the truth without guessing") and `G12` ("does the layout show the architecture") are about.

That is this round. It asks three questions of the module **as a whole** (not of a backend, not of a frame):

1. **Design.** Do the normative pages still describe the tree, and can a false sentence be caught before a
   reader acts on it? (§3 `L1`, `L2`)
2. **Concepts.** Does every exported fact have one owner and one consumer, and does every name say what the
   thing is? (§3 `L3`, `L6`, `L7`)
3. **Framework.** Which sets are open and which are closed on purpose, and does the answer to "how do I add
   one" hold when a second API family arrives? (§3 `L4`, `L5`)

### 1.1 The peers this round reads, and for what

This round's peer question is narrower than round 9's: **how does a peer answer "add a new X", and where does
it state that answer?**

| Peer | Mechanism read | Why it is the right comparator |
|---|---|---|
| wgpu / WebGPU | `GPUPrimitiveTopology` and the format set are CLOSED; the open axis is the pipeline descriptor and the bind-group layout | the purest "closed API set, open data" split, and it is specified in prose, not implied |
| Filament | five shading models are a closed engine set; a material is data compiled offline (`matc`) | the closest case of "view modes closed, materials open" |
| Diligent Engine | `PipelineStateDesc` as a struct; `IRenderDevice` / `IDeviceContext`; `PipelineStateCache` | an extension is a new struct FIELD, never a new enum value |
| Qt RHI | one `QRhi` interface; `QRhiGraphicsPipeline` is a description; the seam is the class | what "no second seam" costs when the class is the seam |
| bgfx / sokol_gfx | one header; handles; `bgfx_topology` closed, `bgfx::VertexLayout` open | the opposite granularity choice, useful only as a counter-example |
| Unreal RDG + RHI | passes are registered in C++; materials are data; PSO precache | where a peer declares "an engine edit is expected" |
| Godot `RenderingServer` | one server singleton; `RID` opaque handles | the ownership model this repo rejected (review 5 §2.4) |
| Granite | render graph with transient aliasing; reflection-derived layouts | the "framework owns the graph" answer |

None of these is a dependency and no code is copied (`D1`). The comparison is about which question each
mechanism answers, and whether the tree's answer is written down.

### 1.2 Rule of evidence

Every finding names a `file:line`, a test, or a number produced by a command that is quoted beside it. A peer
claim is a statement about that peer's documented design, not a benchmark. Every finding names the `G#` it
serves or trades against, and says whether the fix is a mechanism (needs an ADR), a local change (needs a
case), or a doc-only correction.

## 2. The module as it stands

### 2.1 The surface in numbers

Measured with the repo's own stick, so every number below can be re-derived:

| Quantity | Value | How |
|---|---|---|
| Public SDK headers | **101** | `python3 scripts/measure_graphics_claims.py` |
| Exported types (`VN_GRAPHICS_API class/struct`) | **139** | `grep -c '^\(class\|struct\) VN_GRAPHICS_API'` over the 101 headers |
| `RenderEngine.hpp` | **1198** lines, **59** public signatures, included by **25** files | the measuring script |
| Mechanisms behind `RenderEngine` | **8** `detail::` types | `RenderEngine.hpp:41-57` |
| `RenderDevice` | **19** virtuals, **5** pure | `RenderDevice.hpp` |
| `RenderFrameExecutor` / `RenderBackendFactory` | 4 virtuals (1 pure) / 3 virtuals (2 pure) | the same headers |
| The seam | **8** mechanisms inside a shared surface of **37** headers | `scripts/check_include_hygiene.py:173,196` |
| Diagnostic codes | **107** | `scripts/diagnostic_codes.txt` |
| Tests | `test_graphics` **647/82**, `test_vk` **220/46** | `./build/bin/test_* --gtest_list_tests`, via the measuring script |
| Design corpus | **11** pages, **5744** lines | `wc -l .ai/design/graphics-*.md .ai/design/vk-backend.md` |

A second family of numbers the module keeps: **154 public functions answer a bare `bool`** (the measuring
script's `SDK bool returns`) beside six named answer types (`FrameCallOutcome`, `PlanResult`, `PrepareReport`,
`ScenePublishResult`, `PresentOutcome`, `ReadbackResult`). That split is not drift - `graphics-architecture.md`
2.7 states the rule ("a change that can fail is visible from the return value", and `removePass` stays
idempotent because removing twice is one answer), and `L3` is the one place the rule is broken by a *fact*
rather than a verb.

### 2.2 The layering, measured

The `G2` answer was audited in five directions with greps rather than by reading:

| Edge | Result |
|---|---|
| public SDK header → anything outside std / `base` / the SDK | **0 leaks** |
| an API-family word (`Vulkan`, `OpenGL`, `D3D12`, `Metal`, `vsg`) inside the SDK or its implementation | **50 occurrences, 0 dependencies** (35 name a peer in prose, 15 are the `RenderApi` vocabulary, 0 include or call anything) |
| backend plugin → a PRIVATE SDK header (`src/viz/graphics/src/**`) | **0** - structural: only `sdk/` is on the plugin's include root (`cmake/VineLibraryHelper.cmake:77,83`) |
| appfw → graphics | **17 includes in 7 files, one-way**; the reverse edge is **0** |
| graphics → another module | **44 includes, all into `src/base/**`** (`geom`, `models`, `imaging`, `window`, `core`) |

The seam's eight mechanisms are all reached by the shipped backend (`FrameGate`, `DeviceClock`,
`FrameTimeline`, `RetirementQueue`, `Readback`, `DrawState`, `TargetPlan`, `DiagnosticDraft`), so the
promise-based membership rule of `ADR-2026-11-23` is not carrying a passenger today. This section has no
finding: it is the part of the module that is simply right, and it is right because a gate says so rather than
because a reader checked.

### 2.3 Where the design is ahead of its peers

Seven things a peer does not have, kept here so the findings below are read against the right baseline:

1. **The seam is a measured PROPERTY, not a directory.** `SEAM_HEADERS` (8 mechanisms) plus
   `SEAM_SHARED_HEADERS` (37 headers) are asserted by `check_include_hygiene.py`, and the two pages that
   describe the seam must name every mechanism (`SEAM_DOCS`). No peer in §1.1 asserts what its seam contains;
   Qt RHI's seam is simply "the class".
2. **A module states the revision it was built against**, and the registry compares that number *before* it
   calls a single virtual (`ADR-2026-11-18`, `BackendAbi.hpp`). The measured cost of its absence (a stale
   plugin that segfaulted a registration case, 2026-11-23) is exactly what the peers still pay.
3. **One answer value per verb** (`BC-41` / `BC-42`): a refusal is classified *before* any wait, and the
   classification and the product travel in one value. WebGPU needs `mapAsync` + `mapState` + a status;
   Vulkan needs `VkResult` plus a validity rule the caller keeps.
4. **The plan is data and the backend consumes compiled values** (`graphics-architecture.md` 2.2). Unreal's
   RDG and Granite keep the graph inside the backend; here the graph is the host's declaration and the compile
   is shared, so two producers of a frame can be compared value-for-value.
5. **A state priority table stated once** (`StateResolution.hpp`, `graphics-scene.md` §4.1) with the
   pass/data/content order written as prose and as code. Filament states a subset (pass overrides) and leaves
   the rest to the material.
6. **The cost of an extension is written as a table** (`graphics-architecture.md` 2.11). The *idea* is unique
   in the peer set; `L1` is that the table is not maintained, which is the harder half.
7. **Migration by deletion with a `BC#` ledger** (`BC-26`, `BC-27`, `BC-41`, `BC-42`): no deprecated alias
   survives a batch. The peers keep aliases for years; this repo pays one migration per batch instead.

## 3. Findings

Seven findings: two about the design corpus (`L1`, `L2`), three about concepts (`L3`, `L6`, `L7`), two about
the framework (`L4`, `L5`). `L3` is the only one with a runtime cost; `L1` is the only one where the tree
already tells a developer to follow a sentence that no longer matches it.

### 3.1 `L1` The extension table names propagation sites, so it rots - and it is the table the headers send a developer to

- **What the tree does.** `graphics-architecture.md` 2.11 states one row per extension point, "listing today's
  actual files - read this table before adding something, update it afterwards". The row for a pass kind
  (`:119`) says a new kind touches `DrawKind.hpp`; `Plan.cpp`'s validation; **`RenderPass.cpp`'s kind → node
  mapping**; a new node class; `EngineCompile.cpp`'s describe; "each backend's frame compiler and recorder";
  and prices it at **6-8 places**. `DrawKind.hpp:31-37` points a reader at the row by name: "a third kind means
  walking `graphics-architecture.md` 2.11's extension row".
- **Why it is already wrong.**
  - The kind → node mapping moved to `SessionShape.cpp:338-349` (`makePassNode`) when 2.8 split the session
    shape out of `RenderEngine`. A reader following the row edits a file that no longer holds it.
  - The count is low by roughly half. Adding a `DrawKind` value touches, at minimum: `DrawKind.hpp`,
    `PassDesc.hpp`, `SessionShape.cpp:212,344`, `Plan.cpp:80,135,215`, `FrameDescription.hpp:342,409`,
    `FrameExpand.cpp:14,23`, `RenderPass.cpp:80,85,156`, `FullscreenPass.cpp:15,61`, and in the backend
    `ContentCaches.cpp:938,1044`, `ContentDraw.cpp:392`, `Device.cpp:1481,1656,1693,2062` - about 9 SDK files
    plus 4 backend sites (a new `PassStage` additionally reaches `ChainBuilder.cpp:268,309,399,425,510`).
  - The row still says "each backend's frame compiler" though one backend remains (`T35`).
- **Four axes have no row at all**, and they are the four an integrator hits first:
  - a new `PassStage` (`PassStage.hpp:20` enum + `passStageOrder`'s `constexpr` switch `:44`);
  - a new `Topology` value (`Topology.hpp:37`, `TriangleStream.hpp:32,149`, and the backend's
    `StateMapping.hpp:161`);
  - a new built-in `ShadingMode` (`ShadingMode.hpp:36` + the switch in `PassChain.cpp:126-143` + the
    `flatForwardProgram` / `unlitForwardProgram` / `normalsForwardProgram` definitions in `BuiltinShaders.cpp`);
  - **a new content node type** - the largest host extension there is (`Node` / `Group` / `StateNode` /
    `MaterialValues::resolved`, plus the `obj_cast` dispatch `T20` landed).
- **Why it matters.** `G11` / `G12`: a normative table whose purpose is to be read **at the moment of an edit**
  is only useful while it is true, and nothing verifies it (`check_doc_symbols.py` checks names, not roles;
  every path in the row still exists, so even a path check would pass). This is the `K9` defect class - a
  sentence that contradicts its mechanism - applied to the one page that says "update me".
- **What to do.** Change what a row *is*: state the **decision site** (the enum/struct where the new value is
  written) and the **propagation rule as a grep** - `grep -rn 'DrawKind::' src` finds every branch and cannot
  go stale - and demote the file list to "where it landed on 2026-11-25". Add the four missing rows. The
  criterion in 2.11 ("if it needs core-model changes it is a refactor, needing an ADR") then needs one
  sentence: a closed enum is not the core model (see `L4`).
- **`G#`**: `G11`, `G12`. **Cost**: doc-only.

### 3.2 `L2` Numbers in normative prose have no witness, and two are already wrong

- **What the tree says.**
  - `graphics-architecture.md:106-107`: "the shared surface is 35 headers, not the 8 mechanisms". The gate's
    list holds **37** (`scripts/check_include_hygiene.py:196-236`).
  - `backlog/graphics.md:17` (`T35`): "`test_vk` has **186 cases / 45 suites** (2026-11-23)" and "`test_vk` is
    **186/186** with no skips". Today it is **220 cases / 46 suites**
    (`python3 scripts/measure_graphics_claims.py`).
- **Why it matters.** `G4` / `G11`. The corpus pins symbol names (`check_doc_symbols.py`), design-document
  structure (`check_design_docs.py`) and the seam's mechanism names (`SEAM_DOCS`) - but a count is prose, and
  prose is what a reader trusts. The repo already owns the measuring stick
  (`scripts/measure_graphics_claims.py`) and already prints the SDK header count and both test counts; the two
  numbers above simply do not name it. A gate-reviewer in round 9 found the same class in two *sentences*
  (`K9`); this is that class for numbers, and it has two live instances.
- **What to do.** One rule, stated where the corpus's conventions live: a count in normative prose either
  names the command that produces it (the way a caveat names its test) or is deleted. `check_design_docs.py`
  can then refuse a bare "\<number\> headers" / "\<number\> cases" sentence that carries no command - a
  regex, not a parser. Update the two instances in the same batch.
- **`G#`**: `G4`, `G11`. **Cost**: doc-only, plus one check.

### 3.3 `L3` `RenderStateOrigin` is computed per command and read by nothing outside the tests

- **What the tree does.** `resolveRenderState` resolves every command's final state **and** fills a
  seven-field provenance table: `StateResolution.hpp:71` (`RenderStateOrigin`) with one `StateOrigin` per item,
  written at `StateResolution.cpp:96-132` through the `take()` helper. Its own doc claims the table "turns
  'why is this drawn in Line?' from a code-reading exercise into a fact".
- **Who reads it.** Nobody outside the tests. Every `.origin` read in the tree is
  `tests/test_graphics/GraphicsTest.cpp:9601-11058`; the two backend call sites of `resolveRenderState`
  (`ContentCaches.cpp:1148,1240`) and every other consumer of `ResolvedCommandState` (`ContentDraw.cpp`,
  `FrameHead.hpp`) read `.state` / `.topology` only. No report part, no `FrameTable` row, no host accessor and
  no debug view exposes it, so the sentence above is false today.
- **Why it matters.** `G8` (a fact whose only consumer is a test) and `G10` (seven writes plus a reference
  thread on every command, every frame, in the path the design calls the bounded head). This is the exact
  class `BC-26` / `BC-27` / `BC-42` closed by deletion - `Sets`, `ResidencyTotals`, and the seam's readback
  classifier - and architecture 2.15 admits a public fact only when a host must branch on it, request it, or
  read it.
- **What to do.** Decide, in 2.15's terms, which answer this is:
  - (a) **give it its one consumer** - a debug report part (the shape `FrameReport` already has), which makes it
    a mechanism and needs a stated contract for who may read it and when;
  - (b) **delete `origin`** - the precedent, and the cheaper answer; it is a field of a value returned across
    the seam, so the batch bumps `VN_RENDER_BACKEND_ABI_VERSION` and states it in a `BC#`;
  - (c) keep it behind a debug build - the one option that keeps a test-only fact in a public header, which
    `G1` / `G8` argue against.
  Recommendation: **(b)** unless a debug view is planned this quarter; a "why is it drawn like that" answer is
  better served by the state table's prose (which already says the priority) than by a per-command table
  nobody reads.
- **`G#`**: `G8`, `G10`. **Cost**: a code batch with an ABI bump; one test either way.

### 3.4 `L4` Which sets are closed on purpose is nowhere stated, and 2.11's criterion reads as a ban on ever adding one

- **What the tree does.** Four public enums are closed *by design* and each says so in a different place:
  `Topology.hpp:37` ("this engine keeps ONE pipeline per enum value"), `DrawKind.hpp:31-37` ("adding a value is
  a design batch, not a word sweep"), `PassStage.hpp:20` with a `constexpr` order switch, `ShadingMode.hpp:36`
  behind a `switch` with no `default` (`PassChain.cpp:126-143`). The open axes are equally real: a pass'
  declaration is data (`PassDesc`), a program is host-supplied (`PassDesc::program`), a material may subclass
  `MaterialValues::resolved()`, a node type may subclass `Node`.
- **Why it matters.** `G9` / `G8`. "Can a new X be added without editing the core model" is the first question
  a host integrator - or the D3D12/Metal author the owner's premise names - asks, and the answer is a split:
  **closed** (pass kinds, stages, topologies, built-in view modes: an engine edit, in one decision site, with a
  propagation rule) versus **open** (content shape, programs, materials, node types: host data or a public
  seam). Every peer in §1.1 makes that split and states it - WebGPU's topology set *is* its answer, Filament's
  five shading models are its answer. Here a reader must infer it from four @notes, and 2.11's criterion ("if it
  starts needing changes to the core model it is not an extension but a refactor, and that step needs an ADR")
  reads as "you may never add a topology", which is not the rule the tree follows: adding one is an ordinary
  batch with an ABI bump.
- **What to do.** One design point beside 2.11: the two lists, each with its decision site, and the sentence
  that a closed set is *why* the model is small (`G8`), not an oversight. Then 2.11's criterion is restated to
  match (a closed member is not the core model; the frame value's shape, the compile and the scheduling are).
- **`G#`**: `G9`, `G8`, `G12`. **Cost**: doc-only; depends on `L1` (same table).

### 3.5 `L5` `RenderDevice`'s 14 defaulted verbs: the per-verb reasons exist, the rule does not - and one default is the leak

- **What the tree does.** `RenderDevice` carries 19 virtuals, 5 pure (architecture 2.15 states the pair). Most
  of the 14 defaults carry a stated reason - `createFrameDriver` ("a backend that hands out more than one
  answers a NEW executor per call"), `releaseFrameDriver` ("the default does nothing: a device whose
  createFrameDriver() answers its own executor has nothing to reclaim"), `releasePass` ("the default no-op lets
  a device that keeps no per-pass state ignore the call; that is the honest answer, not a missing
  implementation"), `readColorImage` / `readDepthImage` (`Unsupported`).
- **What is missing.** (a) A rule that says when a default is *legal*, so a new backend author can check their
  override list against something. (b) `releaseRenderTarget`'s default is a bare `(void)target;` with no
  sentence - and it is the one whose omission **leaks**: a backend that builds attachments and forgets the
  override keeps them until `shutdown()`, silently. Architecture 2.15 states the "a DEFAULTED one is a silent
  lie" hazard for *facts* ("absent is not zero"), but the verbs never got the same treatment.
- **Why it matters.** `G4` / `G6` / `G11`. The seam is the one place where an omission is invisible by
  construction (nothing calls the verb until a host releases something), and the framework's whole ABI story
  (`L2.3` above, `BackendAbi.hpp`) exists to make *recompiles* safe - it cannot make *forgotten overrides*
  safe. A sentence in the class note is the cheap half: "a default is legal only when its absence is a true
  statement about the device - no surfaces, no per-pass state, no readback - and every such default says so; a
  verb whose omission leaks or answers wrongly stays pure."
- **What to do.** Add the rule to the class note; give `releaseRenderTarget` either its sentence (if a device
  that builds no per-target state exists - it does: the SDK's own default) or make it pure. Either way the
  change rides the next seam batch, because making it pure bumps the ABI.
- **`G#`**: `G4`, `G6`. **Cost**: comment now; an ABI batch later.

### 3.6 `L6` `ImageSource.hpp` is named after the engine's half while the type a host writes is `ImageInput`

- **What the tree does.** `ImageSource.hpp` declares `ImageInput` (`:46`) - the declaration a host builds
  through `byImage` / `byTarget` / `byName` / `byTexture`, the one ordered list `T56` landed - and
  `ImageSource` (`:290`), the record the compile resolves. Every other host-facing header is named after what
  it declares (`PassDesc.hpp`, `Target.hpp`, `Readback.hpp`, `View.hpp`); this one is named after the value the
  host never constructs, so the design page that declares both has to spend a sentence on which half is which
  (`graphics-rendering.md:284`), and the guide's table spells `ImageInput::byTarget` while the file it belongs
  to is named after the other one (`docs/guides/graphics.md:628`).
- **Why it matters.** `G12` (names show the domain) and `G4` (a reader finds the thing by its name). After
  `T56` the input list is the one shape a host states, so the host-facing name is the one that earns the file
  name. It is a small find, and it is listed because it is cheap to fix in the next public-API batch and
  expensive to keep explaining.
- **What to do.** Rename the header after the declaration (`ImageInput.hpp`), with `BC#` migration notes. Do
  not add an alias header: two names for one concept is the family this repo deletes.
- **`G#`**: `G12`, `G4`. **Cost**: one file rename in a public-API batch.

### 3.7 `L7` The public surface has three granularities and no stated rule or index (low)

- **What the tree does.** 101 headers for 139 types, at three granularities: one concept (`Topology.hpp`,
  `Viewport.hpp`, `DepthPolicy.hpp`, `View.hpp`), a value cluster whose parts are only meaningful together
  (`DrawState.hpp` - `LoadOp`, `StoreOp`, `AttachmentOps`, `VertexLayoutKey`, `DeviceFormat`, `RenderPassKey`,
  `PipelineKey`, `DynamicState`), and a report cluster (`FrameReport.hpp` - seven parts plus the report). There
  is no umbrella header and no header index; a host looking for "the pipeline key" opens a file called
  `DrawState`.
- **Why it matters.** `G12`, and only that: the cost is discoverability, not correctness. The peers pick one
  granularity each (Diligent one interface per header; sokol and bgfx one header; QRhi one class), and this
  module picked three without saying which rule a new header follows.
- **What to do.** State the rule where the corpus's conventions live - "a header is named after the value a
  host most often writes or subclasses; a cluster is allowed when its parts are only meaningful together, and
  its name states the cluster" - and keep the guide's reader-facing table as the index. No file moves:
  reorganising 101 headers is churn (`G8`).
- **`G#`**: `G12`. **Cost**: doc-only.

## 4. What does not transfer (the `G8` half of the benchmark)

| Peer idea | Why it does not transfer today |
|---|---|
| bgfx / sokol: one header | 139 types with Qt-style accessors and doc blocks would be one 10k-line file; the header-per-concept rule is what keeps a gate's scope and a `git blame` meaningful. Diligent, the closest peer in ambition, also ships one interface per header |
| WebGPU: the API owns resource transitions | it works because the API sits below everything; here the seam carries no layout at all (`T15`), and the backend that owns the transitions is the correct owner of that knowledge |
| Unreal: PSO precache as a first-class cache | the compile cost is real but it is `T17`'s subject (a cold build under the device-wide lock); a precache system with no measured consumer is `G8` |
| Godot: one `RenderingServer` with `RID` handles | ownership in the signature is the framework's value proposition; review 5 §2.4 rejected opaque handles |
| Filament: offline material compiler | a product decision the owner deferred (review 8 `P4`); the backend's job is to make the compile's cost visible |
| Unreal RDG / Granite: a render graph with transient aliasing | review 5 §2.3: the chain is a declared, known set; the transients are named outputs |
| Diligent: `USAGE_DEFAULT` + `UpdateBuffer` for every dynamic buffer | `T67`'s subject, and it is trigger-gated on a device whose memory types differ |
| Qt RHI: one interface as the seam | the device / executor / factory split is what gives one device several drivers (`ADR-2026-11-13`) |
| bgfx: `BGFX_STATE_*` bitmask state | a bitmask hides the priority table that 2.15's admission test and `graphics-scene.md` §4.1 state explicitly |

## 5. Proposed order

Doc-only first (it is what unblocks the rest), then the one finding with a runtime cost, then the two that
ride an existing batch.

| Step | Item | Kind | Gate / witness |
|---|---|---|---|
| 1 | `L1` + `L2`: rewrite 2.11's rows as decision-site + grep, add the four missing rows, strip or label the two stale numbers | doc-only | the three doc gates; `measure_graphics_claims.py` for the two counts |
| 2 | `L4`: the closed/open lists as one design point, and 2.11's criterion restated | doc-only, after 1 | the same gates |
| 3 | `L3`: decide `origin` - delete, or give it a report part | code, ABI bump if deleted | a `GraphicsTest` case either way (`ResolveTest` today holds the only readers); `test_graphics` + `test_vk` |
| 4 | `L5`: the default-verb rule in the `RenderDevice` class note; `releaseRenderTarget`'s sentence or its pure form | comment now, ABI batch later | rides the next seam change |
| 5 | `L7`: the granularity rule, beside the guide's index | doc-only | doc gates |
| 6 | `L6`: `ImageSource.hpp` → `ImageInput.hpp` | public-API batch | `BC#` note; `check_public_docs.py` |

What this round does **not** add to the backlog: `L1`-`L2` are corrections of claims that are already made,
`L4`-`L5` and `L7` are statements of rules the code already follows, and `L6` is a rename. `L3` is the only
item that could become a backlog entry, and only if the owner chooses (a) - a debug report part - since that
is a new mechanism without a consumer today.

## 6. Verification recipes

| Check | What it proves |
|---|---|
| `python3 scripts/measure_graphics_claims.py` | every number in §2.1, and the two `L2` values, re-derived with their scope |
| `python3 scripts/check_ai_docs.py --strict` + `check_design_docs.py` + `check_cross_refs.py` | this page's references resolve; the index names it |
| `grep -rn 'DrawKind::' src` / `grep -rn 'Topology::' src` | the propagation rule `L1` proposes: the branches a new value must reach, found rather than listed |
| `grep -rn '\.origin' src tests` | `L3`: the only readers of the provenance table are tests |
| `./build/bin/test_graphics --gtest_filter='*Resolve*'` | `L3`'s witness, whichever way the decision goes |
| `python3 scripts/check_include_hygiene.py` | the seam lists and §2.2's five edges |

## 7. References

### 7.1 Code

- `graphics-architecture.md` design points: 2.7 (return values), 2.8 (composition root), 2.9 (seam mechanisms),
  2.11 (extension table), 2.15 (public-surface admission), 2.16 (module ABI).
- `src/viz/graphics/sdk/vine/graphics/`: `DrawKind.hpp`, `PassStage.hpp`, `Topology.hpp`, `ShadingMode.hpp`,
  `StateResolution.hpp`, `RenderDevice.hpp`, `RenderFrameExecutor.hpp`, `ImageSource.hpp`, `FrameReport.hpp`,
  `FrameDescription.hpp`, `Plan.hpp`, `RenderPass.hpp`.
- `src/viz/graphics/src/`: `SessionShape.cpp` (`makePassNode`), `StateResolution.cpp`, `PassChain.cpp`,
  `TriangleStream.hpp`, `ChainBuilder.cpp`, `BuiltinShaders.cpp`.
- `src/plugins/gfx_backend_vk/`: `StateMapping.hpp`, `ContentCaches.cpp`, `ContentDraw.cpp`, `Device.cpp`.
- Gates and measuring tools: `scripts/check_include_hygiene.py` (`SEAM_HEADERS`, `SEAM_SHARED_HEADERS`,
  `SEAM_DOCS`), `scripts/check_diagnostic_formats.py`, `scripts/diagnostic_codes.txt`,
  `scripts/measure_graphics_claims.py`.

### 7.2 Documents and ADRs

- `ADR-2026-11-16-no-backend-half`, `ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace`,
  `ADR-2026-11-23-retiring-the-reference-backend`, `ADR-2026-11-18-names-state-what-is-handed-over`,
  `ADR-2026-11-25-one-input-list-per-pass`, `ADR-2026-11-25-readback-answer-is-one-value` (and its amendment).
- Earlier rounds: `graphics-2026-11-18-review-5.md` (first-contact examples, RID rejection),
  `graphics-2026-11-19-review-6.md` (incremental path, threading), `graphics-2026-11-20-review-7.md`
  (implementation quality), `graphics-2026-11-23-review-8.md` (concepts and capabilities, the second-family
  premise), `graphics-2026-11-25-review-9.md` (the vk backend as a system).
- `docs/guides/graphics.md` §5.3 (five frame values, one question each) and §5.4 (the target vocabulary: six
  host-side roles plus three inside the backend), with §5.1's translation table as the reader-facing index.

### 7.3 External material

- WebGPU specification: `GPUPrimitiveTopology`, `GPURenderPipelineDescriptor`, and the closed format table.
- Qt RHI (`qrhi.h`): one interface, description structs, explicit resource creation.
- Diligent Engine: `PipelineStateDesc`, `IRenderDevice` / `IDeviceContext`, `PipelineStateCache`.
- Filament: the shading-model set and the offline material compiler (`matc`).
- bgfx (`bgfx.h`), sokol_gfx (`sokol_gfx.h`): one-header APIs, handle-based resources.
- Unreal: RDG and `FShaderPipelineCache`; Granite: render graph with transient aliasing.
