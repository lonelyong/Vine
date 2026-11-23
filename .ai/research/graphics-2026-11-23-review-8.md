# graphics review 8: concepts and capabilities with `vsg` on its way out

> **Status**: draft, non-authoritative - a working record. §3 and §4 are findings; §5 and §6 are proposals,
> not decisions. Nothing was changed in `src/` for them.
> **Last verified**: 2026-11-23, tree `265aaee5` on `dev`, plus the test-fixture fix in §2.1.
> **Scope**: the `graphics` module and `gfx_backend_vk`, read at two levels the earlier rounds did not walk:
> which CONCEPTS must exist when only one backend ships, and which CAPABILITIES vk answers today, only `vsg`
> answers, or would be lost. `gfx_backend_vsg` is read as the reference implementation - for what it proves
> and what it constrains (`../backlog/graphics.md` T60) - not as a product.
> It does not re-walk implementation quality (review 7), the incremental edit path (review 6), naming (the
> ADR series) or the threading model (review 6 §4).
> **Baseline**: `test_graphics` 639 cases / 81 suites, `test_vk` 174 / 42, `test_vsg` 576 / 83, all green on
> Linux after §2.2 and §2.3.
> **Backlog**: `../backlog/graphics.md` - this round added no entry; §6 lists the candidates.

## 1. What this round is

Three earlier conclusions make this round possible: the seam is a property rather than a namespace
(`ADR-2026-11-18`), the public surface carries a fact only when a host must decide on it
(`graphics-architecture.md` 2.15), and vk is the backend the application names while `vsg` is the reference
(`gfx_backend_vk/.../BackendFactory.hpp:16`, `app_shell` `chooseRenderBackend()`). So the questions are:

1. **Concepts.** Which nouns in the SDK and the engine exist because there are TWO implementations, and what
   is each one's justification once there is one? `G8` asks whether a capability should exist; a two-backend
   tree answers "should this abstraction exist" with evidence that the retirement deletes.
2. **Capabilities.** What does vk answer today, what does only vsg answer, and which proof dies with the
   folder? A capability the engine promises but only the retiring backend proves is a capability the
   retirement removes by accident.

The rule this round follows: **decide while the second implementation still exists.** A seam whose definition
is "both backends use it" cannot be judged after the second one is gone - the test is the deleted thing.

## 2. Two blocking facts found before the review could run

Both are environment/tree facts rather than design findings, and both are evidence for §3 C1 / C2.

### 2.1 The tree did not build on Linux - the X11 test fixture lost its default constructor

`cmake --build build --target test_vsg` failed in six translation units (`VsgBackendTest`,
`HostWindowReadTest`, `SessionMoveTest`, `SessionContentTest`, `GfxBackendVsgPluginTest`,
`WindowCompositionTest`) at every `TestDisplay display;`:

```
tests/test_vsg/TestHostWindow.hpp:444: note: candidate constructor not viable: requires 1 argument
  444 |     TestDisplay(const TestDisplay&)            = delete;
```

The X11 `TestDisplay` declares a deleted copy constructor, which is a user-declared constructor and therefore
suppresses the implicit default one. The Win32 half of the same header deletes nothing, so the tree builds
and passes on Windows and cannot build on Linux - the platform the second backend was written for. Fixed with
one declaration (`TestDisplay() = default;`) plus the reason beside it; no behaviour changes, and the fix is the
first commit of R0 (§5).

`T12` (2026-11-23) recorded the same shape from the other side: the Win32 window battery landed on a Windows
machine and the Linux half was not run. A fixture shared by both backends is exactly the file where one
platform's edit breaks the other's, and the class of failure is invisible to the suite that was run.

### 2.2 `test_vsg` segfaults on a stale plugin `.so`

After the six fixes above, `test_vsg` crashed in `VsgBackendPluginTest.CreateBackendByName`. Rebuilding
`gfx_backend_vsg` cleared it: the plugin loads `build/plugins/libgfx_backend_vsg.so`, and the SDK's vtable
changed after it was last linked (the known trap in `/memories/repo/vine-backend-vk.md`). This is the
asymmetry the retirement removes: vk's sources are compiled INTO `test_vk` and `test_vsg` (so a stale artifact
cannot lie to the suite), while the vsg path is a deployment artifact that can.

### 2.3 What the two facts mean for the rest of this page

