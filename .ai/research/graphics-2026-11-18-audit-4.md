# graphics audit 4: the third round's open items, re-established

> **Status**: draft, non-authoritative - a working plan; each item becomes a backlog entry, a design
> correction or an ADR when it is decided.
> **Last verified**: 2026-11-18
> **Scope**: the graphics module and the two render backends, limited to (a) the items the
> third-round audit left open and (b) what today's re-check found. It does not re-audit the
> dimensions the third round concluded clean (contract honesty of the headers, layering, test
> shape, macros, concept structure).
> **Sources**: `git show 9fb3246f:.ai/research/graphics-2026-11-17-audit-3.md` (the deleted page),
> the tree at `04018284`, `scripts/*`, `tests/*`, and the commands in §8.
> **Backlog**: `../backlog/graphics.md` (T35, T39).

## 1. Overview

### 1.1 Why this page exists

The third-round audit (`graphics-2026-11-17-audit-3.md`) was deleted with the research corpus <!-- drift-ok -->
(`19aeca13`) after two of its fourteen batches landed. Its other twelve batches were left with no
home in the tree: only git history and the session notes remember them, so a reader of
`.ai/backlog/graphics.md` cannot see that `B3`-`B14` are still owed.

This page restores them. It is **not** a fresh fourteen-dimension audit: it carries the open items
verbatim by ID, re-establishes each one's status against today's tree, and records the drift the
re-check found (the documentation-language migration of 2026-11-18 touched the pages those items
live on).

### 1.2 What "open" means here

An item is **closed** only when the tree shows the end state: the code changed, or the doc says the
true thing. An item is **moot** when what it referred to no longer exists. Everything else stays
open with the evidence that proves it.

### 1.3 Conclusion

Two items landed (`B1`, `B2`), one is moot (the registration task), and eleven were owed. `B3`-`B13` (2026-11-18) have
since landed the guards, the three doc truths, the readback answer shape, the export demotions, the `core` directive
decision, the explicit host backend choice, the validation-clean frame, the allocation measurement, the concept
chapter, the three plug-in-internal renames and the seam-identity decision; **no carried item remains** (`T-12`
excluded by default, `T-16` register-only). The single most useful thing this page adds is that the two places a reader looks for "what is left" -
`backlog/graphics.md` T35 and `vk-backend.md` §9.2 - both still listed gaps that were closed after
they were written, and the user guide still printed two member names that do not exist.

## 2. Design Points

### 2.1 A carried item keeps its ID

- What it is: `A3-xx` (finding) and `T-xx` (task) numbers from the third round are kept; batches
  keep the labels `B3`-`B14`.
- Why: `B1`-`B14` in this page and `B1`-`B17` in the redesign page are different numbering schemes.
  They never appear in the same sentence; a citation says which page it comes from.
- Boundary: batch numbers are not design IDs and are not stable; findings and tasks are.

### 2.2 A stale gap list is a defect, not untidiness

- What it is: T35 and `vk-backend.md` §9.2 each state what is missing before the default-backend
  switch; both list items that have since been delivered.
- Why: the switch gate is the one list a reviewer consults before changing the default. A gate that
  overstates its gaps hides that the real remaining work is platform evidence and performance
  (`G11`).
- Boundary: the fix is to restate the gap, not to weaken the gate.

## 3. Census re-established today

| Fact | Today | Command |
|---|---|---|
| `test_vk` | 174 cases / 42 suites | `./test_vk --gtest_list_tests` |
| `test_vsg` | 572 cases | `./test_vsg --gtest_list_tests` |
| `test_gui` | 332 cases; `RenderControlTest` 26, `BackendChoiceTest` 5 (B7) | `./test_gui --gtest_list_tests` |
| `test_graphics` | 618 cases | `./test_graphics --gtest_list_tests` |
| `BackendParityTest` | 13 cases | `./test_vsg --gtest_list_tests` |
| Diagnostic codes | 124 (`appfw` 1 / `engine` 34 / `vk` 65 / `vsg` 24) | `grep -cE '^[a-z0-9_]+\.[a-z0-9_.]+ ' scripts/diagnostic_codes.txt` |
| Diagnostic baseline | comments only (empty) | `cat scripts/diagnostic_code_baseline.txt` |
| `check_api_style` baseline | 1 (`ChainBuilder`) - was 5 before B4 | `python3 scripts/check_api_style.py --list` |
| Documentation gates | all green (195 notes / 56 design / 2 docs 138 units / 974 entries) | §8 |
| Demo self-check | `session up` 42 ms, `Pending -> Presenting` 190 ms, **0 `[error]`, 0 `[warning]`** (vk); `VN_RENDER_BACKEND=vsg` - 68 ms / 276 ms, 0 `[error]` | `timeout 40 ./Vine --open demo` |
| `.ai` counts | design 56, ADR 77, backlog 13, memory 12, bugs 36 | `ls` |

