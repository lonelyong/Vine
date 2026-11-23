# ADR-2026-11-23: retiring the reference backend — the seam keeps promises, not a second reader

- Status: **accepted** (2026-11-23). It states the terms of backlog `T60`; the deletion itself is that one batch
  and is not performed by this record.
- Date: 2026-11-23
- Scope: what the retirement decides and therefore obliges - the entry condition of the SDK seam (`C1`), the fate of
  the 24 `vsg.*` diagnostic codes (`C4`), how the two comparison documents collapse into vk facts (`C5`, `K6`), the
  order the floor raise imposes on the batch (`K5`), and the obligations that remain if the reference STAYS under
  another owner (review 8 §4.8). The one source edit it lands is a paragraph in `Capabilities.hpp` (`K1`).
- Related: `.ai/research/graphics-2026-11-23-review-8.md`; `graphics-architecture.md` 2.9 / 2.11 / 2.12 / 2.15;
  `vk-backend.md`; `vsg-backend.md`; backlog `T60`, `T54`, `T11`, `T58`, `T62`, `T30`;
  [ADR-2026-11-14](ADR-2026-11-14-vk-becomes-the-default-target.md),
  [ADR-2026-11-16](ADR-2026-11-16-no-backend-half.md),
  [ADR-2026-11-18](ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md).

## Background

`T60` has been the ledger entry for "retire `vsg`" since the switch ADR deferred the deletion to a later record.
Review 8 (§1-§4) established the facts the deletion rests on. The owner then set two premises that change what the
record has to argue:

1. **D3D12 and Metal are committed**, not hypothetical (owner decision, 2026-11-23). The skeleton plugin exists and is
   unwired (`src/plugins/gfx_backend_d3d12/`), so the nest family is a plan with a directory, not a wish.
2. **The reference may STAY**, possibly under another owner (owner decision, 2026-11-23). So "what happens when it
   goes" is not the only question; "where does each concept belong if it stays" must have the same answer, because a
   design that only works when one author holds both implementations is not a design.

Three measurements turn the retirement from a cleanup into a decision:

- **The seam's entry condition is a COUNT.** `graphics-architecture.md` 2.9 and `scripts/check_include_hygiene.py`'s
  `SEAM_HEADERS` both define the seam as "both backends use it". The deletion removes one of the two readers, so the
  sentence that decides membership becomes unfalsifiable (`G11`) at exactly the moment it has to decide the most.
- **The engine reads none of the mechanism headers** except `DrawState.hpp`. After the deletion each of the eight has
  exactly ONE consumer, so membership can never be re-derived from readers again: a rule is needed, not a count.
- **The eight are not one kind of thing.** `FrameGate` is the enforcement of a documented refusal; `DeviceClock` /
  `FrameTimeline` produce two watermarks a host reads; `RetirementQueue` produces `RetentionStats::device_waits`;
  `TargetPlan` decides whether a host-visible pipeline count moves; `Readback`, `DiagnosticDraft` and `DrawState` are
  vocabulary. A rule has to say which of those facts the SDK keeps - and it has to survive a second OWNER as well as
  a deletion.

Measured on the tree this record argues from (2026-11-23, `dev`):

| Fact | Measured value |
|---|---|
| seam mechanism headers / SDK headers both backends include directly | 8 / 25 (`scripts/check_include_hygiene.py`) |
| public SDK headers naming `vk`/`vsg`, and occurrences of either word | 19 headers, 41 occurrences (case-sensitive `\b(vk|vsg)\b`) |
| registered diagnostic codes, of which `vsg.*` | 125, of which 24 |
| `RenderDevice` virtual verbs, of which pure | 19, of which 5 |
| advertised capability surface | 3 features, 4 limits, 2 booleans |

The review's own counts were taken before the `T15` move landed and one of them (the code registry) was not measured
from the file; the table above is what the tree says today.

## The observation that decides the mechanism question

