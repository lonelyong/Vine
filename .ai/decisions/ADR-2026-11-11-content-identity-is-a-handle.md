# ADR-2026-11-11: content identity is a handle, not an address

- Status: Accepted
- Date: 2026-11-11
- Related: `../research/graphics-2026-11-11-next-design.md` (§3.1 identity, §3.3 `ContentRef`),
  `../research/graphics-2026-11-11-next-audit.md` (A1 / A12 / steps 3a-3b),
  `ADR-2026-11-09-address-keys-hold-the-object.md` (partly superseded by this document)

## Background

`ADR-2026-11-09` rejected "stable identity" at the time, chose "a cache keyed by address must hold the object its key names", and wrote down the reopening conditions:
a third hole violating that convention, a table that cannot reach the handle, or needing to recognise one piece of content across sessions or processes. At least two of the three hold today:

- **Replay / dump need a stable key**: the next design's snapshot has to be dumpable, replayable and able to recognise the same content by `(id, revision)`; an address cannot do that.
- **Address reuse already has a patch layer**: `ProgramFacts::generation` is the insurance for "same address, different identity", which shows that "address is identity" no longer holds -
  it is only patched in one table.
- **The backend still has a material table keyed on identity alone**: `ContentStore`'s material lookup is keyed on the address alone and replaced in place, so an old plan hits the new value -
  the residual of audit `A1` (`R9`/`I1`) at the backend layer.

At the same time the next design (§3.1/§3.3) has fixed public identity as `Handle` (gen24/domain8/index32) + `ContentRef{id, revision}`,
and every row of a `ColdChunk` is named by it. The design is settled; what remains is the order of landing.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep address identity, make the revision explicit, reclaim by epoch | The smallest change; only backend tables are touched | No "a stale handle answers gone"; replay / dump still need their own key; patches such as `generation` stay in every table (`G6`) |
| B. Identity becomes `Handle` + `ContentRef`, public types change with it | A stale identity is decidable and address reuse disappears as a class; replay / dump have a stable key; consistent with the settled design | Public API/ABI changes: `RenderCommand` fields, row identity, mirror keys, plan rows and backend tables all change; old objects must be cleared before rebuilding |
| C. Switch to handles inside the backend tables only, leave public types alone | Small visible change | The handle has to come from somewhere - either every call site maps an address to a handle (an extra table + a lookup, `G7`), or "two layers of identity" is accepted (`G1`) |

## Decision

Chose **B**, landed in three stages, each of which compiles and tests on its own:

1. **Public identity** (done): a new `sdk/vine/graphics/ContentId.hpp` - `Handle`, typed `Id<Tag>`,
   `NodeId`/`GeometryId`/`MaterialId`/`ProgramId`/`TextureId`/`InstanceId`, `ContentRef`, `contentRefOf()`,
   `ContentRegistry` (including `instance()` for the process default domain and 24-bit wraparound retirement) and a `std::hash` specialisation.
   A `Node` allocates a `NodeId` when constructed and releases it when destroyed, with `Node::id()`; the row identity chain moves to `NodeId` throughout: `Row::identity`,
   `RenderWorld::RowId`, `DeltaChannel::Delta::identity`, the mirror delta and `SceneSnapshot::identities`;
   the public `RenderCommand::row` (`const void*`) becomes `RenderCommand::node` (`NodeId`), and
   `CollectedCommand`/`CompiledCommand`, `GeometryFacts` and `ContentStore`'s geometry row table and live table are keyed by `NodeId` along with it.
   A hand-written command with no node is named by `geometry->id()` - a **named conversion**, not a second truth.
2. **`ContentRef` becomes the only key** (materials and programs done, the geometry merge not done): `MaterialBase` already holds its own typed id, and the material table has gone
   from being keyed on the address alone to `(MaterialId, revision)` (removing the `A1` residual); `ShaderProgram` already holds a `ProgramId`, and `program` in the plan and the record
   is a `ContentRef` (the `ProgramRef` type is deleted), the program table builds rows on `(ProgramId, revision, variant, kind)` and `ProgramFacts::generation` is deleted with it;
   merging the geometry row's `geometry` + `geometry_revision` was still pending at the time.
   > Addendum (2026-11-12): the geometry merge is done - `geometry`+`geometry_revision` in `CollectedCommand` / `CompiledCommand` / `GeometryFacts`
   > is one `ContentRef`, and `findGeometry` looks it up by it; all that is left of this stage is step 3.
