# ADR-2026-11-27: a share test compares the input the select reads

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-frame-pipeline.md` F1 / F2 / `D14` / `T37`,
  `../research/graphics-2026-11-27-review-19.md` `Z5`,
  `src/viz/graphics/src/SceneSnapshot.cpp`, `src/viz/graphics/src/DrawListBake.hpp`,
  `src/viz/graphics/sdk/vine/graphics/Node.hpp`,
  `src/viz/graphics/sdk/vine/graphics/AutoTransform.hpp`

## Background

A publication shares a chunk with the previous one only when no row in it moved. The test compared four facts:
`identity`, `links`, the node's content epoch, and `Row::resolved_serial` - the serial `storeResolved` moves when the
FOLD's answer changes.

`selectRows` also reads the row's OWN input directly: `own.fold.local` for a view-independent row and
`own.fold.placement` for a per-view one. Two of those are covered by derivation: `local` feeds `resolved.world`, so a
change moves the resolved serial, and `own.body.draws` changes only through a body edit that rebuilds the table
(`RowRefresh::RebuildTable`, fresh serials) or through a value refresh, which moves the publication's `values` version
and disables sharing entirely.

The per-view placement is the one input the fold's answer cannot stand in for. `ResolvedRow` deliberately records only
`view_dependent` plus a conservative box, and two different placements can fold to the same box - so a placement that
changed while the fold's answer did not would ride a shared chunk and be drawn with the previous value.

Today that cannot happen, because every built-in mutator of a placement announces and the epoch is checked. But the
list `Node::announceContentChange` gives the next node author ("transforms, visibility, layers, opacity, draw order,
material, program, state") did not name placements, so the soundness rested on an unstated rule - and review 6's
`P0-1` (a chunk shared on a stale key) is exactly this defect one level down. A pointer comparison was also not
available even in principle: `AutoTransform::viewPlacement()` built a new object per call, so every bake looked like a
new placement.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the epoch as the only guard and say so | no code | The dependency list a node author follows stays incomplete; the next node type that answers a placement without announcing serves a stale chunk - the defect this page exists to prevent (`G1` / `G11`) |
| B. Compare the placement pointer in the share test, and nothing else | one pointer per row | Every bake answers a fresh placement object, so every chunk holding a billboard stops sharing for no change at all - publishing cost grows with view-dependent content (`G10`) |
| C. Move the placement into `ResolvedRow`, so the serial covers it | one field, one serial | `ResolvedRow` is the fold's OUTPUT and the value descendants inherit; the placement is per-row INPUT the select reads from `own`, so `storeResolved`'s equality would compare a fact the fold never derived (`G1`), and every descendant's row would change when an ancestor's placement did |
| D. Give the placement an identity and compare the row's own FOLD group whole | The comparison becomes the definition of "the select would read the same facts": identity, links, epoch, `RowOwn::Fold`, serial. A per-view node answers the SAME value while its own facts are unchanged, so the comparison is meaningful and cheap | `Fold`'s defaulted equality adds one matrix value and two optionals per row per publish, and the parent class gains a rule: a placement value is stable per configuration |

## Decision

Option D.

**The value keeps its identity.** `AutoTransform` caches its placement and rebuilds it in `recompose()`; every mutator
of a fact the value carries (position, rotation, scale, pivot, mode, axis, normal, auto-scale, scale limits) goes
through `recompose()`, so an unchanged node answers the same object and a changed one answers a fresh value holding
the new facts. `Node::viewPlacement` states the contract, and `Node::announceContentChange`'s dependency list now names
per-view placements - the announcement is what re-reads the row; the identity is the second line of defence.

**The share test reads the input.** `makeSceneSnapshot` compares `current.own.fold == prior.own.fold` beside
`identity`, `links`, the epoch and `resolved_serial`. `RowOwn::Body` is deliberately not compared: a body change
rebuilds the table, and a value refresh disables sharing, so comparing its draw vectors per row would buy nothing and
cost a vector comparison on every publish.

**Cost.** The comparison runs only on the publication path (an edit), which already copies the chunks it touches; the
matrix is 16 doubles and the optionals are small, while a shared chunk saves a 32-row copy. The measurement case that
prints the publish figure (`SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect`) is the witness, and its shape
assertion (share every chunk but the edited one) is unchanged.

**Proof.** `SnapshotTest.APlacementThatMovedOutsideTheFoldIsNotShared` builds a bake by hand, swaps one row's
placement without moving the fold's answer (what a node that forgot to announce would look like) and requires the chunk
to be copied - red under the old test, which served the previous placement. `AutoTransformTest.
ThePlacementKeepsItsIdentityUntilItsOwnFactsChange` pins the other half: the same object while unchanged, a fresh one
after a fact moves, null under `NoRotation`.

## Consequences

- A chunk is shared only when the select would read the same row: the rule is now a property of the comparison rather
  than of every node author remembering an unstated part of the contract.
- A view-dependent node type added later inherits both halves: the doc says its placement must announce, and the share
  test catches a forgotten announcement that somehow reached the table.
- No API change beyond the documented contract; `ViewPlacement` stays immutable and node-free.