For each of the eight seam headers there is a sentence a HOST can read that is true only because the mechanism has
ONE implementation instead of one per backend:

| Mechanism | The promise it keeps | Where a host reads it | What a second family would re-derive |
|---|---|---|---|
| `FrameGate` | one frame at a time per executor; a re-entrant host verb is REFUSED, not deadlocked | `RenderDevice.hpp`, threading section | the re-entrancy answer, verb by verb |
| `DeviceClock` | a frame reported complete is provably complete, and its slot is safe to record into again | `FrameReport.hpp` (`FrameProgress.completed`) | the proof of completion |
| `FrameTimeline` | the same watermark, N frames wide, never going backwards | `FrameProgress` + `FrameTimeline.hpp` | the slot bookkeeping behind it |
| `RetirementQueue` | a release is SCHEDULED, not synchronous, and the frame path does not idle the device | `FrameReport.hpp` (`RetentionStats::device_waits`) | when a deferred destroy may run |
| `Readback` | a refusal is classified before any wait: `Unsupported` ("never") versus `NotReady` ("not yet") | `ReadbackOutcome` and the format table | the four-state answer |
| `DiagnosticDraft` | one refusal shape, one stable code identity per module | `RenderDiagnostic` | the shape (each backend OWNS its codes) |
| `DrawState` | identity is what a pipeline is keyed by; a per-draw value never recompiles | `PipelineKey`, `RenderState.hpp` | the two halves of the key |
| `TargetPlan` | a resize does not recompile a pipeline; a SHAPE change does | `PrepareReport::pipelines` ("Pipeline identities THIS call compiled"), `RenderArea.hpp` ("device and compiled pipelines stay"), `RenderDevice.hpp` (`initialize`: "nothing is torn down, nothing is recompiled") | the three-way decision |

The last row decides this record, because review 8 left it open. §3 `C1` proposed moving `TargetPlan` into the plugin
on the ground that "its table is internal and its consequences are witnessed elsewhere". The consequences are exactly
what a host reads as a NUMBER: `PrepareReport::pipelines` is the field the resize rule exists to keep at zero, and
`RenderArea` / `initialize()` state the same promise in words. Two copies of the decision could each be
self-consistent and still disagree about that number - a defect a host finds and no test can name (`G4`, `G6`, `G11`).
So the review's exception is **withdrawn**: one rule covers all eight, and `TargetPlan` stays.

> **The rule.** A mechanism lives in the SDK when it is the family-neutral implementation of a promise a host can
> observe, and moves into a plugin when its only justification is that two backends happen to call it.

Three properties make it a test rather than a phrase:

- **It is falsifiable after the deletion.** Each listed header states its promise (2.9's table, one line per
  mechanism), and `check_vk_exposure.py` V2/E1 already fails if a listed header grows a family spelling - so
  "family-neutral" is gated, not asserted.
- **It gives the same answer under the second premise.** With two owners, a promise enforced twice is two answers to
  one host (`G6`); a promise implemented once cannot drift.
- **It keeps `G8` honest.** The list does not grow on credit: a candidate must name a sentence a host can act on,
  never a future backend.

## Options

| # | Option | Benefit | Cost |
|---|---|---|---|
| A | Move the five all-mechanism headers (`FrameGate`, `DeviceClock`, `FrameTimeline`, `RetirementQueue`, `TargetPlan`) into `gfx_backend_vk/internal`, applying "both backends use it" literally | The sentence stays literal; nothing in the SDK lacks two readers today | A future family copies five headers back; each team re-derives the re-entrancy and watermark answers (`G5`, `G6`); dearer for ever to save a batch |
| B | **Keep them, and change the entry condition from "two readers" to "the promise a host can observe"**, recorded per header | One rule for all eight; one implementation per promise; a falsifier that survives both the deletion and a second owner | The list can no longer be re-derived by a script; the promise table is maintained with the headers (`G11` paid in prose the gates already read) |
| C | Keep them and let the arriving D3D12 backend move whatever it does not need | No decision today | The definition stays untestable until then, and the D3D12 author would have to invent the seam (`G9` read backwards) |