3. **Lifetime goes to the epoch** (done, 2026-11-12): `ContentStore`'s `markUsed` / parking / `kept` are replaced by a per-row epoch
   (`last_epoch < completedFrame() + 1` and no longer the entry's current answer). Two things were settled when it landed:
   - the "current answer" half is necessary - otherwise culled / hidden content would lose its row for not being hit by this frame's plan, directly conflicting with
     F27/F28 of `graphics-content-lifecycle.md` (culling does not drop an upload);
   - an entry outlives its row by one step (a material row borrows the material's textures), so an entry is dropped only when "the host no longer wants it" and "no row still answers
     with it" both hold, and a material arena slot follows its last row.

**A is not done**: the design has fixed identity as a handle, so A would leave the design documents permanently inconsistent with the code, and replay / dump would still need their own key.
**C is not done**: an address→handle mapping table is exactly the "push the complexity down one layer" that `G7` names, and it leaves two layers of identity.
**It is not done in one go**: an intermediate state would hold two lifetime mechanisms in the same `ContentStore` and throw away the evidence already verified.

## Consequences

- Good: a stale row / stale plan can answer "gone", and the whole class of wrong pictures from address reuse cannot happen any more; replay and dump have a stable key
  (`(id, revision)`); the identity sources of patches such as `ProgramFacts::generation` collapse into one concept (`G1`/`G6`).
- Bad / cost: public types change (`RenderCommand::row` → `node`, `Row::identity`/`RowId` become `NodeId`, and `program` in a plan goes from `(address, revision)` to `ContentRef`),
  so producers and consumers change together; `Node` and `ShaderProgram` each grow by 8 bytes in layout, with one registry lock on construction and one on destruction (once per content
  object, not inside the frame loop);
  after changing the layout of `Node`/`RenderCommand`/`ShaderProgram` the old objects must be cleared before rebuilding, or an object from the previous layout is read
  (measured here as `malloc(): invalid size`).
- Cost (found while landing): `ContentRef::revision` has to be `std::uint64_t` - the design draft said `std::uint32_t`, while every
  `revision()` in the SDK is 64-bit, so a narrow key would wrap after four billion edits; the draft fragment has been synced.
- Trigger conditions (when to reopen): if some table **cannot reach** a `Handle` (a bare buffer with no content object, say), that table first follows
  `ADR-2026-11-09`'s "holding is identity" and records "cannot reach the handle" back into this document as a reopening signal; if a handle's domain has to cross
  processes, decide the serialisation format separately.

## Evidence

- `nodeId::` allocation / release semantics: `ContentRegistryTest` (5 cases) and `ResidencyTableTest` (1 case) in `test_graphics`.
- The row identity migration: `test_graphics` 559 cases, `test_vsg` 538 cases, `ctest` 30/30 all green.
- Plans and records carry a `NodeId`: `FrameRecorderTest.EveryFactOfACommandReachesTheRecord`;
  program identity: `ShaderProgram::id()` (allocated in the ctor, released in the dtor); two `ShaderProgram::id()` objects with the same text and the same revision
  get two halves (`ContentHalvesTest.AnotherProgramIdentityIsAnotherHalf`); the program table builds rows on `(ProgramId, revision)`
  (the program assertions in `ContentStoreTest`); a rejected program can still report its name
  (`ContentStoreTest.AProgramThatCannotBeDescribedStillHasItsName`); `ContentSourcesTest` asserts that a fact row carries the program's `ContentRef`.
- `FrameCompilerTest`'s plan assertions; a hand-written command falling back to `geometry->id()`: `GeometryFactsTest`.
- Performance has not regressed: `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` gives 175 µs publish /
  282 µs select under RelWithDebInfo (N2 ≤250 µs and K1 ≤500 µs still pass).
- Gates: `check_ai_docs --strict`, `check_design_docs`, `check_doc_symbols`, `check_include_hygiene`,
  `check_export_annotations`, `check_api_style` and `check_public_docs` are all clean.