- Every capability claim about vsg is currently less exercised than the same claim about vk. The Linux
  failure was in a *shared window fixture*, i.e. the path vsg exists to prove.
- The retirement's real risk is not the deletion; it is that the deletion removes the only reader of a claim
  whose Linux half already rotted once.

## 3. Concept findings: what exists only because there are two backends

### C1 The seam's five mechanisms have their home in the SDK because of a test that the retirement deletes

`check_include_hygiene.py` records the seam as a property: eight mechanism headers "both backends need"
(`SEAM_HEADERS`, lines 172-181) and 25 SDK headers both backends include directly (`SEAM_SHARED_HEADERS`,
192-218). `graphics-architecture.md` 2.9 states the same rule twice: "the seam's definition is still 'both
backends use it'", and "a second CONSUMER is what moves a mechanism there".

Measured against the tree today:

| Mechanism | Consumers in `src/` | Engine (`src/viz/graphics/src`) reads it? |
|---|---|---|
| `FrameGate` | vk, vsg | no |
| `DeviceClock` | vk (`TargetTable`, `ResidencyClaims`), vsg (three sessions) | no |
| `FrameTimeline` | vsg only, reached from vk *through* `DeviceClock` | no |
| `RetirementQueue` | vk (`ContentCaches`, `Device`), vsg (seven units) | no |
| `TargetPlan` (`planTarget`) | vk `TargetTable`, vsg `TargetObjects` / `FrameCompiler`, tests | no |
| `Readback` | vk `Device`, vsg `HostReadback` / `TargetObjects` | the *vocabulary* (`ReadbackOutcome`, the format table) |
| `DiagnosticDraft` | vk `ContentRefusal`, vsg `VsgBackend` | the refusal shape and the code registry |
| `DrawState` | vk (three), vsg (eleven) | `DrawState.cpp` builds `RenderPassKey` |

So five of the eight are pure backend mechanisms with one consumer after the retirement, and the engine never
names any of them. By 2.9's own test they belong in `gfx_backend_vk/internal/` once vsg is gone; keeping them
in the SDK then rests on a hypothetical future backend, which is exactly the consumer `G8` refuses to pre-pay
for. The other three are *vocabulary* a host or the engine can name and should stay.

Two honest notes:

- The counter-argument is real (`G9`): a second backend would have to copy five small headers back. The price
  of a hypothetical consumer is what `G8` says not to pay; the price of a decision deferred is the same
  include churn across two plugins, three suites, `tests/BackendSeamTest.cpp` and four gates, paid later.
- This is a *decision*, not a cleanup: it changes where `VN_GRAPHICS_API` exports live, what the SDK installs,
  and what the seam gate asserts. It belongs in T60's ADR (§5 R2), not in a refactor.

**What a future backend actually reuses (measured).** None of the five carries a Vulkan spelling - `Vk[A-Z]`
/ `VK_` over the five headers is **0 hits** (the only seam header with any is `DrawState.hpp`, i.e. the
`ImageLayout` leftover of T15), and `DeviceClock` / `FrameTimeline` are phrased in device / frame / window
terms ("slots in flight", "all work I submitted up to frame F is done"), which is the same question every
explicit API asks. So they were family-neutral **by construction** - that was the seam's point:

| Mechanism | D3D12 / Metal / WebGPU | GL |
|---|---|---|
| `FrameGate` | reuse as-is (one frame per executor, re-entrancy refused) | reuse, trivially held (no in-flight model) |
| `DeviceClock` / `FrameTimeline` | reuse (fences / timestamps, N frames in flight) | barely (no fence watermark; timer queries differ) |
| `RetirementQueue` | reuse (command lists in flight) | barely (nothing to defer) |
| `TargetPlan` | reuse the Rebuild / ResizeInPlace / Repair decision; `RenderPassKey` has a D3D12 analogue (PSO compatibility) and no GL one | partly (shape yes, render-pass compatibility no) |

Two things follow, and they point in opposite directions, which is why this is the ADR's question:

- **Reuse is real, so the bet is not on a fantasy.** But `T30` states the repo's rule verbatim: "a SECOND
  backend needs `PinnedResources` or `ResidencyClaims` - **then** that one moves to the SDK mainline; *not* 'a
  third backend exists', and not a second API family on its own". Moving IN later is a header move plus one
  gate update; moving OUT later is the include churn above. Asymmetry, not taste.