For the second premise the same question has one more option, which is not an alternative but a consequence: keep the
reference and let the SDK ship the OTHER owner's internals. Review 8 §4.8 measured that this is already wrong for
`LoadOpVariantKey` / `ImageLayout` and it was fixed (`T15`, `7b883904`): one backend's render-pass key is not the
seam. The ADR applies the same answer to everything else - placement is decided by the promise, not by who owns the
folder.

## Decision

**B**, and the retirement's terms follow from it.

### D1 — A shared mechanism earns the SDK by a public promise, not by a second reader

- The eight listed headers stay in `sdk/vine/graphics/`, and 2.9 states the promise each one keeps (the table above
  is its one-line-per-mechanism summary).
- The list changes when a candidate can name a host-observable sentence, and when one of the eight can no longer name
  one - not when a reader appears or disappears.
- `scripts/check_include_hygiene.py` keeps measuring "both backends reach it" for as long as two backends exist; the
  batch that deletes `vsg` re-pins `SEAM_SHARED_HEADERS` to "reached by the shipped backend", and `SEAM_DOCS` (both
  pages name every mechanism) is unchanged. The gate keeps its present meaning until then; this record does not
  pre-empt a gate edit the deletion owns.
- `SEAM_HEADERS` stays 8 and no header moves: `G10` sees no ABI change, no include change and no rebuild.

### D2 — 24 diagnostic codes leave; the registry keeps one prefix owner and no aliases

- `scripts/diagnostic_codes.txt` holds 125 codes; 24 are `vsg.*`. Retiring the backend deletes those 24 lines in the
  same batch as the code that emits them (`check_diagnostic_formats.py` fails either way round).
- **No aliases and no forwarding.** `BC-17`…`BC-21`'s rule ("no deprecated aliases") decides it, and a
  `vk.same_as_vsg_x` code would be a lie: a different module reporting a different cause. A host that gated on
  `vsg.*` gets nothing instead of a classification - that is the compatibility event this record exists to state.
- The event is written where the reader is: `docs/guides/graphics-backends.md` (the user page) and the
  `scripts/diagnostic_codes.txt` prefix list drop the `vsg.*` line in the deletion batch; the ADR is the record.
- `engine.*` and `vk.*` are untouched, and "a code is never reworded and never reused" keeps holding: a retired code
  is deleted, never recycled.

### D3 — The comparison documents collapse into vk facts; the facts that are only stated as differences move now

- `graphics-architecture.md` 2.12's peer table loses its subject when one backend remains. It is **deleted** in the
  deletion batch, and the surviving facts live where they already are: `vk-backend.md` §4.3 names vk's units and
  2.12's first half (the `Content*` naming rule) stays as a naming rule, not as a comparison.
- The user page's §3 "intentional differences" table collapses the same way: each row either becomes a vk FEATURE in
  `vk-backend.md` or disappears because the difference was the reference's half. It is not deleted wholesale - a
  reader must still find the rows.
- **Three rows are vk FACTS today and move now**, because a difference column is the wrong evidence for a behaviour
  the backend has: the declared vertex input is the vertex stage's own set (`vk-backend.md` F5 already states it),
  a refused target is named at build time (`F4` gains the two codes), and D24 / borrowed depth answer `Unsupported`
  (`F11` gains the fact).
- `Capabilities.hpp` states the contract instead of the survey (`K1`): a FEATURE says what the device can do, and a
  report part says what this BACKEND measures; `GpuTimings` is the device fact, `FrameReport::timing` is the backend's
  answer, and the two are not the same claim. It also stops naming the backends that happen to answer it today.

### D4 — The batch order is the argument: raise the floor, then simplify, then delete

`T60` bundles four items; this record fixes their order and says why the order is the point:

