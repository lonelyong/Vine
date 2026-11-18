# ADR-2026-11-12: the snapshot takes over production collection, split into 5a (shadow switch), 5b-1 (flip the source) and 5b-2 (deletion)

- Status: Accepted (the migration shape; 5a, 5b-1, 5b-2a and 5b-2b landed on 2026-11-12, the pass-declaration close-out of 5b-2c has not)
- Date: 2026-11-12
- Related: `../design/graphics-frame-pipeline.md` §9.3; `../research/graphics-2026-11-12-instances.md`;
  `tests/test_graphics/RenderWorldTest.cpp` (the standing cases for the snapshot / bake)

## Background

Step 5 of §9.3 is the only step that "deletes about 2000 lines of live shape": production collection moves from `ContentMirror` + `DeltaChannel` +
`DrawListMemo` + `Version` to `detail::SceneSnapshot` + `selectSnapshot`. `K1` (203 µs), `K2` (1.03x) and
`K3` only prove that **the prototype's snapshot and derivation are fast enough**; they do not prove **item-by-item equivalence in flight**; and `T17` does not yet cover
every shape of the production path on the snapshot side either. Putting "prove equivalence" and "delete the old path" into one commit makes a failure unattributable
and gives the revert surface too much to cover.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A One step: switch and delete | Fewer commits | Two kinds of risk stacked (equivalence and deletion); a failure is unattributable (`G11`); reverting means restoring 2000 lines |
| B 5a the shadow switch, 5b-1 flip the source, 5b-2 delete | 5a only proves equivalence and does not touch the production shape; 5b-1 only flips the source, with the old path still a shadow; 5b-2 is a mechanical deletion with a small revert surface (`G3` / `G10`) | Two paths coexist during the migration; the 5a / 5b-1 diff has to run in CI |
| C Split into three steps: mirror / delta / memo one each | Each step is smaller | The intermediate states are dead code, each needs its own lifetime / version story, and none has a criterion of its own (`G8`) |
| D Keep the mirror and build only new features on the snapshot | The live path is untouched | Two truths; the complexity has not gone away, only been renamed (`G1` / `G7`) |
| E Run both only in debug | Never reaches production | It cannot be a gate; Release behaviour is unproven and a debug-only path rots (`G11`) |

## Decision

Adopt B.

- **5a = the shadow switch**: `selectSnapshot` and the old mirror path run on the same frame and the same input, diffed item by item (row identity, world,
  material / program ref, sort key, instance id); the old path is still what is rendered. No production shape is deleted, `Scene::publish()` is not made public,
  and the `addPass` family is not deleted (`C11`). The diff mode must be always on in at least one CI build
  (the RelWithDebInfo `test_graphics` is suggested), not only run by hand.
- **The 5a exit**: the whole of `test_graphics` / `test_vsg` green + `T17` old-frame isolation green + `T34` (a fixed scene over ≥600 consecutive frames with an item-by-item diff of
  0) + `K1` / `K2` / `K3` / `N3` / `K7` / `N5` green + zero changes to existing semantic assertions.
- **5b-1 = flip the source (landed 2026-11-12)**: Published mode binds `snapshotSource()`; the old rows / delta path is demoted to
  an optional LEGACY shadow (`setLegacyShadow` / `legacySource`, off by default), and `FrameMode::Mirrored` is renamed
  `FrameMode::Published`. This step deletes no production shape, does not make `Scene::publish()` public, and does not delete the `addPass` family (`C11`).
- **5b-2a = the public entry point plus the source moving home (landed 2026-11-12)**: `Scene::publish()` becomes public (three outcomes
  `Posted`/`Unchanged`/`InsideBatch`); `FrameWorker` holds the `SnapshotSource` directly and no longer goes through `ContentMirror`.
  This step deletes no LEGACY files or cases - they stay in the tree but are off the production path.
