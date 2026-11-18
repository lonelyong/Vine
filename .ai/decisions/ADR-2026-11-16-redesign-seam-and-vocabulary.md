# ADR-2026-11-16: what was redesigned is the names and the ownership, not the capabilities - the seam, vocabulary and phases of the two-backend graphics stack

- Status: **Accepted**. Approved on 2026-11-16; stage three ran the 18 batch positions of §10 in `../research/graphics-2026-11-16-redesign-r2.md`
  and **all landed on 2026-11-17** (progress is in "Implementation status" at the end of this document).
  **Partly superseded**: §2 here (the seam moving home: `backend/` keeping only the shared mechanisms) and the "directory as seam" half of batch 3
  are superseded by [ADR-2026-11-16-no-backend-half](ADR-2026-11-16-no-backend-half.md) (2026-11-16); this document's vocabulary, phases,
  refusal shape and remaining batches are unchanged.
- Date: 2026-11-16
- Scope: `graphics/sdk/vine/graphics/backend/**` (25 → 7 headers: 6 mechanisms + 1 guard), `graphics-architecture.md` (§2.9 / §2.11),
  `graphics-sessions.md` (§2.1 / §2.3 vocabulary and phases), `graphics-threading.md` (§2.16 driver identity),
  `vk-backend.md` (§2.16 / §2.17 / §9 status), `vsg-backend.md` (§2.9.1 the seam declaration),
  `docs/guides/` (pages split by backend → pages split by capability), the contract text of `RenderFrameExecutor.hpp` / `RenderDevice.hpp`,
  `FrameStats.hpp` (`FrameReport::locking`, additive).
- Related: `ADR-2026-11-16-authoring-view-and-target-vocabulary.md` (round two's **concept and API shape** sequel: `View` /
  `RenderArea` / `Target`+`Output` / the pass authoring shape / the normative revisions; the two proposals are approved or deferred together),
  `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` (this document closes out its **vocabulary and seam**, without reopening its decisions),
  `ADR-2026-11-13-one-engine-one-surface.md`, `ADR-2026-11-12-adopt-graphics-next.md`,
  `ADR-2026-11-10-gpu-residency-pinned-by-content.md`, `ADR-2026-11-14-vk-becomes-the-default-target.md`.
- Review and construction plan: `../research/graphics-2026-11-16-redesign.md` (A the capability list, B item by item, C cross-cutting, D the conclusion table, the batch plan).

## Background

The owner's request: **redesign graphics and the two backends (concepts / abstractions / framework / C++ structure), losing no functionality.**
Types, interfaces, layers, naming and public API/ABI may all be redesigned; not one capability may be dropped; deliberate behaviour changes are listed one by one and approved.

Stage one did a read-only audit of about 44k lines of `.cpp` (`graphics/src` 14.8k + vk 12.0k + vsg 17.2k), 21k lines of SDK headers,
1286 test cases, 12 design pages and 100 diagnostic codes, and the conclusion came down to one sentence:

> **The capabilities are complete and the evidence is thick; nearly all the problems are in the names and the ownership.**

The three most telling facts (each verifiable by file:line or by include counts):

1. **The seam did not live up to its name**: of the 25 headers in `sdk/vine/graphics/backend/` (including 1 include guard), `gfx_backend_vk` included only 6;
   18 had `gfx_backend_vsg` as their only consumer, while their implementations (18 `.cpp` files) compiled into `libGraphics` and were installed
   with the `VineGraphicsBackendSdk` component. Two design pages called this layer "the backend contract", but in reality it was **the first backend's
   portable half promoted into a seam**.
2. **"Session" was four things**: the design noun (= `RenderFrameExecutor`), vk's `Device::Session` (the frame machine),
   vsg's `api::Session` (window + swap chain), vsg's `VsgDriver` (the frame driver); the identity types were split further into
   `DeviceClock::WindowId`(u32), `ResidenceTable::SessionId`(u64) and `TargetClaims::SessionId`(u32).
3. **One thing invented separately in each backend**: the value a frame head resolves for each draw call was
   `CommandPlan` + `ScreenPlan` + `ResolvedTextures` in vk and `ContentPlan::Call` in vsg.
   Both were right, but the shapes and names did not match; `graphics-sessions.md` itself said "the remaining difference is only the shape".