1. **`T54` first** - raise vk's floor to `VK_EXT_extended_dynamic_state`/`_state3` and delete
   `ContentPipelineKey::RasterState`. This is the capability the retirement is supposed to buy: delete the reference
   first and the only backend keeps baking five states for ever, which makes "retire to simplify" false.
2. **`T11`** (dynamic rendering) rides the raised floor; it cannot precede it.
3. **`T58`** (alpha-to-coverage) becomes one pipeline bit, and its shader `gl_SampleMask` workaround is deleted rather
   than inherited.
4. **Then the deletion**: the folder, `T10`'s vsg half (deleted, not built), the 24 codes, the parity file moved into
   `test_vk`, the seam gates re-pinned, the documents collapsed (`D3`).

Honest cost: after step 1 no in-tree backend exercises a Vulkan 1.2-class device, so the argument "vk exists to test
the seam BELOW the reference's floor" retires with it, and `vk-backend.md`'s floor sentences change. The replacement
is the second FAMILY when D3D12 lands - a stronger reader than a same-family second implementation, and the reason
this ordering is affordable at all.

### D5 — If the reference stays, the placements are the same and four obligations follow

Placement is identical under both premises (review 8 §4.8's table, adopted): the eight mechanisms stay; `Readback`'s
format table stays (an `Unsupported` is a host-visible answer); each backend owns its codes and its prefix;
`LoadOpVariantKey` / `ImageLayout` stay out of the SDK (landed); `PinnedResources` stays vsg-internal;
`ResidencyClaims` moves to the SDK mainline **when** a second explicit-API backend needs it (`T30`'s trigger, which the
D3D12 work will hit - the decision is recorded here so the D3D12 author follows it instead of re-deciding).

Four obligations only an OUTSIDE owner exposes, and where each lands now:

1. **The seam has a conformance suite but no entry condition.** `tests/BackendSeamTest.cpp` is what a backend must
   pass; that sentence is written into the file's note and the user page's §5 in this batch. Cost: one paragraph.
2. **The contract has no ABI rule and no build identity.** With two owners a new virtual is an ABI break nobody sees
   until a plugin loads. The rule (a new virtual is defaulted and documented; a backend reports the SDK build it was
   compiled against, so a stale `.so` is a diagnostic) is decided here and implemented by backlog `T65` - the incident
   that motivates it was measured on 2026-11-23 (a stale plugin segfaulted `VsgBackendPluginTest.CreateBackendByName`
   and only a rebuild told it apart from a regression).
3. **Registry identity must not depend on load order.** `RenderBackendRegistry::registerFactory` is
   `factories[name] = factory`: the last registration wins and nothing is reported. The decision is **keep-first and
   report the attempt** - the first registration owns a name, because a plugin that silently shadows the binary's own
   backend is the shape the tree already measured. Implemented by backlog `T64`, not by this batch.
4. **The cross-team documents must state their obligations.** The user page's difference table now says at its head
   that it lists the differences the CONTRACT ALLOWS (a row that violates §2's promises is a defect, not a
   difference); the code registry's header already says a code is permanent and per-module; and `T62` (a public SDK
   header states the contract, not the survey) matters MORE with a second owner, because a header citing one backend
   reads as the SDK endorsing that team's implementation as the reference.

### What this record deliberately does NOT decide

- **The shader source language** (review 8 `P4`), deferred by the owner on 2026-11-23. It is separable: the retirement
  removes a SPIR-V consumer, not the SDK's GLSL text (`BuiltinShaders.hpp`), so nothing here changes what the deletion
  deletes. When it is decided it gets its own record, and D3D12's arrival - not the retirement - is its trigger.
- **Whether the retirement happens now.** `T60`'s trigger is unchanged (the `T35` gate green and the four items
  scheduled together); this record says what the batch must contain and why.

## Consequences

**Good**