- **5b-2b = delete the old path (landed 2026-11-12)**: deletes `ContentMirror` / `DeltaChannel` / `RenderWorld` /
  `RenderWorldSource` / `PublishOutcome` / `SceneRows::version/publish/editing` (including 23 LEGACY cases).
  `DrawListMemo` stays as the pure per-view cache of `SnapshotSource`; `Scene::refreshBoxes` / `refreshMaterials`
  stay (the snapshot's input).
- **5b-2c = close out the pass declarations**: delete `addPass` / `removePass` / `clearPasses` and the `PassRegistry` runtime refusal,
  so a plan is the only shape. `K4`'s denominator is re-taken when 5b-2c starts; `K5` compares `wc -l` before and after; `T7` / `T30` stay green.

## Decision addendum: the four things A5 must answer

1. **What an in-flight frame reads** (`G3` / `I4`): the answer in 5a / 5b-1 / 5b-2 is the same immutable Snapshot value; an old frame holds a reference to the old
   snapshot, and a new publish only swaps a pointer. `K7`'s criterion is `T17`'s item-by-item diff (material / geometry / program) being 0 on the snapshot side;
   without that case, "safe to delete" is not written.
2. **Who publishes** (`G2`): 5a / 5b-1 still publish implicitly inside the frame from `EngineCompile`; 5b-2a makes
   `Scene::publish()` public, returning a three-outcome result; 5b-2b stops `EngineCompile` from calling `worker_->publish` automatically, so the in-frame implicit
   publish disappears. The public surface changes only in 5b-2a.
   > **Erratum (2026-11-12, checked against the landing)**: 5b-2b did **not** remove the in-frame implicit publish - it removed the mirror / delta / `RenderWorld`.
   > The implicit publish is still there (`EngineCompile` calls `FrameWorker::publish` per pass), but a host can now take it over with
   > `RenderEngine::bindSnapshot()`; removing it is 5b-2c's job (along with the `addPass` family, also still to be deleted).
   > Also: the public answer of 5b-2a now carries a `SceneSnapshotRef` (2026-11-12), see §2.13 of `graphics-threading.md`.
3. **Where `DrawListSource` goes** (`G5`): keep this interface, which is **one question**; `SnapshotSource` is the production implementation from 5b-1 on,
   and after 5b-2 deletes `ContentMirror` / `RenderWorldSource` it is still the same interface. The contract of `FrameDescription::content` /
   `expandPass` is unchanged, and the equivalence test can still ask two implementations. It is not renamed into a new public noun (`G8`: the consumer today is
   `expandPass`).
4. **The deletion surface and the baseline** (`K5`): the 5b-2b deletion of `ContentMirror` / `DeltaChannel` / `RenderWorld` is done;
   `K5`'s final `wc -l` is measured the same way as the 2519 (`.hpp` + `.cpp`) and recorded when 5b-2c closes out. 5a / 5b-1 / 5b-2a
   only add, so `K5` is judged only at the final close-out.

## Consequences

- Good: equivalence, the flip, the public entry point and the deletion are kept apart, so a failure is attributable (`G11`); 5a's semantic assertion changes must be 0,
  so `K4`'s numerator cannot inflate through "a refactor along the way"; 5b-2b is a mechanical deletion with a small revert surface (`G10`).
- Bad / cost: two paths coexist during the migration, and the 5a / 5b-1 diff adds CI time; the code grows before 5b-2b, and `K5` is judged only at that close-out.
- Trigger conditions (when to reopen):
  - the 5a / 5b-1 diff cannot reach 0 ⇒ stop and go back to the design layer to re-examine the snapshot model, without entering 5b-2b;
  - the CI cost of the diff is unacceptable ⇒ keep at least one gating scenario in CI and move the rest to nightly;
  - a new class of in-flight / version defect appears in the mirror / delta, a second backend wants to reuse the content layer, or collection enters a performance budget ⇒
    bring the 5b-2b takeover forward (see the backlog).

## Evidence

- **5a + 5b-1 (2026-11-12)**: `SnapshotSource` (snapshot → `DrawListSource`, memoised by `SceneSnapshot::revision`),
  `DrawListDiff` (field-level comparison), `ContentMirror::snapshotSource()` / `legacySource()` /
  `FrameWorker::setLegacyShadow()`. 5b-1's production binding is `snapshotSource()`; the LEGACY shadow is off by default.
  `T34`: `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames` (600 frames, transform / opacity /
  material / placement edits) + `FrameWorkerTest.AWorkerExpandsTheSameFrameTheSceneWould`;
  `SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith` completes `T17`'s snapshot side (material / geometry /
  program revisions); `SnapshotSourceTest.ADifferentSnapshotIsReportedAsADifference` proves the comparison is not vacuously true.
  `test_graphics` 577 / `test_vsg` 542 / `ctest` 30/30 / TSan 0 warnings.
- **5b-2a (2026-11-12)**: the public three-outcome `Scene::publish()`; `FrameWorker` holds the `SnapshotSource` directly, and
  `T31` is pinned by `FrameWorkerTest.PublishAnswersPostedUnchangedAndInsideBatch`.
- **5b-2b (2026-11-12)**: deletes `ContentMirror` / `DeltaChannel` / `RenderWorld` / `RenderWorldSource` /
  `PublishOutcome` and 23 LEGACY cases; `test_graphics` 554 / `test_vsg` 542 / `ctest` 30/30 / TSan 0.
- The denominator after 5b-2c: `test_graphics` 554 + `test_vsg` 542 = 1096; re-taken at the start.
- The first count of cases tied to the deletion surface was 17; 5b-2b actually deleted 23 LEGACY cases (`ContentMirrorTest` 11 + `RenderWorldTest` 7 +
  `DeltaChannelTest` 5). They pinned only the old mechanism and carried no semantics that needed its assertions changed, which matches `K4`'s counting rule.
- Related criteria: `T17` / `K1` / `K2` / `K3` / `K7` / `N5`; the 2519-line baseline of `K5` is in
  §2 of `../research/graphics-2026-11-11-next-audit.md`.
