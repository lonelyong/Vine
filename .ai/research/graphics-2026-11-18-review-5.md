# graphics review 5: the first contact, and the references that point at nothing

> **Status**: draft, non-authoritative - a working record; each item becomes a design correction, a
> backlog entry or an ADR when it is decided.
> **Last verified**: 2026-11-18
> **Scope**: a fifth architectural pass over the graphics module and the two backends, deliberately
> NOT re-walking the dimensions the first four rounds settled (unit contracts, layering, seam
> identity, allocation measurement, validation cleanliness, naming of the public vocabulary). This
> round asks a different question: **what does a host meet first, and does that experience survive a
> refactor?**
> **Sources**: the tree at `226f83d5`, the four earlier audit pages in git, `scripts/*`, and the
> commands in §8.
> **Backlog**: `../backlog/graphics.md` (T40, T41 added).

## 1. Overall Assessment

The architecture is in good shape and, more importantly, it is in good shape **for the reasons the
design says it is**. The composition root claim is now measurable rather than rhetorical:
`RenderEngine` declares **26** public members (`grep` over the header) and delegates the three
mechanisms it used to carry (`detail::FramePump`, `detail::SessionShape`, `detail::SurfaceLedger`).
The seam is a measured property with a checked list, not a directory. The frame is a described value
that a backend materialises, and the two backends agree byte for byte on the same frame description.

What this round found is not a structural flaw. It is a **maintenance asymmetry**: the parts of the
system that are code are guarded by tests and nine gates, and the part that teaches the system — the
first example a host copies — was guarded by nothing. It had rotted through three public-API changes
and no longer compiled. That is the finding this round is built on, and the fix is mechanical: make
the example a translation unit, then make a script compare the two copies.

### 1.1 Where the framework is genuinely ahead of its peers

| Quality | Evidence |
|---|---|
| A frame is a value, not a side effect | `FrameDescription` is compiled from declarations each frame; the backend records it and reports what it cannot serve. No per-object hidden state. |
| One fact, one home | Size lives in the area, presentation in the area's table, pass registration in one registry, diagnostics in one route. Each is asserted by a design point, not by convention. |
| Failure is a value | `PlanResult` (Applied/Invalid/Refused), `FrameOutcome` (Ran/Submitted + a reason), `ReadbackOutcome`, `SurfaceTargetResult`. A host can act on every outcome without parsing a sentence. |
| Two backends, one contract | `BackendParityTest` (13 cases) plus `content_seam` / `vk_exposure`, so "same picture" is a test, not a claim. |
| Documentation is gated | Four gates read the docs as inputs (`check_ai_docs`, `check_design_docs`, `check_doc_symbols`, `check_public_docs`), which is why the prose is unusually honest. |

### 1.2 What was weak

| # | Class / `G#` | Finding |
|---|---|---|
| R5-01 | documentation / `G4`, `G11` | The module guide's first example — the one every new host copies — **did not compile**. Three errors, each a real API change since it was written: `make_intrusive` named unqualified (it lives in `vn`, not in `vn::graphics`), a `RenderArea` constructed directly (the constructor is private; `RenderEngine::createArea` is the only way), and `engine->frame(dt)` (the frame verb takes no time). Nothing compiled the snippet, and no gate read it. |
| R5-02 | documentation / `G11` | Four section references led nowhere - in `async-design.md`, `base-crash.md`, `graphics-threading.md` and `graphics-architecture.md`, at sections 4.4, 2.10, 3.1.6 and 8.2 respectively (the numbers are printed apart from the file names so this row cannot be followed as a live pointer). A `§` pointer has no owner and no compiler. |
| R5-03 | documentation / `G6` | `graphics-architecture.md` listed design points `2.8 → 2.10 → 2.9 → 2.11`: an inserted point had landed before the one it should follow, so "the ninth design point" was the tenth block. |
| R5-04 | documentation / `G6` | `backlog/graphics.md` T24 still spelled `RenderCommand`, renamed to `DrawItem` on 2026-11-18 in the same tree the row describes. |
| R5-05 | design intent / `G12` | The `collect*` / `resolve*` / `effective*` derivation verbs carried a precise rule — one that is stated in two header comments — but no design point, so the next helper had no place to look up which verb it was. Written down now (`graphics-scene.md` 2.12, guide §5.2). |

## 2. Alternative Architectures Considered

Each entry states the current shape, the alternative, why it may be better, what it costs, and the
recommendation. Nothing here is adopted by this round; two are registered with triggers.

### 2.1 A `FrameContext` argument to the per-frame hook (registered, T40)

- **Today**: `RenderEngine::frame()` takes no arguments and `FrameUpdate::update()` takes none. Time
  is the host's: `FpsOverlay` reads `std::chrono::steady_clock` itself (`FpsOverlay.cpp:237`), and an
  animation is expected to be driven by the host's own clock into a transform.