The audit also found two pieces of **engineering debt** (not design debt): 22 uncoded reports (6 files, the per-file ceilings in `diagnostic_code_baseline.txt`)
and 6 design-page sentences that the implementation had already overturned (`vk-backend.md` §2.17/§9.2, the end of `graphics-threading.md` §2.16, and others).

## Decision

**Adopt "repartition what already exists" (`G13`), adding no capability (`G8`); migrate in ten batches (0-9), each one revertible commit.**

### 1. Vocabulary: one concept, one name

| Concept | Name | Note |
|---|---|---|
| The frame executor | **FrameDriver** (the type stays `RenderFrameExecutor`) | The design noun `Session` retires; the verb `createSession()` stays, and the documentation says "borrow a driver" |
| The driver's identity | **`FrameDriverId`** (u32) | Replaces `WindowId` / the two `SessionId`s; `WindowId` stays as a `@deprecated` alias for one version |
| One window's swap-chain session | **WindowSession** (formerly vsg `api::Session`) | It is a window, not a frame driver |
| The value a frame head resolves | **FrameHead** + **FrameHead::DrawPlan** | vk's three types merge; vsg's `ContentPlan`/`Call` are renamed |
| A frame's submission policy | **`Submission`** (a parameter / enum, no type is built) | `Frame` / `TransferOnly` (prepare); turns the prose "transfer batch" into a noun |

**Unchanged**: the verbs and signatures of `RenderDevice` / `RenderFrameExecutor` / `RenderBackend` / `PresentTarget` / `RenderEngine` /
`Scene` / `Plan` / `PassChain`; implementation type names such as `ContentCaches` / `ContentStore` (renaming them would be pure churn).

### 2. The seam moves home: `backend/` keeps only the shared mechanisms

Six are kept (both backends really use them): `FrameGate`, `DeviceClock`, `RetirementQueue`, `Readback`,
`Keys`, `TargetPlan`; `DiagnosticDraft` is added. The other 18 headers and 18 `.cpp` files **move into `gfx_backend_vsg/internal/`**
(a pure move, zero behaviour). The `VineGraphicsBackendSdk` component shrinks with them; `libGraphics` no longer compiles vsg-only mechanisms.

### 3. Three phases per frame, the same shape in both backends

```
openFrameHead(value, present_to)   // 1 device lock: resolve target/input/content keys/uploads → FrameHead
record(frame_head, cmd)            // 0 device locks: reads FrameHead only
commit(cmd, target)                // 1 device lock: submit, present, frame accounting
```

`prepareFrame` = the same flow + `Submission::TransferOnly`. The **count** of phases is readable across backends from `FrameReport::locking`
(new optional fields: `acquisitions` / `waits`; absent = not measured).

### 4. A refusal has exactly one shape

`backend/DiagnosticDraft`: `category + code + subject + limit + message`, with `refuse()` the only writer and first-come-first-served.
vk's `ContentRefusal` becomes its alias plus "a `default`-less switch from enum → code" (learning from vsg's `HostReadbackRefusal`);
the engine side's 20 reports gain codes, and `diagnostic_code_baseline.txt` goes to zero.

### 5. Targets and the engine: each goes home

- Targets: vk extracts `TargetTable` (map + shape cache + build/rebuild + claim + park), and `Device.cpp` only assembles and forwards;
  vsg's `VsgTargetRegistry` + `HostTargets` are named by the same concept. **A claim is a property of the target table**, not a third table.
- Engine: extract `detail::SessionShape` / `FramePump` / `SurfaceLedger`, and `RenderEngine` returns to being the composition root;
  zero change to the public surface is an acceptance condition.
- Collection: **one implementation** (the snapshot + `selectSnapshot`), deleting the second walk and memo in `ScenePublisher`.
  Measure first, then change; revertible as a whole batch.

## Rejected roads (and why)

