# ADR-2026-11-09: the shape is the tree's storage, not its transcription

> **Status**: Accepted
> **Date**: 2026-11-09
> **Scope**: `src/base/geom/sdk/vine/geom/spatial` (`Bvh.hpp`, `BvhTopology.hpp`, `BvhBuilder.hpp`,
> `TriangleBvh.hpp`, `detail/{BvhBuildJob,BvhBuilders,BvhRefit,BvhTopologyCheck}.hpp`),
> `src/base/models/sdk/vine/models/Mesh.hpp`, `src/robotics/io/src/BvhCache.hpp`,
> `tests/test_geom_spatial`, `.ai/design/geometry-bvh.md`

## Background

The review asked two questions: why `BvhTopology` is a file of its own and cannot be an inner class of `Bvh`; and whether there is a better structure than the current one.
The assessment confirmed that a separate file is right (layer order, precision independence, what the codec has to include), but found one real waste in the current shape:

- A tree node is `detail::BvhNode<T, Volume>{bounds, offset, count}` (AoS, 32 B);
- A node stored in a file is `BvhNodeTopology{offset, count}` (8 B);
- The two are **two transcriptions of one contract**, so the comment can only say "these two numbers mean what they mean in `BvhNode`".

Every `topology()` therefore copied the whole thing: for a 38M-triangle model that is about `≤2n` nodes × 8 B = 608 MB, plus 113M `order` entries × 4 B = 452 MB,
**about 1 GB per call**; the `.vbtp` write calls it once and the restore path copies the nodes one by one through `appendNode()`
into a build job, so one chain holds three copies.

This is the same kind of problem as the welding OOM just fixed (`perf(meshio)!: no vertex weld`): copying bytes that already exist in a file into another
representation.

## Decision

1. **`BvhTopology.hpp` keeps the vocabulary only**: `BvhNodeTopology`, `BvhTopology`, `BvhTopologyView`, `kBvhMaxDepth`,
   depending on the standard library alone. `RestoreTopology` (a build algorithm) moves into `BvhBuilder.hpp`, `refitNodes` moves into
   `detail/BvhRefit.hpp`, and the restore check moves into `detail/BvhTopologyCheck.hpp`.
2. **The shape becomes the tree's storage**: `Bvh` holds a `BvhTopology shape_` (`nodes` + `order` + `depth`) and a parallel
   `std::vector<VolumeType> node_bounds_`. `detail::BvhNode` is deleted, and `BvhNodeTopology` is the node.
3. **Two restore entries, one check**: with only a view, the path is `BvhBuilder::buildWith(tree, RestoreTopology{shape})`
   (a build algorithm, the same gate as SAH / median); when the caller owns the value, the path is `Bvh::restoreTopology(BvhTopology)`
   (**moving** the arrays in). Both read `detail::installableHeight()` and both grow through `detail::refitNodes()`.
4. **The save side is zero copy**: `Bvh::topologyView()` borrows the tree's own arrays and `.vbtp` encodes them directly; `topology()` stays as the
   "I want an owned value" entry point.

## Rationale

- **G10, cost the thing first**: the `1 GB`-per-call copy disappears. This is not "more elegant", it is a measured order of magnitude in memory and time.
- **G1 / G6, one contract, one definition**: there is one node, `BvhNodeTopology`; the tree and the file hold the same layout, and `validate()`,
  traversal and the codec read the same set of numbers.
- **G4, "does not change the topology" becomes a fact of the signature**: `refitNodes()` takes the node-bounds array as its only writable parameter, so it cannot write the combinatorics;
  `C5` turns from a comment into a compile-time guarantee.
- **The layer order does not reverse**: `Bvh.hpp` depends on the shape header, and the shape header does not know the tree. An inner class would drag the whole tree and the build kernels
  into the `.vbtp` codec for one POD, and would give `Bvh<float>` / `Bvh<double>` two shape types with the same layout that cannot be assigned to each other.
- **G2, the shape header becomes a leaf**: the `.vbtp` codec no longer transitively includes the build kernels.

## Rejected alternatives

| Alternative | Rejection reason |
|---|---|
| `BvhTopology` as an inner class of `Bvh` | A precision-independent value becomes templated; the layer order reverses (`Bvh.hpp` depends on the shape header); the codec is forced to include `Bvh.hpp` plus the build kernels (`G2`, `G10`). |
| Keep the transcription and only make `topology()` return a view | Not implementable: AoS nodes step by 32 B, so no contiguous `BvhNodeTopology` array can be produced. |
| Make `BvhTopologyView` point at `BvhNode<T, Volume>` directly | The view would depend on precision again, the file format would be tied to the tree's precision, and the caches of `Bvh<float>`/`Bvh<double>` would no longer be interchangeable. |
| Give `offset` / `count` `isLeaf()` / `rightChild()` and push the contract into the type | `BvhNodeTopology` is deliberately kept a plain value (trivially copyable, read and written directly by the codec); overturning a written design point only to read one comment fewer is a bad trade. |
| Keep `detail::BvhNode` as a "hot-path-only node" | Two contracts from one source, which means freezing the `1 GB` copy; the cost of one more read per node during traversal is far smaller than that. |

## Consequences

- Traversal loads two things per node (`shape_.nodes[i]` + `node_bounds_[i]`), and `refit()` scans only the bounds array. A shape change on the query path
  must be compared with `scripts/callgrind_diff.py` rather than reasoned about (the first landing is recorded in 2.4 of `geometry-bvh.md`).
- `Bvh<T>`'s layout changes: it is an explicitly instantiated DLL class, so this is an **ABI break**, and every consumer is rebuilt.
- `Mesh::restoreBvh` gains a by-value `BvhTopology` overload; `restoreTriangleBvh` gets the same two overloads. The old view entry point stays.
- Header count: `spatial/detail/` goes from 5 to 7 (new `BvhRefit.hpp`, `BvhTopologyCheck.hpp`).
- The `VolumeT` alias is renamed `VolumeType` everywhere (three places: `Bvh`, `BuildJob`, `VolumeTraits<Aabb<T>>`),
  one concept, one spelling.

## Verification

- `test_geom_spatial`: all 256 cases green (new `BvhBuildTest.AnOwnedTopologyIsAdoptedByTheTreeItself`,
  `ATopologyViewBorrowsTheTreesOwnArrays`, `TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied`).
- Full `ctest` 28/29 (the `test_vsg` strip/fan case was already failing on this machine and is unrelated).
- `check_geom_layering.py`, `check_include_hygiene.py`, `check_export_annotations.py`,
  `check_public_docs.py`, `check_doc_symbols.py`, `check_ai_docs.py --strict` and
  `check_design_docs.py` all report 0 findings.