- **Alternative**: one value — frame number, elapsed time, area size in logical and device pixels,
  pixel ratio — passed to `update(const FrameContext&)`; Filament's `FrameInfo`, Godot's `_process(delta)`
  and Bevy's `Time`/`Res` are the same idea.
- **Why it may be better**: it makes the frame a function of `(declarations, context)` instead of
  `(declarations, wall clock)`, which is what a bake, a replay or a deterministic test needs; and it
  removes the only per-object clock in the system.
- **Cost**: a public virtual signature, so every overlay in and out of tree changes; and it is one
  more value every host has to fill in.
- **Recommendation: defer, with a trigger.** Today exactly one consumer wants time and it answers
  itself correctly. `G8` is unambiguous: no second consumer, no argument. The trigger is the first
  feature that needs two of the fields (an animation that must be frame-rate independent, or a
  deterministic replay). The shape to build then is `FrameContext`, not a bare `dt`.

- **Landed 2026-11-18, ahead of that trigger, on an explicit go**: `FrameContext` (frame id, interval, area size,
  ratio) is composed by `RenderEngine::frameContext()` and handed to `FrameUpdate::update(const FrameContext&)`;
  `FpsOverlay` stopped reading `steady_clock`, so the system has one clock again. The trigger is recorded as NOT
  hit rather than declared met, and the rejected shapes (`update(double dt)`, a host-supplied clock, a fourth
  lease) are in `../decisions/ADR-2026-11-18-the-frame-carries-its-context.md`; the review's determinism argument
  therefore stays open - a host that needs a fixed step keeps its own clock.

### 2.2 `Diagnostics` / `DiagnosticsRoute` as one concept (registered, T41)

- **Today**: `Diagnostics` is the route object the engine owns; `DiagnosticsRoute` is a borrowed
  handle to it that `PassChain` holds (a `Lease`, so it survives the engine). Two headers whose names
  differ by a suffix, one of which reads as the thing the other is a handle to.
- **Alternative**: name the handle for what it is (`DiagnosticsHandle`, `DiagnosticsView`) and leave
  `Diagnostics` as the route; or make the chain hold the `Diagnostics&` and delete the handle.
- **Why it may be better**: `G1` / `G4` — a reader meeting `DiagnosticsRoute` reasonably assumes it
  *is* the route, and then has to discover that the route is the other one.
- **Cost**: a public type rename (or, for the second form, a lifetime dependency the `Lease` exists
  to avoid — the chain may outlive the engine). The rename is the cheap half.
- **Recommendation: defer to the next public-API batch.** A rename with no behaviour change should
  travel with other renames, not on its own (`G8`).
- **Landed 2026-11-18**, in the batch this proposal asked for, on an explicit go: the handle is now
  `DiagnosticsHandle`, `Diagnostics` stays the route, and `RenderDevice::setDiagnosticsRoute` keeps its name
  because that call really does take the route. Two neighbouring spellings were renamed in the same batch
  (`BC-23` the duplicate `MaterialPtr` alias in `Geometry.hpp`, `BC-24` the one-argument `resolveRenderState` →
  `applyStateDefaults`); the decision and the rejected roads are
  `../decisions/ADR-2026-11-18-names-state-what-is-handed-over.md`.

### 2.3 A render graph for the frame compiler (rejected, unchanged)

The design point `graphics-frame-pipeline.md` 2.12 already records why the engine is a frame compiler
and not a render graph: a render graph exists to allocate and alias transients across an unknown set
of passes (Frostbite/UE RDG, Unity HDRP, Babylon `FrameGraph`). This engine's chain is a known,
declared set, its transients are named outputs, and the compiler allocates them once per frame. Adding
a graph would add a retargeting layer over a chain the host already writes. **Rejected; no new
argument this round** — this round's benchmark found nothing that changes it.

### 2.4 RID-style handles instead of `intrusive_ptr` (rejected)

Godot's `RenderingServer` hands out `RID` integers, which makes identity cheap and lifetime
serializable. Adopting it here would trade a compile-time-checked ownership story for a registry and
a lookup per call — the wrong trade for a framework whose value proposition is "you can read the
ownership from the signature" (`graphics_global.hpp`'s four spellings). **Rejected.**

## 3. Benchmark: what the eight engines teach, and what does not transfer