- **Not all five are the same kind of thing.** `FrameGate` is not a mechanism the backends happen to share; it
  is the *enforcement of an SDK contract* ("one frame at a time per executor; a host verb called from inside a
  frame is refused instead of deadlocking", 13 verbs). A new backend that re-derives that logic can get the
  re-entrancy answer wrong, which is the defect the gate exists to prevent. `DeviceClock` is the mechanism the
 *   contract needs the day `F5` (more than one frame in flight) flips. `RetirementQueue` is the mechanism behind
 *   `releaseContent`'s documented "a release is SCHEDULED, not synchronous", whose effect a host reads as
 *   `device_waits` - vk's device keeps that number at 0 because it waits each frame, but the promise is the one a
 *   future explicit-API backend also has to keep. `TargetPlan` is the only one of the five that is a pure decision
 *   function with no host-visible consequence of its own (see the rule below).

So the ADR picks one of three dispositions, and the page does not:

| # | Disposition | Cost |
|---|---|---|
| a | Move the five into `gfx_backend_vk/internal` in the retirement batch | the rule as written; a future backend copies five headers back |
| b | Keep them in the SDK and **re-justify** the list as "the mechanisms a backend needs" (2.9 and `SEAM_DOCS` stop claiming "both backends") | `G8` paid on credit; the gate can no longer be falsified by deleting a reader |
| c | Keep them and let the second backend's arrival move them (T30's rule applied to mechanisms) | the retirement batch is then not where this is decided, and the seam doc keeps an untestable sentence until then |

The review's own preference, once the promises below are read closely, is **(b) for `FrameGate`,
`DeviceClock`/`FrameTimeline` and `RetirementQueue`, and (a) or duplicated for `TargetPlan` alone** - but that
split is what the ADR has to argue, not what a review decides.

**The rule all of this reduces to** (and the one the ADR should adopt): share a mechanism when a HOST can observe
the behaviour it produces, duplicate it when the behaviour is internal. "Graphics must not know a backend" is a
Layering rule about the DIRECTION of the dependency, and it is already true here - none of the five carries a
Vulkan spelling, so none of them makes the SDK know an implementation. What the question is really about is who
enforces a PUBLIC PROMISE, and the public headers make four of the five promises a host can read:

| Promise in a public header today | Where | Observable as |
|---|---|---|
| one frame at a time per executor; a host verb called from a frame body is REFUSED, not deadlocked | `RenderDevice.hpp` (threading section), 13 verbs | a value, not a hang |
| SUBMITTED is not COMPLETED; a watermark never goes backwards | `FrameReport.hpp` (`FrameProgress`) | the two numbers |
| a release is SCHEDULED, not synchronous | `RenderDevice::releaseContent` | `RetentionStats::device_waits` |
| a resize does not recompile a pipeline | `vk-backend.md` / `graphics-threading.md`, `contentPipelines()` | the pipeline count |
| shape and extent are two concepts (`Rebuild` / `ResizeInPlace` / `Repair`) | `TargetPlan.hpp` is the table, not the promise | nothing a host reads; the promise is `TargetPlan`'s callers' |

So the first four are the *implementation of a documented behaviour*: two copies could each be self-consistent and
still disagree, which is a defect a host would find and no test could name (`G4`, `G6`, `G11`). `TargetPlan` is
the one whose table is internal - its consequences are witnessed elsewhere (`contentPipelines()`, the ResizeInPlace
case) - so it is the one that can move into the vk plugin or be copied by a future backend without a promise moving.
That inverts an earlier reading of this round: `RetirementQueue` looked like "only the wording of `releaseContent`",
but `device_waits` is a number a host can read, so the behaviour is observable and two copies could disagree about
it. On this rule the split is **(b) for `FrameGate`, `DeviceClock`/`FrameTimeline` and `RetirementQueue`, and (a)
or copy for `TargetPlan` alone** - which is also the split that survives the deletion: the four keep a justification
that a test can falsify, and the one that moves was never a promise.

### C2 `BackendParityTest` is the only cross-implementation oracle, and it is hosted in the suite being deleted

`tests/test_vsg/BackendParityTest.cpp` compares two live devices on the same `FrameDescription` - clear,
triangle, depth order, opacity blend, fullscreen copy, texture sample, engine program, shadow map, deferred
frame, prepare, empty pass, absent pass, sample limits. It is not a vsg test that happens to exercise vk; it is
vk's pixel oracle. Deleting the folder deletes the *comparisons*, and the deletion is cheap only if the
*assertions* survive it.

