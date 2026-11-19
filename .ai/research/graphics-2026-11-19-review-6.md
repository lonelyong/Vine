# graphics review 6: the incremental edit path, and the batches after review 5

> **Status**: draft, non-authoritative - a working record; each item becomes a design correction, a
> backlog entry or an ADR when it is decided.
> **Last verified**: 2026-11-19
> **Scope**: a sixth architectural pass over `src/viz/graphics`, deliberately NOT re-walking the
> dimensions the first five rounds settled (unit contracts, layering, seam identity, allocation
> measurement, validation cleanliness, public vocabulary naming, first-contact documentation). This
> round asks the two questions the earlier rounds did not: **is the incremental edit path correct**,
> and **did the batches that landed after review 5 introduce anything**.
> **Sources**: the tree at `694f7c1d` (HEAD), `src/viz/graphics/**`, `src/plugins/gfx_backend_vsg/**`,
> `.ai/design/graphics-threading.md`, `.ai/design/graphics-frame-pipeline.md`,
> `.ai/design/graphics-scene.md`, `.ai/backlog/graphics.md`, `scripts/*`, `tests/*`, and the commands
> in §7.
> **Backlog**: `../backlog/graphics.md` - this round added no entry; §5.2 lists the candidates and the
> trigger each would carry.

## 1. Overall Assessment

### 1.1 The tree is green, and that is the finding

| Fact | Value | Command |
|---|---|---|
| `build/` vs HEAD | no work to do | `cmake --build build` |
| `test_graphics` / `test_vk` / `test_vsg` | 620 / 174 / 572 cases, all pass | ctest |
| `BackendParityTest` | 13 cases | `./test_vsg --gtest_list_tests` |
| Gates | 10 green | §7 |
| Diagnostic codes | 124 registered, 0 uncoded | `check_diagnostic_formats.py` |

Every defect below is of the class the suite and the ten gates cannot see. That is not a reason to
dismiss them: two of them are P0 correctness defects on the frame's edit path.

### 1.2 What this round deliberately did not re-walk

The first five rounds concluded clean, with evidence, on: header contract honesty, include layering,
seam identity, public vocabulary naming, allocation measurement, validation cleanliness, and the
first-contact example. Re-litigating those without new evidence is churn (`G8`), so this round walks
two other things:

- the **incremental edit path** (`ScenePublisher::ensureRows` -> `refreshRowFacts` -> `resolveSubtree`
  -> `foldDown` -> `makeSceneSnapshot`), which no earlier round read end to end;
- the **batches that landed after review 5** (`f353e0ce` `FrameContext`, `fb673b6f` `DiagnosticsHandle`
  + `BC-23`/`BC-24`, `f1a46757` `onFrame`, the `BC-26`/`BC-27` residency rework, the guide examples,
  the comment citations), which no independent pass has reviewed.

### 1.3 What was weak

The architecture is not the problem. The problem is that **the edit path has two hidden derivation
rules** - one in the fold, one in the snapshot's chunk sharing - and the only test that exercises an
edit followed by a republish compares the snapshot with itself (§3). Both P0s sit exactly in the gap
between those two rules, and each one masks the other in a small scene.

## 2. Findings

Each item states what it is, why it matters, the fix, and the `G#` it violates. Items marked
**verified** were read and reasoned about directly in this round; items marked **reported** come from
the round's parallel sub-review and were not each independently re-verified.

### 2.1 P0-1 A chunk holding only descendants of an edited node is served from the previous snapshot