The three demo warnings are the two known `fragment writes Location 0 ... no pColorAttachments[0]`
and the `Vertex attribute at location 1 not consumed by vertex shader` the third round recorded;
`B8` owns them.

## 4. Carried items and their status today

| ID | What | Status today | Evidence |
|---|---|---|---|
| T-00 | register the third-round page in `.ai/README.md` | **moot** | the page was deleted in `19aeca13`; this page takes its place |
| T-01 / B1 | `TargetObjects::fresh` as an atomic | **landed** | `b349af5c`; the `peek()` value snapshot answers the no-wait refusals |
| T-02, T-03 / B2 | documentation truth sweep (A3-02/04/05/07/08/09/10/24) | **landed, with two residues** | `5b36c6db`; residues are A4-01 and A4-02 |
| T-04 / B3 | gate the seam's file set; put `graphics.md` under the symbol gate | **landed 2026-11-18** | `check_include_hygiene.py` now pins `SEAM_HEADERS` (reachable from both backends) and the directly shared `SEAM_SHARED_HEADERS` set, with an anchor for each root; `check_doc_symbols.py` guards both living guides and keeps the unit-map coverage on the backend guide alone. Both directions mutation-tested |
| T-05 / B4 | one readback answer shape; converge the two SDK refusal enums | **landed 2026-11-18** | the verbs return `ReadbackOutcome` (`Readback.hpp` now owns `ReadbackResult` + `ReadbackOutcome` + `readbackResultOf`); the old `bool` + `ReadbackResult*` spellings are `[[deprecated]]` shims (BC-16); `check_api_style` baseline is 1 (the `ChainBuilder` line) |
| T-06 / B5 | demote four "internal-smelling" root exports | **landed 2026-11-18, with one kept public on evidence** | `keyAuditTable` (with `KeyAuditEntry`) → `detail` (a test witness, `G8`); `refusalName` → `detail` (seam message/test spelling, `G1`); `planCodeText` → `detail` (the engine composes the sentence, `G2`); `renderApiToString` **stayed public** because `src/plugins/app_shell` prints it in the backend list - hiding it would send a host into `detail` (`G4`), so it gained the "public on purpose" note instead. `ContentPushConstants` → `PushRangeConstants` (the SDK's `PushRange` word; `PushConstants` is taken by `::vsg::PushConstants` in the same TUs) |
| T-07 / B6 | decide the `core` using-directive | **decided 2026-11-18: kept, measured and documented** | the directive's surface was census'd by removing it and building `gfx_backend_vsg`: **31 names** (all SDK vocabulary `core` builds on). A using-declaration list cannot live in that header (a using-declaration needs its target at PARSE time, and the global header deliberately pulls no graphics header; closing the set costs 5 headers there or 36 include lines). The recorded correction: a using-directive widens *unqualified and qualified* lookup, **not ADL** ([basic.lookup.argdep]) - the audit's ADL clause was wrong. Revisit when `core` gains a colliding name or the unqualified SDK surface grows |
| T-08 / B7 | make the host's default backend explicit | **landed 2026-11-18** | `useDefaultBackend()` no longer takes `entries().front()`: it asks `chooseBackend` (`src/fw/appfw/src/gui/BackendChoice.hpp`), which answers the application's name, else the only registered backend, else a refusal naming the candidates. The application states it once (`RenderControl::setDefaultBackendName`); the app shell states `vk` and honours `VN_RENDER_BACKEND`, so the same build now runs the demo on `vsg` (0 errors, session up 68 ms - impossible before this batch). Tests: `BackendChoiceTest` (5 cases, no registry and no window) and `AStatedBackendThatIsNotRegisteredFailsTheAttachAndNamesIt` |
| T-09 / B8 | validation clean | **landed 2026-11-18** | the shadow pass names `BuiltinShaders::depthOnlyProgram()` (the content vertex stage + a fragment stage that declares no output), so the two `fragment writes Location 0 ... no pColorAttachments[0]` warnings are gone and the pass does no per-fragment material/lights/shadow work; vk's content pipeline now declares only the vertex-input locations its vertex stage reads (read back from the SPIR-V interface, one location per matrix column), which removes `attribute at location 1 not consumed` for the sky program. Demo: **0 `[error]`, 0 `[warning]`**, session up 42 ms / `Pending -> Presenting` 190 ms (was 86 / 300). Evidence: `VkVertexInputTest` (5 device-free cases), `scripts/vn_shader_check.sh` PASS over 10 stages |
| T-10 / B9 | measure the frame's allocation count; re-check the performance sentence | **landed 2026-11-18** | counted once, in two scopes that are no longer mixed: vk's `executeFrame` window asks **85 allocations / 10.6 KB** for one drawing call and **133 / 12.4 KB** for four (≈16 per further call, 69 fixed, identical on every steady frame), while the **whole chain frame** (engine + backend, the demo's deferred path, 10 passes / 10 draws) is ≈**1966 allocations / ≈2.6 MB** (`VN_BENCH_FRAMES` 8 vs 120). The folklore "97 per frame" was the backend window mislabelled as the frame; `vk-backend.md` 9.1 and `graphics-threading.md` 1.1 now state both with their methods, and 9.1 says they are not interchangeable |
| T-11 / B11 | concept tables (translation, false friends, five frame values, target roles, "frame compiler, not a render graph") | **landed 2026-11-18** | `docs/guides/graphics.md` §5 (5.1 translation ↔ Filament / O3DE / UE, 5.2 false friends, 5.3 the five `Frame*` values, 5.4 the six target roles, 5.5 the frame-compiler boundary, 5.6 `SceneView` as the per-window controller); the normative halves are `graphics-frame-pipeline.md` 2.11 (five values, one question each) and 2.12 (no render graph, `G8`), `graphics-architecture.md` 2.13 (`SceneView`), and `vk-backend.md` 2.6 now says `TargetDecision` where it said `TargetPlan` (C-A3-05's wording unification). Glossary §3 points at the chapter instead of copying it |
| T-12 / B12 | public vocabulary unification (the third round's five words) | **not doing by default; register corrected 2026-11-18** | Against today's tree **two of the five are moot**: no `releaseResidency` (the verb is `releaseContent`), no `SessionId` (`DeviceClock::FrameDriverId` merged the three widths). The three live ones, with today's cost - `createSession()` / `releaseSession()` → `createFrameDriver()` / `releaseFrameDriver()` (65 references: 3 SDK headers, 34 src, 28 tests, 23 doc lines; the last "session" spelling after S2 renamed the noun); `MaterialBase` → `MaterialValues` (129 references: 96 src / 16 tests / 17 doc lines; an interface named like a base class); `EngineNotReady` → `EngineNotReadyReason` (33 references: 15 src / 7 tests / 11 doc lines; an enum whose own field is named `reason`). Deferred and separate - `RenderCommand` / `DrawDescription` → `DrawItem` / `DrawCall` (≈1000 references: 137 src / 649 tests / 50 doc lines and 76 / 144 / 12): the industry order is item-then-call, but the pair is only reversed in naming order, not in meaning, so it needs a quiet tree and its own batch. Shape when any of them happens: one `[[deprecated]]` alias per word for one release (BC-xx, the shape B4 used for `ReadbackOutcome`), one migration note per word, and the API-style baseline decreasing |
| T-13 / B13 | plug-in-internal renames (`ProgramHalves` / `ResourceTable` / `ResidenceTable`) | **landed 2026-11-18** | now `CompiledPrograms` (vsg: one pipeline layer + recorder per key) / `PinnedResources` (vsg: the pin-driven template) / `ResidencyClaims` (vk: who may build a key); eight files moved with `git mv`, and the seam guard's list (`scripts/check_vsg_content_seam.cmake`) plus both test CMake lists follow, so a rename cannot silence it. Every gtest case name is unchanged (`test_vsg` 572 / `test_vk` 174 / `BackendParityTest` 13). Glossary §1/§2 carry the new and the retired spelling; the three ADRs that named the old classes gained a dated correction. The members (`halvesFor` / `halves()`) and the prose word *half* stayed - renaming them item by item is churn and the case names quote them (`G8`); the class docs now define one *half* as one compiled program |
| T-14 / B14 | seam identity (an alias group, or the file-set gate alone) | **decided 2026-11-18: no alias group** | `ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md`: the seam is the measured property "both backends reach it", recorded once in `SEAM_HEADERS`; the alias group is rejected (two spellings of one class, and it would cover 8 of the 25 headers both backends include - a name that understates the seam). The missing half was mechanical, not structural: `check_include_hygiene.py` now also requires the two pages that state the seam (`graphics-architecture.md` 2.9, `docs/guides/graphics-backends.md` 5) to name every mechanism the list holds - mutation-tested by un-backticking one name (red) and restoring it (green) |
| T-15 | vsg's extent/size refusal precision, or a stated intentional difference | **landed 2026-11-18 (stated)** | `docs/guides/graphics-backends.md` §3 now has the row "目标拒绝的精度", naming both vk codes and vsg's narrower `build_failed` / resize-time refusal |
| T-16 | register-only items (`OwnedFrame`, `GraphicsTest.cpp` split, macros, `const_cast`, `Frame*` density) | **carried as §7** | `grep -rn OwnedFrame src` outside its own header: nothing |

Also still true, and deliberately unchanged: the 8 `const_cast` ownership spellings
(`Scene.cpp:46`, `RenderDevice.cpp:13`, `DrawListBake.cpp:247`, `RayIntersection.cpp:162`,
`VsgBackend.cpp:102`, `FrameRecorder.cpp:239`, `VsgBufferView.hpp:85/95`), `ImageLayout` reachable
from `DrawState.hpp`, `ContentPushConstants` in vk's `ContentAbi.hpp:94`, and `EngineNotReady` as
the name of a reason enum (15 references today, was 22).

## 5. New findings from this round

| ID | Class / `G#` | Evidence | Effect | Severity | Batch |
|---|---|---|---|---|---|
| A4-01 | documentation / `G4` | `docs/guides/graphics.md:87` printed `resolveRenderState(cmd.authoredState, ...)` and `:1076` printed `RenderCommand::drawOrder`; the members are `authored_state` (`RenderCommand.hpp:182`) and `draw_order` (`:169`), and the accessor `drawOrder()` belongs to `Node` (`Node.hpp:175`) | **fixed 2026-11-18**: both spellings now match the header | low | B3 |
| A4-02 | documentation / `G11` | `../backlog/graphics.md` T35 said "`test_vk` has 72 cases", listed "the vk page in `docs/guides/`" and "the vk page in `.ai/design/`" as missing, and quoted the 2026-11-14 ASan/TSan 66/66 counts | **fixed 2026-11-18**: T35 now states 169 cases / 41 suites, the 2026-11-18 quality evidence pointer, the 8.24 ms reading, and the real remaining gaps (Win32, Wayland, the performance reading) | medium | B3 |
| A4-03 | documentation / `G11` | `.ai/design/vk-backend.md` §9.2 listed "`releasePass`' explicit no-op with a reason" and "a vk user page under `docs/guides/`" as missing | **fixed 2026-11-18**: §9.2 now names the documented default no-op and the existing page, and states the same remaining gaps as T35 | medium | B3 |
| A4-04 | measurement / `G11` | the user guide's backticked identifier census today: 329 identifiers, 1 apparent miss (`Raycaster`, which is three.js's name at `:1097`) - so the drift is exactly A4-01, not a wider gap | gives B3's symbol-gate baseline: the page is one edit away from being guardable | low | B3 |
| A4-05 | capability/documentation / `G4` | `docs/guides/graphics-backends.md` §1 said "按名创建可显式选 `vsg`", but no host named one: the attach took `entries().front()` and neither a flag nor a variable existed, so the demo could not run on vsg from a normal build at all | **fixed 2026-11-18 (B7)**: the application names its backend (`RenderControl::setDefaultBackendName`), the app shell states `vk` and honours `VN_RENDER_BACKEND`; the guide now says which rule decides | low | B7 |
| A4-06 | plan protocol / `G2` | after B8 the demo still logged `applyPlan was refused` (a frame being recorded) and, at shutdown, four `plan refused [unknown-input]` for the G-buffer tiles. Two causes: the demo stated rows (and built its chain) without waiting for the frame, and `SceneView::applySurfaceGeometry` handed the surface size to the chain - which restates the passes it re-lays out - without waiting either, so ANY host's resize could be refused mid-frame. The tiles were also re-stated after the chain that publishes `GBuffer` was gone (the view runs its layout steps when it drops a chain) | **fixed 2026-11-18 (B8b)**: `applySurfaceGeometry` waits for the frame first (bounded 250 ms), `addDemoPass`/`PassChain::build` in the demo do the same, and the tile step stops when the view has no chain. 18 demo runs (including kills at 2 s and 5 s) with 0 `[warning]`; pinned by `RenderEngineTest.AViewResizeWaitsForTheFrameBeforeItRestatesItsChain` (mutation-checked) | medium | B8 |

The third round's "22 of 520 identifiers missing" figure is therefore superseded: the four API
names were fixed, and two spellings remain.

## 6. Batch plan (third-round labels kept)

Order and content are the third round's, with the re-check folded in. Every batch is one
revertible commit; renames and moves do not share a commit with a public API change.

| Batch | Content | Risk | Acceptance |
|---|---|---|---|
| **B3** (landed 2026-11-18) | the two guards (seam file set + `graphics.md` under `check_doc_symbols`) and the three doc truths of A4-01/02/03 | medium (adds red) | mutating the seam set reddens the gate and reverting greens it; `graphics.md` baseline lists only third-party and example names; `ctest -R 'content_seam\|vk_exposure'` |
| **B4** (landed 2026-11-18) | readback answer shape (`ReadbackOutcome`) and the refusal-enum convergence; the old bool shape survives as `[[deprecated]]` shims for one release | medium (public API) | three suites green; `check_api_style` baseline 5 → 1; the migration note is BC-16 in `graphics-rendering.md` §2.9 / F5. The in-tree tests still call the shims on purpose, so a build prints the deprecation warnings that mark the migration; the batch that deletes the shims updates them |
| **B5** (landed 2026-11-18) | demote the three exports whose only consumers are a test witness (`keyAuditTable`), the seam's own spelling (`refusalName`) or the engine's sentence (`planCodeText`); keep `renderApiToString` public and say why (the app shell's backend list is a host consumer); rename `ContentPushConstants` → `PushRangeConstants` | medium (source ABI) | export/docs/api gates green; three suites green (`test_graphics` 617 once, after the load-sensitive measurement case was re-run alone - it measures collection time and had flaked under the parallel run, not a regression); `check_include_hygiene` 0/1347 |
| **B6** (decided 2026-11-18) | keep the `core` using-directive, record its measured 31-name surface and its real effect (unqualified + qualified lookup, not ADL), with the trigger to revisit | medium | `test_vsg` 572 green; `content_seam` green; the plugin builds with 0 errors; the note at the directive is where a future reader checks the claim |
| **B7** (landed 2026-11-18) | the host names its backend: `chooseBackend` owns the rule, `RenderControl::setDefaultBackendName` is the application's one statement, the app shell states `vk` and honours `VN_RENDER_BACKEND`; `entries().front()` is gone | medium (host behaviour) | demo 0 `[error]` on `vk` **and** on `vsg` from the same build; a stated-but-unregistered name fails the attach and the reason names it; `BackendChoiceTest` proves several-registered-without-a-name is a refusal, not the alphabetically first |
| **B8** (landed 2026-11-18) | validation clean: a colour-less fragment stage for the depth-only shadow pass (`depthOnlyProgram`), and a content pipeline that declares only the vertex-input locations its vertex stage reads | low-medium | demo `[warning]` = **0** (`[error]` 0); `test_vk` 174 green, `test_vsg` 572 green, `test_graphics` green; `vn_shader_check.sh` PASS |
| **B9** (landed 2026-11-18) | count a frame's host allocations once, in the two scopes a reader needs (the backend's `executeFrame` window and the whole chain frame), and write both down instead of the folklore figure | low | vk `executeFrame`: 85 (1 draw) / 133 (4 draws) allocations per steady frame; whole chain frame ≈1966 allocations / ≈2.6 MB (`VN_BENCH_FRAMES` 8 vs 120); recorded in `vk-backend.md` 9.1 and `graphics-threading.md` 1.1 with method and date |
| **B11** (landed 2026-11-18) | the concept tables: a translation table against Filament / O3DE / UE, the false friends, the five `Frame*` values, the six target roles, "the engine is a frame compiler, not a render graph", and `SceneView` as the per-window controller | low | `grep -rn -i "frame compiler\|target role"` returns `docs/guides/graphics.md` §5; doc gates green (194 notes / 56 design / 138 units / 974 public entries) |
| **B13** (landed 2026-11-18) | the three plug-in-internal renames (`ProgramHalves`→`CompiledPrograms`, `ResourceTable`→`PinnedResources`, `ResidenceTable`→`ResidencyClaims`); the seam guard's guarded list and both test CMake lists follow | low-medium | case names unchanged (`test_vsg` 572, `test_vk` 174, `BackendParityTest` 13); `content_seam` green **after** the guard was updated - it first reported the old header as gone, which is the guard working; demo on vk and on vsg 0 `[error]` / 0 `[warning]` |
| **B14** (decided 2026-11-18) | the seam's identity: no `vn::graphics::backend` alias group; the identity is the measured property plus one recorded list, now also checked against the two pages that state it | medium | ADR-2026-11-18 + design 2.9 + guide §5; `check_include_hygiene.py` green (0 findings / 1350 files) and red when a page drops one mechanism name; no code moved, no ABI change |
| **B12** | public vocabulary - **only on an explicit go** | high | one migration note per word; baselines decrease |

`T-15` rides on B3: either vk's `vk.target.size_unknown` / `borrow_extent_mismatch` gain vsg
counterparts, or the user page's difference table states that vsg refuses at resize time.

## 7. Not doing (carried unchanged)

| Item | Why not |
|---|---|
| moving all 58 root functions into `detail::` | they have real consumers; no name collision evidence; churn (`G8`) |
| renaming `EngineNotReady` immediately | 15 references and a public API; a separate public-API batch |
| replacing `VN_ENABLE_ENUM_FLAGS` / `VN_DISABLE_COPY` with templates or bases | changes overload and inheritance semantics for every enum/class (`G8`) |
| merging vk's `ContentCaches` with vsg's `ContentStore` + `ContentFacts` | the two backends divide the work differently on purpose (architecture §2.12) |
| restoring `SEAM_HEADERS` as a path set | the ADR deleted that notion; the correct thing is a property check (B3) |
| splitting `tests/test_graphics/GraphicsTest.cpp` | maintenance only; wait for the next large edit (`G8`) |
| multi-frame-in-flight / dynamic rendering / VMA / bindless / LOD / point-light shadows | each has an ADR or a backlog trigger that is not hit |
| clearing `ImageLayout` out of the seam now | the design lists it as the next C6 step; no second API family |
| a new allocator for the "97 allocations" | measured in B9 instead: 85 per backend recording frame, ≈1966 per chain frame, all short-lived vectors in a frame that still measures 8.2-8.6 ms; no allocator is justified (`G8`, `G10`) |

## 8. Verification recipes used today

```bash
cd build/bin
./test_vk --gtest_list_tests | grep -c '^  '          # 169
./test_vsg --gtest_list_tests | grep -c '^  '         # 572
./test_graphics --gtest_list_tests | grep -c '^  '    # 617
timeout 40 ./Vine --open demo > /tmp/demo4.log 2>&1    # 0 [error], 3 [warning]

cd /opt/opensrc/Vine
grep -cE '^[a-z0-9_]+\.[a-z0-9_.]+ ' scripts/diagnostic_codes.txt   # 124
python3 scripts/check_api_style.py --list                            # 5 baseline
grep -n 'SEAM' scripts/check_include_hygiene.py                      # no output
sed -n '42,44p' scripts/check_doc_symbols.py                         # one guarded page
python3 scripts/check_ai_docs.py --strict
python3 scripts/check_design_docs.py
python3 scripts/check_doc_symbols.py
python3 scripts/check_public_docs.py
```

The third round's TSan readings (`0 / 0 / 1`, then `0` after B1) were not re-run this round; the
last recorded run is in `../memory/graphics.md` and the backend notes.

## 9. References

- `git show 9fb3246f:.ai/research/graphics-2026-11-17-audit-3.md` - the deleted third-round audit
  (findings A3-01…A3-24, batches B1-B14, tasks T-00…T-16).
- `git show 19aeca13 --stat` - the commit that deleted the research corpus.
- `../backlog/graphics.md` - T35 (the switch gate) and T39 (the seam file-set gate).
- `../design/vk-backend.md` §9.2 - the backend's own gap list (A4-03).
- `docs/guides/graphics-backends.md`, `docs/guides/graphics.md` - the user pages whose truth
  A4-01/02 measure.