The parity set's coverage boundary matters here as much as its contents: no MSAA, no instancing, no cube map,
no wireframe/point, no depth readback, no pass timing, no pipeline cache, no second window. After the
retirement those capabilities have one reading - vk's own - which is the situation T60 already warns about for
the seam ("no second reading to locate a seam failure from"), applied to pixels.

Disposition: convert the 13 comparisons into vk-only assertions (readback value against a stored expectation)
**before** the deletion, and add the missing capabilities to that set as they land. A golden value is a
weaker oracle than a second implementation and it is the only one that survives.

### C3 The public SDK explains itself by naming the two backends (20 headers, 41 mentions)

No gate reads header prose, and the contract has grown sentences whose subject is a backend:

| File:line | Sentence (abridged) | What dies with vsg |
|---|---|---|
| `DrawState.hpp:36-37` | "vsg (an EDS3 floor) moves all of them; vk (a 1.2 floor) bakes those five in its own key extension" | the half of the sentence that defines the contract by comparison; the *rule* (a device MUST state what it bakes) survives |
| `ShaderAbi.hpp:242,247` | "the vsg backend renders reverse-Z ... into a top-down image"; "both were measured ... by `vsg_backend_selftest`" | the convention is implemented by BOTH backends (vk folds y and z in `foldToDeviceClip`), so the sentence is already one-sided; the cited binary was deleted in B2 |
| `RenderDevice.hpp` `releasePass` | "the vk backend retains nothing by pass identity ... the vsg backend forgets the slot id it keyed by it" | the second half; the no-op default's justification must be restated without it |
| `PolygonMode.hpp:17` | "derived in the vsg backend (`detail::mapPolygonMode`)" | vk has `StateMapping.hpp`; the sentence names the backend that leaves |
| `Capabilities.hpp:21,83` | what each backend answers today / both backends answer the same way | a host-facing contract sentence phrased as a survey |
| `DiagnosticDraft.hpp:26` | the code registry (`vk.content.*`, `vsg.readback.*`, ...) | the second family (see C4) |

Disposition: rewrite each as the contract, not the survey, in the retirement batch. A rule ("a public SDK
header does not name a backend") is gate-able as a baseline count that must fall to zero, with one exception:
a diagnostic CODE is an identity a host matches (`DiagnosticDraft.hpp:49`), so `vk.*` inside a code string is
not a violation.

### C4 Diagnostic codes are a public identity, and 24 of 168 are `vsg.*`

`scripts/diagnostic_codes.txt` is the registry a host matches against. Retiring `vsg` deletes 24 codes - a
seventh of the registry - and the repo's rule is "no deprecated aliases" (`BC-17`…`BC-21`). That makes the
retirement a compatibility event that has to be written down, not a folder deletion: a host that gated on
`vsg.target.samples_above_limit` gets nothing instead of a classification.

### C5 The peer-vocabulary table and the guide's difference table must be collapsed, not deleted

`graphics-architecture.md` 2.12's ten-row table is half vsg now (`VsgBackend`, `VsgDriver`, `WindowSession`,
`OffscreenSession`, `ContentStore`/`ContentFacts`, `CompiledPrograms`, `BindingSets`, `HostReadback`). The
reader-facing `docs/guides/graphics-backends.md` §3 is an "intentional differences" table, and several of its
rows are not differences at all but **facts about vk a host can observe**: vertex locations are declared only
where the vertex stage reads them (`SpirVModule::input_locations`), a refused target names its reason at build
time, D24 depth is `Unsupported`. Those rows must move into `vk-backend.md` as its features, or the single
backend's documented behaviour disappears with the comparison.

### C6 Host-side backend identity survives the retirement unchanged

`RenderBackendRegistry` + `BackendChoice` + `RenderControl::setDefaultBackendName` are plugin-extensibility,
not two-backend code: the app names `"vk"`, `VN_RENDER_BACKEND` overrides, and an unregistered name is refused
with the registered list (`AppShellUi.cpp:188`). One branch - "several registered, none named" - becomes
unreachable in the shipped build and remains correct for a host that loads another plugin. No action.

