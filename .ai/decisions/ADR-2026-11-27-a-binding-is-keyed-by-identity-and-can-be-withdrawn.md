# ADR-2026-11-27: a binding is keyed by the scene's identity, and it can be withdrawn

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.13 / 2.15 / 4.1 / 4.2 / N19 / T46,
  `../design/graphics-frame-pipeline.md` F3 / 4,
  `../design/graphics-architecture.md` 2.10,
  `../research/graphics-2026-11-27-review-19.md` `Z6`,
  `ADR-2026-11-09-address-keys-hold-the-object.md` (the same defect one layer down),
  `src/viz/graphics/{sdk/vine/graphics/Scene.hpp,sdk/vine/graphics/RenderEngine.hpp,src/PublicationStore.cpp}`

## Background

Publication is the content writer's verb: `Scene::publish()` posts an immutable value, and a frame takes the scene's
latest by default. `RenderEngine::bindSnapshot(scene, ref)` is the *override*: "this engine draws this publication
instead", per engine and per scene (an editor and an offscreen bake one publication apart).

Review 19 `Z6` found two defects in that override, and they are the same defect as `Z12` in a different table: a row
that names a borrowed address, in a store that outlives what it names.

1. **The override had no end.** `PublicationStore::bindSnapshot` always stored, `bindings_` had no erase path, and
   `publicationFor` returned the binding whenever one existed. A host that bound once never saw that scene's later
   publications again - and the documented way to "clear" a binding was to bind an **empty handle**, which means "draw
   nothing" (every view answers an empty list). Two different facts - "draw this picture instead" and "draw the scene's
   latest again" - shared one spelling, and one of them did not exist at all.
2. **The row outlived its scene.** A binding was keyed by `const Scene*` and stayed after the scene was destroyed, so a
   new `Scene` allocated at that address inherited the destroyed scene's picture - silently, because the row was found
   before the fallback to `Scene::publishedSnapshot()` was ever reached.

The source table (`publications_`) shared the address key but not the defect: its key is `(scene, revision)` and
`SceneSnapshotRef::revision` is a process-wide counter that never repeats, so a recycled address can never be matched.
That safety is an accident of a counter that happens to be global; it is not a property of the key, and it is not the
reason the binding keys the way it does.

A scene cannot announce its own death to the engines that bound it: content does not know which engines hold an
override, and a destructor that calls back into them is the back edge `G2` forbids (and the repo's "a destructor never
invokes a registered callback" rule). So the shape of the answer is not "notify", it is "make the key an identity".

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. `Scene` carries a content identity (`SceneId`), the store keys both tables by it, `unbindSnapshot` is the withdrawal, and `beginFrame` drops bindings whose scene answers gone | One key rule for every row, and the row's lifetime is the scene's; the withdrawal is its own verb; nothing in the store needs an announcement | A public `Scene::id()` and a registry slot per scene; `Scene`'s layout grows by one handle (a rebuild for a host built against the old header); one `alive()` per binding row per frame |
| B. Add `unbindSnapshot` only, and say the address may be inherited | The smallest diff | Leaves `Z6`'s second fact standing, i.e. documents a silently wrong picture |
| C. Make an empty handle mean "no override" and give "draw nothing" another spelling | No new verb | Loses a legitimate bound value (an empty picture is a real answer for a bake with nothing in it), and still keys by address |
| D. Hold the scene alive in the binding (`intrusive_ptr<Scene>`) | The address cannot be recycled under a live row | The engine becomes an owner of host content with no observable end - a host that drops a bound scene could never learn it is still alive - and the override still has no withdrawal (`Z12`'s option D, one layer up) |
| E. Key the binding by the bound publication's address and keep the scene address only as a filter | The value's address is stable, and the row holds it | Does not answer the lookup: the question is "is there an override for this scene", and a publication is content-agnostic by design (it cannot name the scene it came from) |
| F. Give the store a private per-scene ticket | No public surface | A second identity for content, invented privately, where the framework already has one (`Handle`); `G1` |
| G. Announce the scene's death from `~Scene` to the engines that bound it | The store would learn at once | A back edge from content to engines (`G2`), and a destructor invoking host code |
| H. State that a bind is permanent and that an address may be inherited | Nothing to build | Writes a defect into the contract |

## Decision

Option A.

**The scene has an identity.** `Scene` holds a `SceneId` (`Id<SceneTag>`, i.e. a `Handle`) allocated at construction and
released by its destructor, exactly as `Node`, `MaterialValues` and `ShaderProgram` already do. `ContentId.hpp` states
why: "an address is reused, and a stale one silently names somebody else's bytes" - and a scene is the content root an
engine's tables name across lifetimes. `Scene::id()` is that handle.

**The store keys by it.** `PublicationStore::Publication::content` and `Binding::content` are `SceneId`, so a scene
destroyed while bound and the next scene at the same address are different keys, and inheritance is impossible by
construction rather than by a comparison that has to remember to happen.

**The withdrawal is its own verb.** `RenderEngine::unbindSnapshot(scene)` (and the store's verb of the same name) ends
the override; a frame then takes the scene's latest publication again. An empty handle keeps its one meaning - a bound
picture that happens to be empty - so "draw nothing" and "no override" no longer share a spelling (`G1` / `G4`).

**The row ends with its scene.** `beginFrame` erases the bindings whose handle answers gone
(`ContentRegistry::alive`). That is a poll, and it is the only honest mechanism content's silence leaves: the store
already settles what it holds at the frame open, and a binding is one `shared_ptr` to an immutable value, so the cost is
one registry read per bound scene per frame.

## Consequences

- The override's lifetime is its scene's, never longer: a recycled address cannot inherit it, and a destroyed bound scene
  does not keep its publication alive for the engine's life.
- `publications_` rows need no liveness check, and that is now stated rather than accidental: their key carries the
  process-unique publication revision and the sweep drops them within a frame.
- Two additive public surfaces (`Scene::id()`, `RenderEngine::unbindSnapshot`), and one that is not free: `Scene`'s LAYOUT
  grows by one handle, so a host built against the previous header must be rebuilt. That is the batch that touches the
  layout, which is the only time a field is added (the field rule in `.github/copilot-instructions.md`), and it buys the
  content root an identity every other content object already has. Every scene now takes one registry slot for its life.
- Residuals. A scene destroyed while a frame is being built keeps its row until the next frame open (one frame of
  retention), and an engine that never opens a frame never settles - the same reason it never draws. A `ContentRegistry`
  that retires a slot at the 24-bit generation limit simply means the next scene gets another slot; nothing resolves to
  the retired one.

## Evidence

- `PublicationStoreTest.ABindingOverridesTheScenesLatestPublication`: bind / empty bind / withdraw / re-bind, and the
  withdrawal answering the scene's latest.
- `PublicationStoreTest.ASceneAtARecycledAddressDoesNotInheritAnOverride`: the same storage built over by the next scene;
  the new scene draws its own publication, and the dead row leaves at the frame open.
- `FrameExpandTest.ABoundPublicationIsThePictureTheFrameDraws`: bind, publish, withdraw, and the frame built afterwards
  draws the scene's latest publication (the review's "bind, unbind, publish, the frame draws the new publication").
- Mutation proof: with either half reverted - the key compared by address, or `unbindSnapshot` doing nothing - the cases
  above fail (see the memory record for the numbers).