**Where**: `src/viz/graphics/src/SceneSnapshot.cpp:47-64` (the chunk-share test) with
`src/viz/graphics/src/DrawListBake.cpp:84` (`foldDown`'s recursion). **verified**

**What it is**: the share test compares only `identity`, `parent`, `end`, `child_count` and
`node_epochs`. `Node::contentEpoch` is "the content announcement count of THIS SUBTREE"
(`sdk/vine/graphics/Node.hpp:395`), so one edit moves the edited node and its **ancestors**, never its
**descendants**.

**Why it matters**: `foldDown` rewrites `world`, `parent_world`, `world_box` and `local_box` of every
descendant row of the edited subtree. For a subtree wider than one chunk
(`SceneSnapshot::kRowsPerChunk == 32`, `SceneSnapshot.hpp:36`), a chunk that contains only strict
descendants passes the share test and is served from the **previous** snapshot, so the re-fold is
silently discarded for those rows. The frame then draws part of a moved subtree at its pre-edit
placement and culls it by the pre-edit box.

**Reachability**: a pure placement edit keeps `boxes` and `values` unchanged, so `may_share` stays
true (`SceneSnapshot.cpp:47`); the epoch test is the only thing that could catch it, and it cannot.
The staleness **persists**: every later republish runs the same test, so the stale chunk survives
until a row inside it is itself edited or `row_count` / `boxes` / `values` move.

**Fix**: compare the fold outputs in the share test (`current.world == prior.world &&
current.world_box == prior.world_box`), or record a per-row fold stamp that `resolveSubtree` bumps
for every row it touches and compare that.

**`G#`**: `G11` - a failure that cannot be located from the outside. It also breaks the design's
"only changed chunks are touched" and "the new frame is the new picture".

### 2.2 P0-2 The incremental re-fold seeds its inherited value from a container row that never holds one

**Where**: `src/viz/graphics/src/DrawListBake.cpp:113-118` (the write guard) with `:433-437` (the
seed). **verified**

**What it is**: `foldDown` writes `row.state`, `row.material`, `row.material_description`,
`row.program` and `row.program_description` only `if (!row.draws.empty())`. A container (`Group`,
`MatrixTransform`, a childless `StateNode`) has no draws, so those five fields stay default / null
**forever** - the fold is their only writer. `resolveSubtree` then rebuilds the `Inherited` value for
a re-folded subtree from the **parent row's** stored fields, unconditionally.

**Why it matters**: when the edited row's parent is a container - the ordinary case - the re-fold
starts from a default state and a null material and program. Everything an ancestor `StateNode`
contributed is silently dropped. `selectRows` reads exactly those fields for the command
(`DrawListBake.hpp:556-573`: `cmd.authored_state = row.state`, `row.material`, `row.program`), so the
loss reaches the picture: depth / polygon / shading state and the material and program disappear for
the whole edited subtree.

**Reachability**: the incremental path is taken whenever the structure is unchanged, some row's epoch
moved, and the changed row's own facts are still "hot" (`ScenePublisher.cpp:255-300`); a matrix edit
under a group qualifies. `fillOwnFacts` does not touch `local_box`, so the `non_hot` guard does not
accidentally force a full rebuild.

**Fix**: write the inherited payload on every row in `foldDown` (drop the `!row.draws.empty()` guard
for those five fields), or reconstruct the `Inherited` by walking up the chain and re-merging each
ancestor's own facts.

**`G#`**: `G3` - the fold has a second, hidden rule that only the edit path uses; and `G11`.

**Interaction**: P0-1 and P0-2 mask each other. In a small scene the shared chunks keep serving rows
that are still correct, so a wrong re-fold is invisible; in a large scene a wrong re-fold is
partially discarded. That is why both survived.

### 2.3 P1 - six items

| # | Where | What | `G#` | Fix |
|---|---|---|---|---|
| P1-1 | `Diagnostics.hpp:132-136` + `RenderEngine.cpp:585` | The diagnostic counters are plain `std::uint64_t` arrays. The engine half reads `diagnosticCount()` on the **engine thread** (`runEngineHalf`), while the host thread can call `report()` concurrently (a refused `applyPlan`'s Busy episode, `publish()` with a null target). A data race: lost or torn counts, and a wrong per-frame delta. The repo's own rule for this shape is in `DrawListMemo.hpp` - counters another thread may read are relaxed atomics. **verified** | `G6` | make `per_category_` / `per_origin_` `std::atomic<std::uint64_t>` (relaxed) |
| P1-2 | `RenderEngine.cpp:88-105` | `setDevice` calls `releaseFrameDriver` and nulls `session_` with no `waitIdle` and no busy check, while the threaded engine half may be inside `session_->executeFrame`. Every other session change is guarded (`applyPlan` / `removePass` refuse while busy, `shutdown` stops the pump first, `setFrameThreading(false)` waits). Use-after-free on the one lifecycle swap that can race a frame. **verified** | `G6` | `pump_->waitIdle()` at the top of `setDevice`, or refuse while busy like `applyPlan` |
| P1-3 | `FrameResources.cpp:200` | `FrameResources::releaseAll` has **zero callers** in the tree (grepped `src/` and `tests/`); `~FrameResources` is `= default` and neither `shutdown()` nor `~RenderEngine` calls it. One device serving several engines is a supported shape, so an engine torn down while its device lives destroys its declared transient `RenderTarget`s without the `releaseRenderTarget` announcement. **verified** | `G4` / `G10` | call `resources_->releaseAll(device_.get())` in `~RenderEngine` (or `shutdown`) before the members go |
| P1-4 | `FrameRunner.cpp:113` | `loop()` runs `job()` bare, with no exception barrier. The engine half allocates (a moving camera allocates every frame) and calls into the backend; inline mode propagates the exception to the host, threaded mode lets it escape the thread function and `std::terminate` the process. **verified** | exception safety | wrap the job in a try/catch that reports and drops the frame |
| P1-5 | `docs/guides/graphics.md:510-512`, `:583-589` | The `5402a8aa` block replacement left two old block tails and two orphan ```` ``` ```` fences. Rendered, the first orphan fence swallows the §3.4 pass table, the 要点 list, the §3.5 intro and its whole block; the second swallows the manual-vs-builder table and all of §3.6 / §3.7 / §3.8's head. The `### 3.5` heading itself is gone (numbering jumps §3.4 -> §3.6) while `tests/test_graphics/GuideExamplePasses.cpp:2` and the commit message still say §3.5. `check_doc_symbols.py` only reads statements inside `// example:` blocks, so residue outside fences escapes it. **verified** | `G4` / `G11` | delete the two stray fragments and the orphan fences, restore `### 3.5`; add a fence-balance rule to the doc gates |
| P1-6 | `src/plugins/gfx_backend_vsg/src/internal/BlockStorage.cpp:341` | `row.bytes = d->arena.capacityBytes()` - a fixed allocation, so the number never moves. `ResidencyRow::bytes`'s contract (written by the `BC-27` commits) is "the GPU bytes this kind occupies NOW ... a release has to make it go DOWN". Every other fill is honest: vk uses live bytes, vsg's `Textures` row uses `MaterialImages::bytes()` which is decremented on release. A host building the documented trim signal on vsg gets a constant. **verified** | `G4` / `G6` | fill `live() x copies x block_bytes`, or re-document the field |

### 2.4 P2 - reported items

Reported by this round's parallel sub-review; the line numbers are its measurements, not independently
re-verified here. Grouped by the batch or area they belong to.

| # | Where | What |
|---|---|---|
| P2-1 | `EngineCompile.cpp:293` + `RenderEngine.cpp:332` | threaded mode resets the engine-half micros before the host can read the previous frame's, so `record_micros` means different things in the two modes |
| P2-2 | `ScenePublisher.cpp:209` | a rootless scene answers `Posted` on every publish and mints a fresh source, instead of keeping a stable empty snapshot |
| P2-3 | `PassChain.cpp:298` | `apply()` copies the whole plan for nothing (`PassRegistry::apply` takes `const Plan&`), and `resize()` restates once per overlay |
| P2-4 | `SnapshotSource.cpp:52` | a null snapshot allocates a fresh empty list per call, exactly the case `detail::noCommands()` exists to make free |
| P2-5 | `ScenePublisher.cpp:92` + `DrawListMemo.cpp:62` | the second `find` before `store` is dead code, and `store`'s "or replaces" contract is false - it only `push_back`s |
| P2-6 | `FrameTable.cpp:35-40` | `clearText` emits malformed text: the format already closes the paren and the return appends another, giving `linear(...))` |
| P2-7 | `DrawListBake.cpp:84` (also `foldUp`, `bakeSubtree`) | the fold recurses on host-controlled tree depth; `FrameScheduler` went iterative for the same reason and `selectRows` already uses an explicit stack |
| P2-8 | `DrawListBake.cpp:325` | a world-answer leaf can never take the incremental hot path: `fillOwnFacts` resets `local_box`, so the `non_hot` guard always sees a difference and forces a full rebuild |
| P2-9 | `Plan.cpp:83-90` | `InvalidTarget` / `InvalidOutput` exist in the code but are absent from the design page's `PlanProblemCode` list |
| P2-10 | `DeviceClock.hpp:51` | cites `D-01`, which is defined nowhere in the tree (the repo's rejected-option IDs are `D1`-`D7`) |
| P2-11 | `StateResolution.hpp` (`PassRenderState`) | points at "the table in `graphics-scene.md` §4.1"; §4.1 is the Node/Group interface table and no design page carries the promised state table |
| P2-12 | `FrameContext.hpp:22` + `RenderEngine.hpp` | call the interval "WALL-CLOCK"; the clock is `std::chrono::steady_clock` (`RenderEngine.cpp:367`), which is specifically not wall-clock time |
| P2-13 | `Residency.cpp:9-13`, `ResidencyTable.cpp:10` | `noexcept` on operations that allocate (`reserve`, `try_emplace`); `bad_alloc` becomes `std::terminate` |
| P2-14 | `Residency.hpp:36-38` | `kResidencyKindCount` duplicates `ResidencyKind::Count` - one fact, two spellings; `nameOf(ResidencyKind::Count)` answers `"count"` |
| P2-15 | `VsgBackend.cpp:2172-2174` | the comment says "the stream store does not count bytes", but `:2181` fills `.bytes = uploads.residentBytes()` - the comment predates `a56b3431` |
| P2-16 | `AxisGizmo.hpp:26`, `FpsOverlay.hpp:29`, `:31`, `FpsOverlay.cpp:233`, `docs/guides/graphics.md:577`, `:587` | seven stale `update()` spellings left by the `BC-25` second half; `FpsOverlay.cpp:233` cites `AxisGizmo::update`, a method that no longer exists |

## 3. Why the Suite Is Green: the Oracle Gap

`tests/test_graphics/RenderWorldTest.cpp:276`, `SnapshotTest.AnEditedTransformRepublishesAndMatchesTheScene`,
is the only case that edits a transform and then republishes. Two properties make it unable to catch
either P0:

- **The oracle is the code under test.** It asserts
  `selectSnapshot(*after, view, selected, ...)` against `scene.collectDrawItems(view)`, and
  `graphics-threading.md` §4.3 states that `Scene::collectDrawItems` shares the one
  `detail::selectSnapshot` rule with the frame's source. Both sides read the same snapshot, so a
  snapshot that is internally wrong answers the same wrong list on both sides.
- **The assembly is too small to reach the chunk rule.** `makeAssembly()` (`RenderWorldTest.cpp:137`)
  is documented as "a root with one two-deep chain and a second leaf beside it", i.e. a single chunk,
  and the file contains no `StateNode` at all (its only `setMaterial` is at `:184`, a leaf material in
  another case). P0-1 needs a subtree wider than 32 rows; P0-2 needs a state-bearing ancestor above
  the edited transform.

**The shape a regression test needs**: more than `kRowsPerChunk` rows in the edited subtree, an
inherited material or `StateNode` above the edited transform, and an expectation computed
independently of the snapshot - the world matrices and boxes the edit implies - compared item by item.

## 4. Threading Model Re-verification

`graphics-threading.md` was read section by section against the code. It **matches**:

- `PublicationStore` holds its mutex only for a lookup, an insert or an eviction, never across a
  collection; `publicationFor` reads the scene's own state without the lock because it runs on the
  content thread; the `beginFrame` eviction window `last_used_frame + 1 < frame` is exactly wide
  enough for one frame in flight.
- One frame's two halves stay on their own threads: `buildContentHalf` describes and binds,
  `runEngineHalf` expands, executes and presents, and `publishEngineMicros` runs only when the pump is
  not threaded.
- The present outcome and the frame number travel as one packed atomic word; the frame's whole answer
  is published under the pump's lock.

**One correction to the record**: `T31` is **closed**. `Buffer::revision_` and `Texture::revision_`
are `std::atomic<std::uint64_t>` (`src/base/core/sdk/vine/Buffer.hpp:379`,
`sdk/vine/graphics/Texture.hpp:208`), with acquire on the read and release on the write, which is what
`graphics-threading.md` §2.4 claims. The earlier audit-3 note that called the read "non-atomic" is
obsolete, and the backlog no longer carries `T31`.

**One gap in the document**: §2.4 says the only real UB is the UI changing a non-atomic field the
engine thread reads. P1-1 is exactly that shape and is not named there - the diagnostics counters are
the remaining cross-thread plain integers. The section should say so.

## 5. Prioritised Plan

### 5.1 Order

1. **P0-1 and P0-2 in one batch.** They mask each other, so fixing only one can make the other more
   visible without making it fixed. The same batch carries the regression test of §3.
2. **P1-1 and P1-2** - a data race and a use-after-free, both small, both in the engine's lifecycle.
3. **P1-3 and P1-4** - teardown completeness and the exception barrier.
4. **P1-5 and P1-6** - documentation truth and the vsg byte semantics.
5. **P2-6, P2-15, P2-16** as one sweep (a malformed string, a comment that contradicts the next line,
   and the seven stale `update()` spellings); the rest of §2.4 follows as its own batches.

### 5.2 Backlog candidates

This round added no backlog entry. The candidates, each with the trigger it would carry:

| Candidate | Trigger |
|---|---|
| P0-1 / P0-2 | none - a correctness defect, so §1 "Actionable Now" if the backlog is used at all |
| P1-1 .. P1-4 | none - same |
| P1-5 | none - documentation truth |
| P1-6 | none - an API field that does not mean what its contract says |
| P2-7 (fold recursion depth) | a scene whose depth is host-controlled and measured to overflow, or a host that builds degenerate chains |
| P2-9, P2-10, P2-11, P2-12, P2-14, P2-15 | none - documentation and naming truth, batchable with the next touch of each page |
| P2-13 (`noexcept` on allocating operations) | a measured `bad_alloc` path, or the next exception-safety batch |

## 6. Census and Evidence

| Fact | Value | Command |
|---|---|---|
| `test_graphics` | 620 cases | `./build/bin/test_graphics --gtest_list_tests \| grep -c '^  '` |
| `test_vk` | 174 cases | same |
| `test_vsg` | 572 cases | same |
| `BackendParityTest` | 13 cases | `./test_vsg --gtest_list_tests` |
| Diagnostic codes | 124 registered / 124 in use / 0 uncoded | `check_diagnostic_formats.py` |
| `check_api_style` baseline | 1 (`ChainBuilder`) | `check_api_style.py --list` |
| Gates | 10 green | §7 |
| Sub-review coverage | every file in §1.2's two scopes read in full | - |

## 7. Verification Recipes

```bash
cd /opt/opensrc/Vine
cmake --build build --target test_graphics test_vk test_vsg gfx_backend_vk gfx_backend_vsg
ctest --test-dir build -R 'test_graphics|test_vk|test_vsg' --output-on-failure

python3 scripts/check_ai_docs.py --strict
python3 scripts/check_design_docs.py
python3 scripts/check_doc_symbols.py
python3 scripts/check_include_hygiene.py
python3 scripts/check_api_style.py
python3 scripts/check_diagnostic_formats.py
python3 scripts/check_public_docs.py
python3 scripts/check_export_annotations.py
python3 scripts/check_vk_exposure.py
python3 scripts/check_cross_refs.py
```

Measured at the end of the round: `test_graphics` 620, `test_vk` 174, `test_vsg` 572,
`BackendParityTest` 13; ten gates green; no code changed, so no demo run was owed.

## 8. Remaining Debt and Open Questions

1. **The two P0s are the round's real output.** Everything else is either a small correctness fix or
   documentation truth. The reason they matter is not their size but that the edit path is the one
   part of the frame pipeline a host exercises every frame it animates.
2. **The oracle gap is a design gap, not a test-count gap.** `T34` ("the published source equals a
   direct collection item by item") is the right criterion and it is implemented - but its
   "direct collection" is the same snapshot, so it cannot see a snapshot that is wrong in a way both
   readers share. The §3 test is the missing half.
3. **`graphics-threading.md` §2.4 needs one sentence** naming the diagnostics counters as the
   remaining cross-thread plain integers (P1-1).
4. **The sub-review's P2 list is unverified line-by-line.** It is recorded here as measured by the
   parallel pass; a batch that touches one of those files should confirm the line before fixing it.

## 9. References

### 9.1 Code

- `src/viz/graphics/src/SceneSnapshot.cpp`, `SceneSnapshot.hpp`, `DrawListBake.cpp`, `DrawListBake.hpp`
- `src/viz/graphics/src/ScenePublisher.cpp`, `RenderEngine.cpp`, `FrameRunner.cpp`,
  `FrameResources.cpp`, `EngineCompile.cpp`, `PassChain.cpp`, `SnapshotSource.cpp`,
  `DrawListMemo.cpp`, `FrameTable.cpp`, `Plan.cpp`
- `src/viz/graphics/sdk/vine/graphics/Diagnostics.hpp`, `Node.hpp`, `Texture.hpp`, `DeviceClock.hpp`,
  `FrameContext.hpp`, `StateResolution.hpp`, `Residency.hpp`
- `src/base/core/sdk/vine/Buffer.hpp`
- `src/plugins/gfx_backend_vsg/src/internal/BlockStorage.cpp`, `MaterialArena.cpp`,
  `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`
- `tests/test_graphics/RenderWorldTest.cpp`

### 9.2 Documents and ADRs

- `.ai/design/graphics-threading.md` §2.4, §2.12, §2.15, §4.3, §6.3
- `.ai/design/graphics-frame-pipeline.md`, `.ai/design/graphics-scene.md` §2.12,
  `.ai/design/graphics-architecture.md` §2.15
- `.ai/decisions/ADR-2026-11-18-the-frame-carries-its-context.md`,
  `.ai/decisions/ADR-2026-11-18-names-state-what-is-handed-over.md`
- `.ai/backlog/graphics.md`
- `.ai/research/graphics-2026-11-18-review-5.md`, `.ai/research/graphics-2026-11-18-audit-4.md`

### 9.3 External Sources

None - this round's evidence is the tree, the tests and the gates.