- One rule, no exception: the eight mechanisms have one justification each, and it is a sentence a host can read.
  The review's awkward split ("`(b)` for four, `(a)` or a copy for `TargetPlan`") is gone (`G6`).
- The deletion becomes cheap: no header moves, no include churn, no ABI change, and `T60` is what it was meant to be -
  one batch with four items in an argued order.
- The rule survives the second premise unchanged, so the answer does not depend on who owns `gfx_backend_vsg`.
- The two facts that were only true "because two backends did it" now have a reader that outlives both: the promise
  table and the exposure gates.

**Bad / cost**

- The seam's membership is no longer script-derivable. `G11`'s price is paid in prose: a header whose promise stops
  being true is a review finding, not a gate failure. The exposure gates narrow the gap (a family spelling is still
  caught), but a stale promise sentence is a human check.
- `T54` before the deletion means the last 1.2-class implementation leaves the tree before the replacement reader
  (D3D12) exists. Between those two points the seam has one implementation and one family; the witnesses that
  survive are the golden pixel values, `BackendSeamTest` and the promise table.
- 24 codes are removed without aliases: a host that matched them loses a classification. Stated, not softened.

**Trigger (when to reopen)**

- A backend that cannot keep one of the eight promises (a family that needs a different re-entrancy answer, say), or
- a promise in the table that stops being true in a public header, or
- `T60` being scheduled: then this record is the batch's specification and the gates it names get re-pinned.

## Evidence

- **Mechanism consumers**: review 8 §3 `C1`'s measured table (engine reads: none of the eight; the five have vk, vsg
  or both), re-read at `7b883904`; the promise locations are the `@details` blocks cited in the table above.
- **Seam lists**: `scripts/check_include_hygiene.py` `SEAM_HEADERS` (8) and `SEAM_SHARED_HEADERS` (25).
- **Mentions**: 19 headers / 41 occurrences of `\b(vk|vsg)\b` under `src/viz/graphics/sdk/vine/graphics/` (2026-11-23, after the
  `K1` paragraph removed the survey from `Capabilities.hpp`).
- **Codes**: `scripts/diagnostic_codes.txt` - 125 codes, 24 `vsg.`, 65 `vk.`, the rest `engine.` / `appfw.`; the file's
  own header states the prefix-per-module rule.