### C7 The retirement ledger's premise is already false: vsg has not been the default since 2026-11-18

`T60` says vsg's three justifications are "being the default while the gate is open, `BackendParityTest`'s
second leg, and the seam's own definition"; `T10`'s trigger says "(a) vsg stays the default backend". Since
B7 the application names `"vk"` explicitly, so the first of the three is gone, and T10's trigger (a) can no
longer be hit by the shipped app. Whoever runs T60 will read a premise the tree contradicted five days
earlier. Correcting it is not a decision; it is the ledger saying what is true.

## 4. Capability findings

### K1 The advertised capability surface is small and honest; one report part differs

Measured from code, not from the guide: `Features` has three values, `Limits` four numbers, `Capabilities`
two booleans. Both backends answer every one from the device (`Device.cpp:2737-2764`;
`VsgBackend.cpp:2097-2128`), and `render_targets` is `true` on both. The only asymmetry in `FrameReport` is
`timing`: vk fills it from its timestamp pool, and `FrameTiming` / `gpu_micros` appear nowhere in the vsg
plugin. That is a genuine intentional difference and the guide states it ("设备能写时间戳"不等于"本后端在报告").

What the SDK header does not state is the same fact at the point a host reads it. `Capabilities.hpp` only
says a device that cannot write timestamps reports `false` and "the backend's frame report then leaves its
GPU half disengaged" - the true⇒engaged direction is left to the reader, and vsg is the counter-example.
Disposition: one paragraph in `Capabilities.hpp` separating "the device can" (the feature) from "this backend
does" (the report part). Do NOT add a fourth feature for it: a fact only one backend satisfies is a backend
capability, which 2.15 (c) keeps off the public surface.

### K2 The feature↔report pair has no witness, and the one case skips itself

`tests/test_vk/PassTimingTest.cpp:105`:

```cpp
const FrameReport report = backend->executor().frameReport();
if (!report.timing.has_value()) {
    GTEST_SKIP() << "this device keeps no clock (see FrameTiming): nothing to attribute";
}
```

The case skips on the *observation it is testing*, so a backend that stopped filling `timing` would turn the
suite green-by-skip rather than red. This is review 6's oracle-gap family, one layer up (a capability instead
of an edit). The fix is two lines: skip on `!capabilities().features.supports(Feature::GpuTimings)`, then
`ASSERT_TRUE(report.timing.has_value())`. Cheap now, impossible later - after the retirement there is no
second reader of the pair.

### K3 One capability proof dies with vsg: two live window targets on one device

`VsgBackendTest.ADifferentWindowGetsItsOwnTarget` (line 4509) is the only case in the tree asserting that one
device serves two windows through two target objects. vk has no counterpart: `WindowPresentTest` *moves* a
single target between handles (lines 355-400), `ProbeTest` asks for the device's own window, and
`MultiSessionTest` is two frame drivers, not two windows.

Design point 2.4 ("a device is not a window") and `ADR-2026-11-13-one-engine-one-surface` (many windows are
many engines on one device) both rest on it, and so does the ceiling question vsg answers with
`VSG_MAX_DEVICES = 4` (vsg doc §9.2, no bounds check). Retiring vsg removes the ceiling - good - and removes
the only proof that vk has none.

Honest note: the vsg case is weak on its own terms (`deviceCaseAvailable()` skips, the second handle is a
dummy `int`, it asserts target identity and a counter, not two framed windows). Re-homing it is therefore not
a copy; it needs a vk case that presents a frame through two host windows.

### K4 A capability vk can claim and does not: alpha-to-coverage

`alphaToCoverage` appears nowhere in the tree. That is correct today - nothing draws Mask content into a
multisampled target - and it stops being a vsg workaround the moment the floor rises (K5): the pipeline bit
becomes one flag, and T58's `gl_SampleMask` shader path becomes dead. Record it so the retirement does not
inherit the workaround by inertia.

### K5 The floor raise is the capability the retirement is supposed to buy