| Option | Why not |
|---|---|
| Split `RenderDevice` into six interfaces | Only the test doubles would benefit (they already override just the 5 pure verbs, thanks to default implementations); only a second API family forces that shape (`G8`) |
| One shared `ResolvedDraw` type for both backends | Only type erasure, pushing the complexity onto every reader (the reverse of `G7`); a unified shape is enough |
| Merge `ResidenceTable` and `ResourceTable` into one | One answers "who may build this key", the other "may this residence go"; merging loses either the three states or the pin (`G1`) |
| Use `Session` instead of `RenderFrameExecutor` as the public type | Changing a public API for one word, with a large migration surface and zero benefit (`G8`) |
| Turn `RenderBackend` into a value of two pointers (splitting T32) | Breaks 2 backends + 5 test doubles, and there is no consumer today (`G8`); recorded as a trigger condition |
| Delete the direct collection path right away | `T34` is equivalence evidence, not performance evidence; measure first, then change |
| Unify the five failure enums (A10) | They answer different questions, and unifying loses precision (`G8`) |
| Retire vsg in this round | A product decision that has to be raised separately; the owner has stated both backends stay |
| One big commit | Not revertible, not verifiable batch by batch |

## Consequences

**Good**

- A fourth backend can, for the first time, read a seam with **6 mechanisms only**, without learning vsg's `FrameCompiler` / `VariantPool` / `PhaseTable`;
  nor Vulkan's `subpass` (if batch 8 lands).