- **ABI surface**: `RenderDevice.hpp` - 19 virtual verbs, 5 pure.
- **The T15 precedent** (one backend's internals are not the seam): `7b883904`, review 8 §5.3.
- **The stale-plugin incident** (why `T65` exists): review 8 §2.2, `tests/test_vsg`'s plugin segfault of 2026-11-23.

## Amendment 2026-11-23 - D4 step 1 landed

`T54` is in: the floor is EDS1 + EDS3, `ContentPipelineKey::RasterState` is deleted, the five items are commands in
`recordRasterState`, and a device without the three extensions is refused with `vk.init.dynamic_state_missing`.
Witnesses that fail when the commands are removed: `VkContentTargetTest` (a wireframe paints fewer pixels than the
filled frame AND creates no pipeline - dropping `vkCmdSetPolygonModeEXT` made both red at 1024 vs 1024) and
`VkContentDrawTest.AStateChangeIsACommandNotAPipeline` (blend on/off, pixel-checked, no compile - dropping
`vkCmdSetColorBlendEnableEXT` turned the mix into a replace). `test_vk` 176 cases / 42 suites, `test_vsg` 576,
`test_graphics` 639. Steps 2-4 (`T11`, `T58`, the deletion) remain.

## Amendment 2026-11-23 - D4 step 2 landed (dynamic rendering)

`T11` is in: `gfx_backend_vk` has no `VkRenderPass` and no `VkFramebuffer` left. A pass opens a dynamic-rendering
instance (`vkCmdBeginRendering`) over its attachment set, load ops / clear values / resolves are per-attachment fields
(colour `AVERAGE`, depth `SAMPLE_ZERO`), and pipelines are created with `VkPipelineRenderingCreateInfo` and
`renderPass = VK_NULL_HANDLE` from the attachment FORMATS of their compatibility. The four load-op render-pass variants
and the window's four variants are gone; the HUD case is now one field pair.

What the record's promise table said had to survive, and did: the attachment set is still the SHAPE (formats, samples),
so a resize recompiles nothing; a borrowed depth is still always LOADed (the borrower may never clear the lender's
image - `TargetObjects::beginPass` states the override, and
`VkContentTargetTest.ABorrowedDepthRejectsTheFragmentTheLenderAlreadyCovered` was red while it was missing);
MSAA depth still resolves with `sample0`.

The witness for the NEW requirement dynamic rendering introduces: deleting the per-attachment transition in
`DynamicRendering::beginRendering` turns seventeen `test_vk` cases red, every one citing
`VUID-vkCmdBeginRendering-pRenderingInfo-09592` (the view is not in the layout the attachment info states) - the
implicit `initialLayout` a render pass gave is now the recorder's job, and the suite proves it is done. The window's
per-image layout record has its own witness: the frame loop added to
`VkWindowPresentTest.AWindowFrameIsDrawnAndPresented` forces a presented image to be acquired again, and recording the
wrong post-present layout there makes the device report validation errors.
`test_vk` 176 cases / 42 suites, `test_vsg` 576, `test_graphics` 639; ten gates green. Steps 3-4 (`T58`, the deletion)
remain.

## Amendment 2026-11-23 - D4 step 3 (T58) lands as a decision, not as code

Step 3 was written as "becomes one pipeline bit, and its shader `gl_SampleMask` workaround is deleted rather than
inherited". The second half has nothing to delete: measured today, `gl_SampleMask` appears nowhere in `src/`, `tests/`
or the GLSL - the workaround was a design sketch that was never written, because its trigger (the first real
Mask-on-MSAA workload) never fired. So the obligation is met by making sure it is never written, and the page that would
grow it is `graphics-material.md` F7 N6.

What the step's remaining half turned out to be, and why it is recorded instead of built:

- The switch itself is one command: `VkPipelineMultisampleStateCreateInfo::alphaToCoverageEnable` as dynamic state under
the EDS3 floor this batch already requires (`VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT`, feature
`extendedDynamicState3AlphaToCoverageEnable`, issued in `ContentDraw::recordRasterState` beside `blendEnable`).
- But alpha-to-coverage derives the sample mask from the alpha of the fragment's FIRST colour output, and this tree's
forward program writes the drawable's opacity there (`builtin_forward.frag`), not the masked alpha. The bit alone would
therefore dither a faded Mask draw instead of antialiasing a cutout. The feature is three parts: that shader branch
(under `gl_NumSamples > 1`, so a single-sampled target writes exactly what it writes today), one neutral fact beside
`blend` in `DynamicState`, and the backend's switch. It can never serve the deferred G-buffer, whose attachment 0 alpha
is the occlusion the lighting pass scales.
- Building that now would add a seam field and a required EDS3 sub-feature for a capability no host uses - **no content
in the tree draws Mask into a multisampled target** (`G8`). `T58` therefore stays trigger-gated with the mechanism and
its three parts written down (backlog `T58`, `graphics-material.md` N6, `graphics-rendering.md` N11), which is what step
3 was for: the retirement must not leave a compromise for a later reader to inherit by inertia.

The rejected option is now explicit: a shader-side `gl_SampleMask` write. It existed only because vsg's render graph and
compile traversal pushed their own `MultisampleState` after the application's - a fact that retires with the backend - and
it is a spelling Vulkan-1.3, Metal and D3D12 do not share (`G6` / `G9`).

Counts at this amendment: `test_vk` 178 cases / 42 suites (the two-frame multisampled shadow case and the same-state
barrier case landed with the depth-pair fix), `test_vsg` 576, `test_graphics` 639, ten gates green. Step 4 (the
deletion) is all that remains.