`ContentPipelineKey::RasterState` exists because vk's floor is 1.2: cull, polygon, depth test/write/compare
and blend are baked into the pipeline key, so a state change is a pipeline variant. T54 raises the floor to
EDS1 + EDS3 (vsg's floor), which deletes `RasterState` and is the prerequisite for dynamic rendering (T11).

T60 lists T54 among the four items the retirement must land in the same batch. The stronger statement is
that T54 is *why* the batch is worth doing: delete vsg before the raise and vk keeps baking five states
forever, which makes "retire the second backend to simplify" false. T54's own justification - "vk exists to
test the seam BELOW vsg's floor" - is the clearest argument for vsg existing today, and it is an argument
about the seam, not about the product; T60's ADR should say that in one sentence.

### K6 What vk pins that vsg does not, stated as capability rather than as difference

The guide's difference rows read as "vsg lacks X". After the retirement they must read "the backend has X":
the cross-session pipeline cache (F12), the hand-written device memory pool with fragmentation numbers
(`MemoryStats`), the five report parts, per-pass device timing, and device-loss classification with a named
rebuild path. Folding them into `vk-backend.md` as features is C5's other half.

### 4.7 Premise set by the owner (2026-11-23): D3D12 and Metal are coming, not hypothetical

This is not a review finding but a decision the review has to design against, and it changes two of the answers
above. The whole seam was validated by **two Vulkan implementations** (vk and vsg are the same family); a D3D12
or Metal backend is the first *cross-family* reader, so the question stops being "does vsg prove the seam" and
becomes "would a non-Vulkan family fit the seam".

| # | Consequence | Evidence |
|---|---|---|
| P1 | **C1 resolves to (b), and honestly.** The mechanisms stay in the SDK: their second consumer is now committed, so this is not `G8` on credit. The `SEAM_HEADERS` list stops meaning "both plugins include it" and starts meaning "the family-neutral implementation of a public promise"; it keeps a falsifier because every listed header must also carry **no family spelling** - which `check_vk_exposure.py` (V2/E1) already enforces for Vulkan | C1's promise table |
| P2 | **The SDK's last Vulkan-shaped type has exactly one producer, and it is the backend being retired.** `LoadOpVariantKey` (and with it every `ImageLayout` field in the SDK) is *constructed only by vsg* (`TargetObjects.cpp:353`); vk builds its three render-pass variants by hand and only the engine's `DrawState.cpp` compares the key. So it leaves with vsg, and `ImageLayout` becomes **vk's own barrier vocabulary** (`BarrierPlan`, uploads) inside `gfx_backend_vk/internal`. That is T15 completed, not deferred - and it costs one backend's edit now instead of three backends' later | `grep LoadOpVariantKey`: vsg constructs, vk does not |
| P3 | **`RenderPassKey` / `DeviceFormat` were built for this day and need no change.** `RenderPassKey` is an opaque `uint64` the backend states and this layer never interprets (`kNotStated` = "this family has no such structure"); `RenderPassCompatibility`'s own doc states the rule ("what a pass structure is belongs to the device's API"). A Metal backend answers `kNotStated` and still shares pipelines correctly. They finally get the reader they were designed for | `DrawState.hpp:155-205` |
| P4 | **The largest cost the commitment forces is one the tree does not answer: the shader source language.** `ShaderProgram.hpp` says the backend compiles GLSL to SPIR-V or consumes SPIR-V, and `BuiltinShaders` emits GLSL `#version 450` text. D3D12 wants DXIL, Metal wants MSL. The family-neutral design is: **GLSL stays the SDK's shader language, each backend owns the translation** (SPIRV-Cross → HLSL / MSL), and `BuiltinShaders`' text stays the one portable source. The alternative - one source text per family - multiplies the engine's built-ins by N. This is an ADR-level decision, not an implementation detail | `ShaderProgram.hpp:34`, `BuiltinShaders.cpp` |
| P5 | **`ResidencyClaims` moves to the SDK mainline** (T30's rule, trigger now hit by decision: a second explicit-API backend needs "who may build this key"); `PinnedResources` leaves with vsg | T30 |
| P6 | **The extension-point table understates "a backend".** 2.11 says about six kinds of file; a FAMILY additionally needs its exposure gate (a `check_<family>_exposure.py`, same shape as vk's) and its shader translator | `graphics-architecture.md` 2.11 |

What stays per backend **by design**, in every family, and should be said that way in the ADR: the state → API
mapping (`StateMapping.hpp`), barrier/state planning (`BarrierPlan`), the format tables, the shader translation,
and the residency tables' storage. Those are exactly where the families differ; the seam is everything that
must not.

## 5. Proposed orderFour batches; R0–R2 pay for themselves even if the retirement never happens, R3 is T60's batch.

| # | Batch | Contents | Why now |
|---|---|---|---|
| R0 | Truth sweep, no behaviour | C7 (T10/T60 premises), the three `backend/<Header>.hpp` paths (`FrameReport.hpp:55`, `RenderDevice.hpp:139`, `PresentTarget.hpp:32`), the four `vsg_backend_selftest` citations, and `SceneBridgePipeline.h` inside the EMITTED shadow GLSL (`BuiltinShaders.cpp:129,132`) | each sentence sends a reader to something deleted; the emitted text ships to every shader compile |
| R1 | Witnesses before deletion | K2 (feature↔report), K3 (two windows on vk), C2 (parity converted to golden values + the missing capability rows) | after the deletion these have no second reader, and one of them (K3) loses its only case |
| R2 | The retirement ADR | C1 (mechanism ownership), C3 (41 mentions / 20 headers, rule + baseline), C4 (24 codes, compatibility note), C5 (doc collapse), K1 (Capabilities wording), K5 (floor + T54/T11/T58 ordering) | every one of these is a decision whose evidence is the tree as it is today |
| R3 | Delete vsg | one batch: C4, T10's vsg half, T11, T54, T58, the seam gates re-pinned | T60's rule: paid only as one batch |

### 5.1 R0 landed 2026-11-23
- The X11 `TestDisplay` build break (§2.1), and this page with its index entry.
- The three `backend/<Header>.hpp` paths now name `FrameGate.hpp` / `FrameTimeline.hpp` directly.
- `ShaderAbi.hpp`'s shadow block now states the map convention as a requirement both backends keep, and cites
  the live parity case instead of the deleted self-test.
- The emitted shadow GLSL (`BuiltinShaders.cpp`) no longer points at `SceneBridgePipeline.h` or at the deleted
  self-test; the engine comment above it and three `GraphicsTest.cpp` comments were re-homed to `VkShadowTest`
  and the parity case.
- `scripts/callgrind_diff.py`'s docstring no longer names a deleted binary or a deleted memory page.
- T10's trigger and T60's three-justifications sentence (§3 C7) now say what the tree does: the application
  names `vk`; vsg is the reference, and the EDS3-floor reading is the third justification.
- **Left deliberately, as history**: `.ai/bugs/vsg-maximize-black-band.md:67` (a postmortem keeps the evidence
  that existed when it was written), `gfx_backend_vsg/CMakeLists.txt:124` ("USED TO BE HERE" is the accurate
  note explaining the missing target) and `docs/guides/graphics.md:1586` (the user-facing "retired" note).

### 5.2 R1 landed 2026-11-23 (the witnesses a deletion must not remove)

- **K2 - feature and report tied**: `VkPassTimingTest` now skips on `!features.supports(GpuTimings)` and then
  `ASSERT_TRUE(report.timing.has_value())`. Mutation: dropping `report.timing = books.timing` in
  `Device::FrameDriver::frameReport` turns exactly that case red (it used to skip, i.e. pass). **Trap met while
  reverting it**: `test_vk` compiles the backend sources, so reverting `Device.cpp` without relinking `test_vk`
  left the mutated backend in the binary and the case stayed red 20/20 - a stale binary looks exactly like a
  regression, and only `cmake --build build --target test_vk` tells them apart. After the relink: 10/10 green.
  One branch stays "absent" by design and is NOT measured to fire: a completed, sampled frame whose two
  timestamps are EQUAL answers disengaged (`Device.cpp`: `stamps[1] > stamps[0]`), i.e. a frame shorter than
  one tick reports no clock while the feature says true. Left alone - no measurement shows it, and turning a
  zero into a value is a decision about `FrameTiming`, not a test fix.
- **K3 - two windows on one device**: `VkWindowPresentTest.TwoWindowsOnOneDeviceKeepTheirOwnPictures` creates two
  host windows, two targets before `initialize()`, presents a magenta-cleared frame into the first and a
  green-cleared one into the second, and reads the FIRST window AFTER the second presented - one shared swapchain
  would show the second window's picture there. Both handles then answer `WindowOccupied`. Mutation: asking for
  the first handle twice turns exactly that case red. The pixel half measured on this machine (no skip message).
- **C2 - every parity case states its absolute expectation twice**: `expectPixelOnBoth` asserts the same pixel on
  the reference and on the candidate; the count cases assert the candidate's own numbers as well as equality; the
  two "no picture" cases use `EXPECT_FALSE(candidate.read)`. The file header now maps each fact to the standalone
  `test_vk` case that already asserts it (ten facts), and names the five that exist ONLY here - a depth-only clear
  over a drawn picture, packet/draw counts, the prepare shape agreeing with the record, a pass with no effect, and
  an entry that names no pass - which is what makes moving this file into `test_vk` mandatory rather than optional.
  The sample-limit case is the one that retires with its successor named (`VkMsaaTest`).
- Verification after R1: `test_vk` 175 cases / 42 suites, `test_vsg` 576 / 83, `test_graphics` 639 / 81; all green;
  the parity suite 15/15; `check_include_hygiene` and `check_api_style` at their baselines.

## 6. Backlog candidates (listed for approval; not added)

- **T61 - Stability before the deletion.** Golden-ize `BackendParityTest`'s assertions, add the capabilities
  the parity set never compared (MSAA, instancing, cube map, wireframe/point, depth readback, timing, pipeline
  cache), pin the feature↔report invariant, and give vk a two-window case. Trigger: T60 is scheduled. Source:
  C2, K2, K3.
- **T62 - The public SDK does not name a backend.** 20 headers, 41 mentions, with the diagnostic-code
  exception; baseline in a gate so the count cannot rise again. Trigger: the next SDK comment sweep, or T60
  being scheduled. Source: C3, C5, K6.
- **T63 - Correct the retirement ledger's premise.** T60 no longer has "the default" as one of its three
  justifications, and T10's trigger (a) cannot be hit by the shipped app. Trigger: none (a truth fix).
  Source: C7.
- Two fixes from §4 were R1's substance and landed (§5.2): `PassTimingTest.cpp`'s self-skip (K2) and vk's
  two-window case (K3), together with the parity re-hosting (C2). What remains before R2 is a decision, not a
  fix: whether the five mechanisms of C1 move into the vk plugin in the retirement batch.

## 7. Verification

| Check | Result |
|---|---|
| `cmake --build build --target test_graphics test_vk test_vsg gfx_backend_vk gfx_backend_vsg` | clean after §2.1; before it, six TUs failed on the X11 `TestDisplay` |
| `./build/bin/test_graphics` | 639 cases / 81 suites passed |
| `./build/bin/test_vk` | 174 cases / 42 suites passed |
| `./build/bin/test_vsg` | 576 cases / 83 suites passed (after rebuilding the plugin `.so`, §2.2) |
| Doc gates for this page | `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py` - run at registration |

Two earlier-round counts are stale against this tree: the design pages say `test_graphics` 617 and
`test_vsg` 572; the numbers above are what the tree at `265aaee5` runs.

## 8. References

- `.ai/design/graphics-architecture.md` 2.4, 2.9, 2.12, 2.15 - the device/window model, the seam's property,
  the peer table, the public-surface test.
- `.ai/design/vk-backend.md` 1.1/1.3 (why a second implementation exists, what is deferred), §3 features,
  §8 acceptance.
- `.ai/design/vsg-backend.md` §9.2 (`VSG_MAX_DEVICES`), `D4` (Wayland).
- `.ai/backlog/graphics.md` T10, T11, T35, T54, T58, T60.
- `ADR-2026-11-14-vk-becomes-the-default-target.md`, `ADR-2026-11-16-no-backend-half.md`,
  `ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md`,
  `ADR-2026-11-13-one-engine-one-surface.md`.
- `scripts/check_include_hygiene.py` (`SEAM_HEADERS`, `SEAM_SHARED_HEADERS`), `scripts/diagnostic_codes.txt`.
- Code read for this round: `src/viz/graphics/sdk/vine/graphics/{Capabilities,FrameReport,RenderDevice,
  ShaderAbi,DrawState,PolygonMode,DiagnosticDraft}.hpp`, `src/viz/graphics/src/{BuiltinShaders,Readback}.cpp`,
  `src/plugins/gfx_backend_vk/src/internal/{Device,WindowSystem}.cpp`,
  `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`, `tests/test_vsg/BackendParityTest.cpp`,
  `tests/test_vk/{PassTimingTest,WindowPresentTest,MultiSessionTest}.cpp`,
  `tests/test_vsg/{VsgBackendTest,TestHostWindow.hpp}`.
