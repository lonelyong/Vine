# Graphics review 22 (2026-11-28): what a dynamic scene does to its publication

> **Status**: two findings (`Z29`, `Z30`), both high, plus one low gap (`Z31`) - **all three fixed 2026-11-28**, in the
> same session, with four cases and four mutations (§2.5 / §3.5 / §5). §2 and §3 state the rule each fix moved and, at
> their end, what was built and what was measured; §4 is what was read and why it is clean.
>
> **Scope**: the CONTENT side of a dynamic scene - the half review 21 did not open. Review 21 read every backend table that
> keeps something between frames and asked "who can still name this entry"; this round asks the same question one layer
> up: when the scene's resources are added, removed or modified, what happens to the four values the content side derives
> from them - the baked rows, the per-view list memo, the publication a frame draws, and the light set - and where does
> the code learn that the content moved at all.
>
> **Verified 2026-11-28**: read first (every claim cites the line it was read at), then fixed and run: `test_graphics`
> **714/714** (four new cases) and `test_vk` **240/240**; four mutations, each red on exactly its own assertion (§6).
>
> **In one line**: the derived half of a scene is keyed on counters the ROOT TREE owns and on no fact about which tree it
> describes, and one of the scene's own switches - its visibility - never reaches the publication at all, so the two most
> ordinary dynamic operations (hide the scene; replace the whole content) change the queries and leave the published
> picture alone.

## 1. Why this theme, and what "read" means here

The dynamic scene is the case every static frame test skips: nodes added and removed while the session runs, a mesh
regenerated, a material re-coloured, a document swapped under one pane. Review 21 settled what the BACKEND does with the
objects that stop being named. What it could not settle is the step before it: whether the content side ever *tells* the
backend that the content moved, and whether the value a frame draws is rebuilt when it should be.

Four derived values were read, with the one question: **what makes this value stale, and where does the code learn that?**