| Engine | Strongest idea worth having | Relevance here | What not to copy |
|---|---|---|---|
| Filament | `Engine` / `SwapChain` / `View` separation, `FrameInfo` per frame, and a renderer with **no render graph** — it compiles a fixed set of passes per view | **high**: our one-engine-one-area and our `View` value are the same shape; `FrameInfo` is the trigger for T40 | a fixed internal pass set: our chain is host-authored |
| O3DE (Atom RPI) | pass templates + `FrameGraph`, and a "feature processor" per visual feature | **medium-high**: the closest sibling to a declarative pass chain; its draw-packet model matches `DrawItem`/`DrawCall` | the template/graph machinery, unless the chain stops being host-authored |
| Unreal | RDG (transient aliasing, async compute phases) and `FMeshBatch`/`FMeshDrawCommand` split | **medium**: the batch-vs-command split is already our item-vs-call split; RDG needs the many-transients problem we do not have | RDG's scope; its cost is a whole retargeting layer |
| Unity (SRP) | `RenderPipeline` as a swappable top-level recipe + `ScriptableRenderContext` | **medium**: `ChainPresets` is our version of the swappable recipe | a context object as a mutable god-handle; our frame is a value |
| Godot | `RenderingServer` decoupled from the scene tree by handles | **low**: we keep ownership in the type system instead | RID indirection |
| three.js | nothing architectural; per-object `onBeforeRender`, hidden renderer state | **low as a model, high as a warning**: this is exactly the hidden-per-frame-state shape our frame value removes | implicit dispatch |
| Babylon.js | `FrameGraph` (new), material variants, `Effect` cache | **medium**: the material-variant cache idea is what `CompiledPrograms` / `ContentCaches` already do | graph again |
| Bevy | render-graph nodes + ECS schedule; explicit `Time` resource | **medium**: the `Time` resource is T40's argument made for us | ECS coupling |

Conclusion: the ideas that transfer are already in (frame value, one recipe per picture, item-vs-call
split, explicit time on the trigger). The ideas that do not transfer are all *solutions to a problem
this engine does not have* — an unknowable pass set, and many simultaneous transients.

## 4. The Refined Conceptual Model (unchanged, now stated in four verbs)

Nothing in the model moved this round; what moved is that it is written down in one more place.

```
host declarations            engine (compiles)              backend (materialises)
  PassDesc / PassChain  ──▶  FrameDescription  ──▶  targets, subpass, barriers, commands
  Scene (author tree)   ──▶  publication ──▶ DrawItem ──▶ one DrawCall per item
  Light / Material      ──▶  LightSet / MaterialValues ──▶ declared bindings + push ranges
```

and the derivation vocabulary a reader now meets in one place (`graphics-scene.md` 2.12):

| Verb | Inputs | Answer |
|---|---|---|
| `collect*` | a node | the authored value folded up the tree (unset items stay "nobody said") |
| `resolve*` | an authored value + one context | the final value, plus who decided each item |
| `effective*` | a node (+ implicit context) | the one-step composition of the two above |

## 5. Prioritised Plan

### P0 — Fundamental (none open)

No architectural flaw was found this round. The two items that looked like P0 candidates were checked
and refuted with evidence rather than argument:

- "The engine is still a god object" — refuted: 26 public members, three delegated mechanisms, and a
  design point that names the boundary (`graphics-architecture.md` 2.8).
- "The frame cannot be reproduced deterministically" — real but narrow: exactly one consumer
  self-clocks, so it is a trigger, not a flaw (T40).

### P1 — Important, landed this round

| Item | Why it mattered | What changed |
|---|---|---|
| R5-01 | The first thing a host copies did not compile; the failure mode is a new user's first hour | guide §3.1 corrected against the headers; the block is now compiled as `tests/test_graphics/GuideExample.cpp`, its device-free half is **executed** by `GuideExampleTest`, and `check_doc_symbols.py` requires every statement in the block to exist in the file |
| R5-02 | Four pointers that lead nowhere; no owner, no compiler | all four repaired; `scripts/check_cross_refs.py` resolves every `name.md §N.M` in `.ai/**` and `docs/guides/**` and is mutation-tested |

### P1 — Registered with triggers (not done)

| Item | Trigger |
|---|---|

**T40 and T41 landed the same day** (explicit go): `DiagnosticsRoute` → `DiagnosticsHandle`, with `BC-23` / `BC-24` riding on
it - see the correction in §2.2 and `../decisions/ADR-2026-11-18-names-state-what-is-handed-over.md`;
`T40` (`FrameContext`) landed too, see the correction in §2.1.

### P2 — Landed this round

| Item | What changed |
|---|---|
| R5-03 | `graphics-architecture.md` 2.9/2.10 swapped into numeric order, **IDs unchanged**; `check_design_docs.py` gained R6 (numbered items ascend, in both the `### N.M` and the `N.M **point**` forms) |
| R5-04 | `backlog/graphics.md` T24 now says `DrawItem` |
| R5-05 | the three derivation verbs written down as a design point and as a guide paragraph |

## 6. Actual Changes Made