- "Three phases per frame" goes from two paragraphs of prose to a contract plus cross-backend numbers; "the record path takes no lock" has a shared reading for the first time.
- One concept, one name: a newcomer no longer has to learn "session here means a window".
- The diagnostic surface collapses from three shapes into one; the engine-side baseline goes to zero.
- The engine's threading policy changes land in `FramePump` and no longer spill across 9 files; there is one collection rule.
- Net code deleted (a second selection, a second refusal shape, vk's two plan types, one gate).

**Cost / risk (stated plainly)**

- Batch 3 is a move of 36 files (18 headers + 18 implementations); one missed `.cpp` is a link error, and the rollback granularity is coarse.
- Batch 2 touches threads and locks and must run the whole of TSan/ASan; when vk's and vsg's gates are merged into one, the semantic details (leases, recursion,
  diagnostic re-entry) have to be aligned one by one, and any difference is a behaviour change.
- Batch 7 may become slower (publish enters the query path); only measurement allows it to land.
- Though the public surface keeps "the same signatures", adding fields to `FrameReport` and moving 18 headers are a **source-level break** (for consumers outside the repository);
  the documents and ADR statuses of batches 3 / 4 must be updated in the same batch.
- This machine (lavapipe) cannot measure a concurrency gain - this document does not use it to prove a benefit, only to prove the cost structurally (no lock on the hot path,
  paying only on a miss).

**Trigger conditions (when to reopen a rejected item)**

1. A backend of a second **API family** (D3D12 / Metal) appears ⇒ split `RenderDevice` (D1) and remove the Vulkan spellings (batch 8) together.
2. The first factory that "only wants to return a `RenderDevice`", or a third backend ⇒ split the `RenderBackend` aggregate (T32).
3. A consumer that holds a frame value across frames appears (a render graph / offline rendering) ⇒ split `FrameValue` / `FrameHandles`.
4. A real host "edits while baking" and batch 2's waits enter the frame budget ⇒ go back to `ADR-2026-11-15`'s multi-session extension.

**Explicitly not done**

- No extension point without a consumer (`G8`); no third-party dependency.
- Do not change the capabilities the two backends are **deliberately different** in (borrowed-depth release semantics, memory pools, pipeline-cache files, GPU timing) - they are
  written into the user documentation's difference table rather than smoothed over.
- Do not take on any of the backlog's trigger-gated items in this ADR (multi-frame in flight, dynamic rendering, VMA, LOD, spot-light shadows, …).

## Implementation status

- **Approved (2026-11-16)**: both ADRs and every batch approved in one go. The three behaviour changes **as one batch** (BC-08 `before_draw` no longer called implicitly,
  BC-09 vk `setRenderTarget` pre-building, BC-10 host reports not counted in the engine's `diagnosticCount`); `A-01` takes **option 1**
  (vk pre-builds too, adding CAP-86) rather than retiring the verb.
- **Stage one (audit)**: done (`../research/graphics-2026-11-16-redesign.md` §2-§5).
- **Stage two (target design + this ADR)**: done and approved.
- **Batch 0 (characterization) + batch 10 (convention sweep) landed (2026-11-16)**: the three new cases for CAP-44 / CAP-45 / CAP-86;
  the four normative passages (one `///` line per field, no section comments, enum underlying types, numeric types and accessors/commands, the ownership vocabulary) and the sweep of 26
  section comments. Evidence: `test_vk` 140 and `test_vsg` 564 all green, nine gates with no new findings.
- **Batch 1 (vocabulary and identity) landed (2026-11-16)**: `backend::FrameDriverId` (u32) becomes the only definition of a frame driver's identity,
  `TargetClaims::SessionId` (u32) and `ResidenceTable::SessionId` (u64) merge into it (the latter narrowed to u32; there is no 64-bit source anywhere in the tree);
  vsg `api::Session` → `api::WindowSession` (files, includes, the `tests/test_vsg/CMakeLists.txt` list, `SessionOptions` →
  `WindowSessionOptions`); vk `Device::Session` → `Device::FrameDriver`; `DeviceClock::WindowId` keeps a `@deprecated`
  alias for one version. **A pure rename, zero behaviour**: not one of the three suites' case names changed (they are fingerprints), and
  `VkMultiSessionTest.TwoDriversShadeWithTheirOwnDefaultProgram` / `VsgBackendTest.TwoDriversKeepTheirOwnDefaultProgram` /
  `HeadlessBackendTest.OneDeviceServesSeveralAreasWithoutCrossingThem` stay green after the rename; `DeviceClock`'s **verbs** are still `addWindow` /
  `windowCompleted` (only the type changed; the verbs are left to the next batch).
- **Batch 2 (three phases per frame + frame gate) landed (2026-11-16)**: `backend::FrameGate` has one implementation only (`enterShared` /
  `enterExclusive` + lease + a per-gate per-thread nesting stack + `Counters`), and vk's device-wide gate and vsg's `internal::DeviceGate` both became it;
  13 vk host verbs change from "deadlock on the same thread" into a refusal value plus one `vk.device.reentry`; `FrameReport::locking` lands (one case per backend
  asserting "a steady-state constant + waits 0"). **Batch 2b (the `FrameHead` vocabulary) landed the same day**: the value a frame head resolves is called `FrameHead` +
  `FrameHead::DrawPlan` in both backends - vsg's `internal/ContentPlan` (the `ContentPlan` / `Call` classes) is renamed,
  and vk's `CommandPlan` / `ScreenPlan` / `ResolvedTextures` merge into the new header `internal/FrameHead.hpp` (one verb `planDraw`
  with two overloads; frame-level texture/sampler facts hang off `FrameHead`; the screen plan is a `DrawPlan` too, told apart by `kind`). **No shared header is built**; what is unified is
  the names and the shape (the `ResolvedDraw` in this document's "Rejected roads"). A pure rename/merge with zero case renames: `test_graphics` 607 / `test_vk` 159 /
  `test_vsg` 571, parity 13; the nine gates plus `content_seam` / `vk_exposure` all green.
- **Batch 3 (the seam moves home) landed (2026-11-16)**: `sdk/vine/graphics/backend/` keeps only 7 mechanisms + the guard - `FrameGate` /
  `DeviceClock` / `FrameTimeline` / `RetirementQueue` / `Readback` / `Keys` / `TargetPlan`; 17 headers and 16 `.cpp` files
  (of the original 18 + 18, `FrameTimeline` stays and `ResourceTable` never had an implementation) move into
  `gfx_backend_vsg/{include/vine/backend_vsg/internal,src/internal}`. `FrameTimeline`'s criterion: `DeviceClock` holds it and
  `RetirementQueue::retire/advance/retirePoint` take it as a parameter - moving it is a seam refactor, not a move (`G1`); vk's comments in Device.cpp
  refer to it too. The moved classes switch to `VN_BACKEND_VSG_API` (`VN_GRAPHICS_API` would expand to dllimport inside a plugin, breaking the Windows link),
  and no longer include `BackendHalfGuard.hpp` (they are no longer that half). `libGraphics`'s `src/backend/` is down to 6 `.cpp` files; the new gate <!-- drift-ok -->
  `check_include_hygiene.py` pins the seam's **file set** (one extra file turns it red). `CoreTimelineTest` (5 cases) moves with `FrameTimeline`
  into `tests/BackendSeamTest.cpp` (all three test binaries compile it); **zero case renames**, `test_graphics` 612 / `test_vk` 164 /
  `test_vsg` 571, parity 13, the nine gates all green.
- **Batch 4 (one refusal shape + diagnostic origin) landed (2026-11-16)**: the seam gains `backend/DiagnosticDraft` (category + code +
  subject + limit + message, with `refuse()` the only writer, first-come-first-served, and `diagnostic()` converting it into the SDK's value); vk's `ContentRefusal` becomes
  its alias, with reasons changed to the `ContentRefusalCause` enum plus a `default`-less switch in `contentRefusalCode()` (D-22: adding a reason without writing a code
  does not compile; `VkContentRefusalTest.EveryCauseNamesAStableCodeAndTheDraftIsTheSharedShape` walks the whole table and checks the shape); vsg's read-back refusals
  (`HostReadbackRefusal`) produce the same value through the draft as well. All 20 engine-side reports carry an `engine.*` code (`detail::engineProblem` is the only
  constructor), and one host report carries `appfw.viewer.foreign_view`; `scripts/diagnostic_code_baseline.txt` is **zeroed** (gate: 122 codes
  aligned in both directions, 0 uncoded reports). **`RenderDiagnostic::origin` (Engine / Device / Host)** lands (A-06 / CAP-87): the three-argument form of
  `reportEngineProblem` is the host entry point and carries `Host`; `diagnostics().report` is the engine/recipe entry point and carries `Engine`; a device report defaults to `Device`.
  So `diagnosticCount()` / `diagnosticsClean()` / `diagnosticCount(category)` count only engine + device (**BC-10 lands**), and a host has
  `diagnosticCount(DiagnosticOrigin::Host)` separately; the witness is `RenderEngineTest.AHostReportIsCountedApartFromTheEnginesOwnFindings`.
  Zero case renames: `test_graphics` 613 / `test_vk` 165 / `test_vsg` 571, parity 13.
- **Batches 5-18 (S4-S8) all landed (2026-11-16 / 11-17)**:
  - **S4**: B5a `0e9a9b84` (vk `TargetTable`: map + shape / build + lease + park; `TargetClaims.{hpp,cpp}` deleted),
    B5b `81e0ba0d` (`TargetImages` → `TargetObjects`; vsg's `OffscreenTarget` → `TargetObjects`, `HostTargets` →
    `TargetTable` merged with `VsgTargetRegistry`), B11 `1d57dc77` (SDK `LightSet`: the lights as one immutable shared value;
    `FrameHead::storageGrows()` as the steady-state witness).
  - **S5**: B12 `efc01336` (the `View` value: `CameraSnapshot` + `CollectView` + viewport + layer_mask),
    B13 `5b22c289` (`RenderSurface` → `RenderArea`), B14 `001b2b80` (the two values `Target` / `Output`; vk
    `setRenderTarget` really pre-builds; the `releasePass` contract).
  - **S6**: B6 `2d47390e` (`RenderEngine` as the composition root: the three mechanisms frame pump / session shape / surface ledger), B7 `16c55e69`
    (the scene's draw list has one selection rule only).
  - **S7**: B15 `955f7a94` (`Capabilities` → `Features` / `Limits`; read-back takes an `ImageRef`), B8 `c200d667`
    (`DeviceFormat` + `RenderPassKey` replace the Vulkan spellings in the seam), B16a `5e31e89b` / B16b `82b4ec3f` /
    B16c `49645332` (one pass authoring shape: `tag` deleted, `FrameUpdate`, `PassDesc` the only author).
  - **S8**: B17 `7b83a509` (`ContentHalves` → `ProgramHalves`, `ContentSets` → `BindingSets`,
    `ContentPush` → `PushRange`; the naming rule written into `graphics-architecture.md` 2.12), B9 (documentation truthfulness + the user documentation regrouped by
    capability: `.ai/design` reconciled page by page, `docs/guides/graphics-backends.md` replacing the three per-backend pages).
  The counts when B9 landed: `test_graphics` **617** / `test_vk` **167** / `test_vsg` **572** / `BackendParityTest` 13;
  the nine documentation/API gates all green; the demo at `0 [error]`.
- **Stage three (migration)**: **done** (2026-11-17). Batch progress is archived in the git commit bodies (each batch lists its capability ID triples),
  and this document only records status; the batch table and order remain governed by §10 of `../research/graphics-2026-11-16-redesign-r2.md`.
- **This ADR was changed to "Accepted" when the last batch (S8) landed**; no batch was abandoned.