- the baked rows (`SceneBake`: every node's own facts plus the fold) - `ensureRows` (`ScenePublisher.cpp:195`);
- the chunked publication (`SceneSnapshot`) - `snapshot()` (`ScenePublisher.cpp:171`) and `makeSceneSnapshot`
  (`SceneSnapshot.cpp:20`);
- the per-view list memo (`DrawListMemo`) - keyed by the publication's revision (`ScenePublisher.cpp:74-101`);
- the light set (`Scene::lightSet`) - keyed by every light's (identity, revision) (`Scene.cpp:258`).

The two findings below are both of one shape: **a value that must answer a question about the scene reads a stand-in
owned by something else** - the root's own edit counter for "is this table this scene's", and "nothing in the query
path" for "is the scene visible". Both fail silently: the picture is wrong and every counter that reports health says
`Posted` / `Unchanged` as usual.

## 2. Z29 (high) - `Scene::setVisible(false)` publishes the whole picture

### 2.1 Two answers to one question

`Scene::setVisible` (`Scene.cpp:140-147`) flips `visible_` and announces a content change. From that moment the content
QUERY answers "nothing":

- `ScenePublisher::collectDrawItems` gates on it first - `if (!scene_.visible_ || !scene_.hasRootedRoot()) return
  noCommands();` (`ScenePublisher.cpp:56`), which is what `SceneTest.CollectCommandsHidesWholeScene`
  (`tests/test_graphics/GraphicsTest.cpp:799`) asserts;
- `Scene::boundingBox` (`Scene.cpp:290`) and every ray walk (`RayIntersection.cpp:546`, `:562`) read the same flag.

The PUBLICATION never reads it. `ensureRows` starts with `hasRootedRoot()` only (`ScenePublisher.cpp:200-205`); the rows
are then built from the root (`rebuildRows`, `ScenePublisher.cpp:376-381`) exactly as when the scene is visible;
`snapshot()` copies them into a new `SceneSnapshot` (`ScenePublisher.cpp:171-193`); `makeSceneSnapshot`
(`SceneSnapshot.cpp:20-89`) is a value copy with no gate.

And a frame draws the publication: `PublicationStore::publicationFor` answers `scene.publishedSnapshot()`
(`PublicationStore.cpp:54`), which `EngineCompile.cpp:377-379` binds to the pass' draw packet.

### 2.2 The step-by-step, so nothing hides in it

`setVisible(false)` (epoch moves because `invalidateContent` announces on the root and invalidates every bounds stamp,
`ScenePublisher.cpp:156-166`) → the next `publish()` finds `bake_epoch_ != epoch` → `refreshChangedRows`
(`ScenePublisher.cpp:210-214`) re-reads every announced row and finds `RowOwn` unchanged on every NODE → answers "no
reason to keep the old table" → `rebuildRows()` builds the SAME rows from the same visible tree → `makeSceneSnapshot`
numbers a new publication → `publish()` answers `Posted` (`ScenePublisher.cpp:102-118`) → the frame draws everything
while the query says there is nothing to draw.

The contrast that names the defect: hiding the ROOT NODE works - `Node::setVisible` (`Node.cpp:144-148`) announces and
`bakeSubtree` prunes an invisible subtree before any row exists (`DrawListBake.cpp:303`) - so "hide the content" has two
spellings and only one of them reaches the picture (`G1`, `G6`).

### 2.3 What it costs

- A host that hides the 3D view (a menu toggle, a "show the log instead" state) keeps the scene on screen; the host's
  own check reads the same query the API says to read, so it believes the switch worked.
- The toggle also pays a FULL re-copy: `setVisible` invalidates every bounds stamp, the table is rebuilt with fresh
  `resolved_serial`s (`DrawListBake.cpp`, `storeResolved`), and `makeSceneSnapshot`'s share test compares serials
  (`SceneSnapshot.cpp:55-72`), so no chunk is shared and the whole publication is copied to hide a scene.

### 2.4 The witness, and the rule the fix must state

Shape: publish, frame → the pass' command list has one draw; `setVisible(false)`, publish (require `Posted`), frame →
require the list empty AND `collectDrawItems` empty (the two readers must agree, which is the point, `G1`); then
`setVisible(true)`, publish, frame → one draw again, and no stream is retransmitted (the review 21 sweep must not have
withdrawn anything - hiding is not leaving). Mutation: remove the publication-side gate → the empty-list assertion red;
keep it while also withdrawing residency → the retransmission assertion red.

The rule: **the publication IS the picture, so everything the collection gates on gates the snapshot too** - one flag,
two readers, one answer. The fix batch picks the mechanism (`ensureRows` treating `!visible_` like "no rooted root", or
`snapshot()` answering the empty publication) under one constraint: hiding must release NOTHING (F27 / F28) - the live
objects keep their own share of every description (`Geometry::description()`'s cache, `Material::description()`,
`TextureSlotBinding::texture`), so review 21's frame-end probe still finds them held.

### 2.5 Fixed 2026-11-28 - the flag is read where the table is built

The mechanism is the stronger of the two: **`ScenePublisher::ensureRows` treats `!scene_.visible_` exactly like "no
rooted root"** - the table is emptied and the publication cache reset - so the publication, the collection, the box and
the ray walks answer from one flag by construction, and `Scene::setVisible` no longer announces a content change (there
is nothing to announce: the table reads the flag itself, and the old announcement walked every node's box for a switch
no node states). Hiding releases nothing: the live objects keep their own share of every description, so the backend's
frame-end probe still finds them held - the same property `graphics-content-lifecycle.md` F27 / F28 states for a hidden
node, now stated for the scene level too.

- Witness: `RenderEngineTest.AHiddenScenePublishesAnEmptyPicture` (`tests/test_graphics/GraphicsTest.cpp`) - publish +
  frame → 1 command; `setVisible(false)` → the query empty, `publish()` `Posted`, the frame draws 0; a second publish
  while hidden is `Unchanged` (the table emptied ONCE - `resetToEmpty` is idempotent, so the steady state of "nothing to
  draw" allocates nothing); `setVisible(true)` → `Posted`, 1 command again.
- Mutation: removing `!scene_.visible_` from `ensureRows` reddens exactly that case (the frame keeps drawing 1), and no
  other of the four new cases moves.

## 3. Z30 (high) - the baked table is keyed on the root's own clock

### 3.1 A per-tree counter compared across trees

`ensureRows` is the ONE place that decides "are these rows still this scene's":

```cpp
const std::uint64_t epoch = scene_.root_->contentEpoch();          // ScenePublisher.cpp:209
if (content_bake_ == nullptr || bake_epoch_ != epoch) {            // ScenePublisher.cpp:210
    ... rebuild ...
    bake_epoch_ = epoch;                                           // ScenePublisher.cpp:216
}
```

`contentEpoch()` is a per-subtree count: `0` on a new node, `++` on the node and every ancestor per announcement
(`Node.cpp:15-38`). It has no component that is unique to a tree, and `bake_epoch_` (`ScenePublisher.hpp:209`) records
it as if it were "the table's identity". Two trees built by the same code reach the same number - trivially the identity
counter and every ancestor's counter bumped by the same builder.

`Scene::setRoot` (`Scene.cpp:171-201`) replaces `root_` and announces on the NEW root (`invalidateContent`,
`ScenePublisher.cpp:156`); `Scene::clear` (`Scene.cpp:203-211`) drops the root and cannot announce (the root is already
null). Neither touches `content_bake_` or `bake_epoch_`.

### 3.2 Three outcomes, one cause

1. **A replaced root draws the old tree.** `setRoot(a)`, `publish()` (Posted, `bake_epoch_ = a.epoch`), `setRoot(b)`
   where `b` was built by the same builder → `b.epoch == a.epoch` → the epoch compare passes → `refreshBoxes` runs on
   the OLD table's nodes, `snapshot()` finds its cached publication matching the frozen version triple
   (`ScenePublisher.cpp:184-189`) → `publish()` answers **`Unchanged`** and the frame keeps drawing A's content. Reopen
   the same document in a pane that reuses its Scene - the most natural "load again" - and A IS B, so the collision is
   the default, not an accident.
2. **A freed tree is still walked.** `SceneBake::nodes` holds raw `const Node*` on purpose ("content side only",
   `DrawListBake.hpp`). When the old root was the scene's last share it is destroyed by `setRoot`, and `refreshBoxes`
   (`ScenePublisher.cpp:219-232`) dereferences each leaf row's node - `readLeafBox` (`DrawListBake.cpp:40-53`) calls
   virtual `subtreeBounds` / `boundingBox` on it. Geometry leaves survive through the row's own share
   (`RowOwn::Body::geometry`), and a row with child rows is skipped; every other leaf row - a childless `Group`, a plain
   `Node`, a custom node - reads freed memory.
3. **A cleared scene can never show its next content.** `clear()` then a collect runs `resetToEmpty`
   (`ScenePublisher.cpp:243-254`), which clears the rows and resets `publication_cache_` - but leaves `bake_epoch_` at
   the old tree's number. `setRoot(c)` with a colliding epoch then skips the rebuild, the rows stay EMPTY, `snapshot()`
   caches an empty publication and every later `publish()` answers `Unchanged`: the scene draws nothing, forever, with
   no refusal and no diagnostic.

### 3.3 The guard that would catch it cannot run

`rowsStillMatchTheGraph` DOES compare the root's identity - `content_bake_->rows[0].identity != scene_.root_->id()`
(`ScenePublisher.cpp:260`). But `ensureRows` calls it only from inside the `bake_epoch_ != epoch` branch, i.e. only when
the counter says something moved. On a collision the counter says nothing moved, and the check that would have said
otherwise is never asked. This is `G1` at the table: "which tree these rows describe" is a fact the table must carry
itself; a clock owned by the tree is not that fact, and `G10` is what the raw `bake.nodes` add to it.

### 3.4 The witness, and the rule

Two cases, both deterministic (the builder runs twice, so the counters match by construction - no luck involved):

- `AReplacedRootIsNeverServedFromTheOldTablesEpoch`: one Scene; build tree A and tree B with one helper; `setRoot(a)`,
  publish (require `Posted`); drop the test's own share of `a`; `setRoot(b)`, publish; require `Posted` and the
  publication's only command to carry `b`'s leaf identity (`DrawItem::node == b_leaf->id()`), not `a`'s. Red today
  (`Unchanged`, and `a`'s row).
- `AClearedSceneDoesNotInheritItsOldTablesAnswer`: `setRoot(a)`, publish; `clear()`, publish (empty, Posted);
  `setRoot(b)`, publish; require a non-empty picture. Red today (empty, `Unchanged`).

Mutations: removing the identity guard → both red; restoring it alone → the replace case green and the cleared case
still red on the "not built" flag; restoring `resetToEmpty`'s invalidation → green. Every arm load-bearing.

The rule: **a derived table names the root it was built from** - compare `rows[0].identity` with `root_->id()` once per
`ensureRows` (O(1)) and treat the epoch as a change clock valid only while that identity matches; `resetToEmpty` must
leave the table "not built" (clear the recorded epoch / mark it), so a scene that is cleared and re-rooted cannot
inherit the previous tree's answer.

### 3.5 Fixed 2026-11-28 - the table says whose it is, and the version says which table

- **`ScenePublisher` records `baked_root_` (the root's `NodeId`)** and `ensureRows` compares it once per collect, before
the epoch: a different root skips the incremental path entirely and rebuilds (`SceneBake::nodes` describes the old tree,
so there is nothing to refresh in place). `rebuildRows` records the root; `resetToEmpty` forgets it.
- **The publication compares ONE value, `detail::RowsVersion{table, epoch, boxes, values}`** (`SceneSnapshot.hpp`), and
the snapshot carries it. `table` moves on every rebuild and every reset, so a cleared-and-re-rooted scene cannot match
a cached empty publication even when its epoch repeats; the epoch stays what it always was - the cheap "did a node
announce" clock for the incremental path. `makeSceneSnapshot`'s chunk sharing requires the same table as well, so the
rule is stated once.
- `Scene::setRoot` / `Scene::clear` no longer announce a content change either: the table follows the root by identity,
and announcing would invalidate every box of a tree that is only now being adopted.
- Witnesses: `SceneTest.AReplacedRootIsNeverServedFromThePreviousTablesEpoch` (two roots from one builder, the old one
  dropped before the swap; `publish()` must answer `Posted` and the picture must carry the new leaf's `NodeId`) and
  `SceneTest.AClearedSceneDoesNotInheritItsOldTablesAnswer` (publish, clear, publish, re-root: the new content must
  appear). Both assert the colliding epoch as their PREMISE, so a builder change that stops colliding shows up as a red
  premise rather than as a silently weaker case.
- Mutations: removing `baked_root_ != root` reddens both (and the hide/show round trip, which relies on the same guard
  to rebuild after `resetToEmpty`); removing `table` from the version reddens both again, at `publish().posted()` being
  false and the picture empty.

### 3.5 Why this is the mirror of review 21

Review 21's rule was "an entry that holds its key ends when nobody else names it". Here the same question is asked of a
different table and the answer is missing: the rows DO hold what they describe (geometry, material, program, boxes), but
the key that decides "are these rows current" is a number the described tree owns - so a NEW tree at the same number
inherits the old answer. One question, two tables, one of them keyed by a stand-in (`G1`).

## 4. Read and clean (with the property that makes it so)

- **Lights are describe-time values, not rows.** `RenderPass::describe` copies `scene->lightSet()` into the packet
  (`RenderPass.cpp:197-199`), and `Scene::lightSet` is keyed by every light's (identity, revision) (`Scene.cpp:258-290`);
  every `Light` mutator bumps the revision (`Light.cpp:67-201`; `setName` does not, and a name is not lit). So
  `addLight` / `removeLight` / `clearLights` need no content announcement - a memoised command list cannot freeze the
  lighting - and adding `invalidateContent()` there would pay a full rebuild per light edit for nothing.
  (Correction 2026-11-28: this asked whether an EDIT reaches the picture; it did not ask whether a REPLACED list can
  reuse the address and the revision. It can - the address is freed by the same mutators and `Light::revision_` starts
  at 1 for every light - see `graphics-2026-11-28-derived-state-census.md` §4, finding `Z32`.)
- **Material and program edits reach the picture without an announcement.** `refreshValues` re-takes
  `MaterialValues::description()` / `ShaderProgram::description()` per row at the publish boundary
  (`DrawListBake.cpp:446-520`); every `Material` setter compares before bumping (`Material.cpp:57-135`), every
  `MaterialInstance` setter routes through `markOverridden` (composite revision, `MaterialInstance.cpp:16-143`), and
  `StateNode` / `Geometry` announce the pointer changes (`StateNode.cpp:18-176`, `Geometry.cpp:394-416`).
  `ResolvedRow::material` / `program` are owning shares, so a dropped object cannot dangle in a row.
- **A removed node's residency still ends through the rows' shares.** `RowOwn::Body` carries `GeometryPtr` +
  `GeometryDescriptionPtr` + one `ResolvedDraw` per primitive with owning material/program shares; a snapshot drops the
  `nodes` record on purpose (`SceneSnapshot.hpp`). The last snapshot using a dropped object is its last content-side
  share, so review 21's frame-end sweep withdraws the entry exactly when it said it would.
- **Chunk sharing is keyed on the fold's own serial, which has one writer.** `makeSceneSnapshot` shares a 32-row chunk
  only when identity, links, node epoch, `RowOwn::Fold` equality and `resolved_serial` all agree
  (`SceneSnapshot.cpp:55-72`), and the serial moves only through `storeResolved`, compared before stored
  (`DrawListBake.cpp:66-79`). A row whose answer moved cannot be served from the previous publication.
- **The scene memo ages by frames and is keyed by the publication.** `DrawListMemo::find` matches revision + view
  (`DrawListMemo.cpp:9-25`); `beginFrame` drops entries nothing asked for in the previous frame (`DrawListMemo.cpp:45-52`);
  the engine announces the token per frame (`RenderEngine.cpp:612`) and the store opens it on every source
  (`PublicationStore.cpp:66-92`).
- **The engine's publication store ends what no frame names.** Sources not used by the previous frame are erased, and an
  override whose scene handle is no longer alive is dropped with the publication it kept (`PublicationStore.cpp:66-92`);
  `SnapshotSource` holds only a shared, immutable value and a mutex (`SnapshotSource.hpp`).
- **The scene's own identity survives address reuse.** `SceneId` is released by the destructor (`Scene.cpp:110-116`) and
  the override row is keyed by it (`PublicationStore.cpp:19-30`) - the question review 21 settled for content objects is
  settled for scenes too.

## 5. Z31 (low) - the batch's "starting picture", recorded then fixed

The edit batch's documented promise - "inside a batch the answer is the picture the batch started from" (`Scene.hpp`,
`collectDrawItems`) - held only once a publication was BUILT: `publish()` returns early inside a batch, and `snapshot()`'s
"pre-batch picture" was `publication_cache_`, which `resetToEmpty` clears. A first-ever `collectDrawItems` inside a batch
(or one after `clear()`) answered the CURRENT, half-edited rows under the frozen version. The FRAME path was safe - it
draws `publishedSnapshot()`, which `publish()` freezes - so this was a query-vs-doc gap.

**Fixed 2026-11-28 in the same batch**: inside a batch `ScenePublisher::snapshot()` answers the PREVIOUS PUBLICATION -
the value `publish()` answers `InsideBatch` with (`detail::emptyPublication()` before anything was ever posted) - so the
query, a mirror and a frame agree on "the state the batch started from" without depending on a cache surviving the
batch's own edits, and `collectDrawItems`'s fast path (hidden / unrooted) applies only outside a batch.
`SceneTest.AnEditBatchAnswersThePublishedPictureWhateverTheBatchEdited` is the witness (remove a child, then the root,
inside the batch: the query keeps answering the published picture, and the edit lands in one publication after
`endEdit`); deleting the batch branch reddens exactly that case at its own assertion (the mid-batch answer collapses to
0 after the root drop).

## 6. Gates

- `test_graphics` - **714/714** (710 before; the four new cases) - and `test_vk` - **240/240** (unchanged; the SDK path
  every vk case drives was rebuilt); `test_appfw` - **69/69** (the host that publishes per frame was rebuilt against the
  new contract).
- Mutations, each run separately and restored by reverse patch: (a) `ensureRows` without the visibility gate →
  `AHiddenScenePublishesAnEmptyPicture` red only; (b) without `baked_root_ != root` → the replace case, the cleared case
  and the hide/show round trip red; (c) without `table` in `RowsVersion` → both identity cases red, at
  `publish().posted()` false and the picture 0; (d) without the batch branch in `snapshot()` →
  `AnEditBatchAnswersThePublishedPictureWhateverTheBatchEdited` red only. Every arm load-bearing; no case red without its
  arm.
- Documentation gates: `python3 scripts/check_ai_docs.py --strict`, `python3 scripts/check_design_docs.py`,
  `python3 scripts/check_cross_refs.py`.

## 7. What the next round should open

- Review 21's still-open items: the sweep's two-driver story (`VkMultiSessionTest` has the harness) and `Z26`'s sampler
  table, which stays the one table whose key is a value and whose ending is "never".
- The mirror path (`SceneRows::snapshot` consumed by a side that owns no graph): it answers through this round's
  `snapshot()`, so a mirror now sees the published (or batch-frozen) value by construction - and `RowsVersion.table`
  is what a mirror can compare to tell "a new table" from "the same table, edited".
- The residency half of `Z29` has no case in `test_graphics` (the suite has no device): it rides on the hidden-node
  cases in `test_vk` through the same share story. A direct case - hide the scene, show it, assert `stream_builds`
  unchanged - is owed if a scene-level hide ever becomes a product path.