| File | Change |
|---|---|
| `docs/guides/graphics.md` | §3.1 example corrected (three compile errors) and marked `// example:`; §4.3 pointer repointed at `graphics-rendering.md` §2.6/§2.7/§8 T5; §5.2 gained the three-verb paragraph |
| `tests/test_graphics/GuideExample.cpp` | **new**: the compiled copy of the example; `guideScene` / `guideCamera` executed by `GuideExampleTest` (asserts one collected draw item), `guideHelloTriangle` compiled and not called |
| `tests/test_graphics/CMakeLists.txt` | the new translation unit |
| `.ai/design/graphics-architecture.md` | 2.9 before 2.10 (ids unchanged) |
| `.ai/design/graphics-scene.md` | new 2.12: the three derivation verbs |
| `.ai/backlog/{async,base-crash,graphics}.md` | three dead references repaired, one stale spelling fixed, T40/T41 added |
| `scripts/check_cross_refs.py` | **new gate** |
| `scripts/check_design_docs.py` | R6 (numbered items ascend) |
| `scripts/check_doc_symbols.py` | rule 3 (`// example:` blocks must exist in the file they name) |
| `.github/copilot-instructions.md` | the `.ai/` post-edit command list names the new gate |

## 7. Census and Evidence

| Fact | Value | Command |
|---|---|---|
| `test_graphics` | **619** cases (was 618; +`GuideExampleTest`) | `./build/bin/test_graphics --gtest_list_tests \| grep -c '^  '` |
| `RenderEngine` public members | 26 | `grep` over `RenderEngine.hpp`'s `public:` blocks |
| Dangling section references before / after | 4 / 0 | `python3 scripts/check_cross_refs.py` |
| Design points out of numeric order before / after | 1 page / 0 | `python3 scripts/check_design_docs.py` |
| Guide statements not in the compiled example | 3 fixed statements had no compiled copy before this round; 0 now | `python3 scripts/check_doc_symbols.py` |
| Clock reads in the SDK + engine | 5 `::now()` call sites + 1 cached time point | `grep -rn 'steady_clock' src/viz/graphics` |

## 8. Verification Recipes

```bash
cd build/bin
./test_graphics --gtest_filter='GuideExampleTest.*'      # the executed half of the example
./test_graphics --gtest_list_tests | grep -c '^  '       # 619

cd /opt/opensrc/Vine
python3 scripts/check_cross_refs.py                      # 0 findings
python3 scripts/check_design_docs.py                     # 0 error / 0 warning / 56 docs
python3 scripts/check_doc_symbols.py                     # 2 docs / 138 units agree
# Mutation tests used for the two new rules:
#   base-crash.md §2.9 -> §2.10          => check_cross_refs red
#   design point 2.13 -> 1.13            => check_design_docs red (R6)
#   `engine->frame();` -> `frame(dt);`   => check_doc_symbols red
```

Recorded at the end of the round: `test_graphics` **619**, `test_vk` **174**, `test_vsg` **572**,
`BackendParityTest` **13**, `content_seam` and `vk_exposure` green; **ten** gates green (197 notes /
56 design docs / 1351 include-checked files / 974 public docs / 138 units). No host or backend code
changed, so no demo run was owed.

## 9. Remaining Debt and Open Questions

1. **The guide's four complete programs are all guarded now** (the follow-up batch of the same day
   added §3.3 / §3.4 / §3.5 as `GuideExamplePasses.cpp`). Converting them found seven more defects,
   each of the same family as R5-01: three unqualified `make_intrusive` calls (§3.3 twice, §3.5 once),
   an overlay registered on a `chain` the block never declared (the variable is `pipeline`), the
   default content program set on the ENGINE (`engine.setDefaultContentProgram`) although it belongs to
   the device's frame half (`engine->device()->executor().setDefaultContentProgram`), `&engine` passed
   where the parameter is already a `raw_ptr`, an ignored `[[nodiscard]]` (`addOffscreenToScreen`), and
   two root-namespace `intrusive_ptr`s spelled as if they were `vn::graphics` names. The remaining 20
   blocks are fragments, not programs, so they stay unguarded on purpose - a fragment has no compiled
   counterpart to compare against.
2. **The cross-reference gate does not read intra-page references** (`see §3` inside one page). Those
   are visible to the reader of that page, so they were left out on purpose; a page that is long
   enough to make that a problem is a page that should be split.
3. **`FrameContext` (T40)** is the one design change this round believes is inevitable if the
   framework grows animation or replay. The shape is settled; only the trigger is missing.
4. **The `// example:` rule compares one direction: the guide's statements must exist in the compiled
   file, and the file's extra statements are not compared back.** That is the right direction for now
   (the file may legitimately hold the wrapper a snippet does not show), but a block that *omits* a
   required step still passes. Bringing the second direction in is cheap once a second example is
   converted (item 1).
