# geometry tessellation of analytic solids

> **Status**: current
> **Last verified**: 2026-11-09
> **Scope**: the entry that turns the five analytic solids into vertex arrays (`Triangulation`), its quality parameter and result contract,
> and the model-side step that assembles it into a `TriangleMesh`.
> Not the value type itself and its analytic queries (see `geometry-solid.md`), not the model's modelling contract (see `geometry-parametric.md`), not B-rep tessellation (see `brep-operations.md`), not mesh storage and rendering (see `graphics-geometry.md`).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp`, `src/base/geom/src/SolidTessellation.cpp`,
> `src/base/models/sdk/vine/models/SolidTessellation.hpp`; `tests/test_geom_spatial/SolidTessellationTest.cpp`.
> **Backlog**: `../backlog/geometry.md` G5

## 1. Overview

### 1.1 Problem and background

`Primitive` is a modelling type: it carries identity, name, revision and dimensions, yet contains no vertex data at all (`geometry-parametric.md` 2.4).
When this entry was designed, the rendering boundary `graphics::geometryFromShape()` accepted meshes only, so an analytic solid could not be drawn; B-rep's `Tessellation` can turn a `BrepShape` into a mesh, but that route needs the OCCT backend, while the geometry of an analytic solid is pure mathematics.
The boundary now takes primitives through this chain (9.1).

Tessellation needs two things a value type cannot give: a traversal that lays out a mesh, and a set of vertex arrays.
The former goes into a free function, and the latter is just an ordinary value type whose precision follows the solid (a `float` / `double` pair).

### 1.2 Goals

- One free function turning the five analytic solids into a `Triangulation<T>` with positions, normals and indices, with no dependency on OCCT and none on the modelling module.
- The result is in the solid's own canonical local space, closed, with outward winding, and can be sent straight to rendering or collision.
- The subdivision level is an explicit parameter: predictable and testable.

### 1.3 Non-goals

- Not doing adaptive subdivision / curvature error control: there is no consumer that takes a mesh by error today (`G8`), and a fixed segment count is enough and predictable.
- Not doing texture coordinates: an analytic solid has no answer to "which parameterization", and a caller that needs them can derive them from positions.
- Not doing a double-precision mesh: `TriangleMesh` is float; `Triangulationd` exists, but the caller reduces precision explicitly when assembling it into a mesh (9.2).
- Not doing B-rep tessellation: that is `brep`'s job (`brep-operations.md`).
- Not doing the mesh: `TriangleMesh` comes from the model side's `meshFromTriangulation()`, and the geometry side hands over arrays only.

### 1.4 One-paragraph conclusion

**One stateless free function lays triangles analytically over the five analytic solids per kind: the box is six quads, the sphere / ellipsoid a latitude band plus two pole fans, the cylinder a side band plus two caps, the cone a side fan plus a base cap; the quality is the caller's fixed segment count, and the precision matches the solid.**

## 2. Design Points

### 2.1 A free function, not a member of `Solid<T>`

- **What it is**: `tessellateSolid()` is implemented in `geom`'s `src/`, one overload per kind (`BoxSolid<T>` … `EllipsoidSolid<T>`), returning `Triangulation<T>`.
- **Why**: `G2`, `G4`, `G6`, `G10` - the algorithm touches geometry values only (the five kinds / `Vector3<T>`) and `std::vector`, while `TriangleMesh` is a modelling type: putting it in the return type would be a geom → models back edge.
  Precision matching the other geometry values is the template parameter `T`, not two sets of entry names; when `T` follows the solid there is no implicit precision loss.
  The implementation stays in the `.cpp`, and both precisions are instantiated and exported explicitly in `Instantiations`' single TU (the same rule as `Aabb` / `Bvh`).
- **Boundary**: the algorithm sits on the **geometry side** (not on the model), the same side as `closestPoint()` / `intersectWith()`; the model takes part only through `visitPrimitive()` and assembles with `meshFromTriangulation()`.
  The parameter is the concrete kind rather than `const Solid<T>&`: laying out each kind needs its own extents; the caller hands the concrete subclass in with `visitPrimitive()`, so missing a new kind is a missing overload and the compile error still appears.

### 2.2 Quality is a fixed segment count

- **What it is**: `TessellationQuality{ around, along }`: `around` is the azimuth segment count of a surface of revolution, `along` the latitude segment count of the sphere / ellipsoid; the box ignores both.
  `around` is clamped to ≥3 and `along` to ≥2.
- **Why**: `G8`, `G4` - a fixed segment count has no hidden error criterion, and the caller knows how many triangles it gets; clamping makes "0 segments" a minimum legal shape rather than a degenerate mesh.
- **Boundary**: no adaptivity and no screen-space error - when one is needed the caller computes the segment count from the dimensions itself.

### 2.3 Exact per-vertex normals, so vertices are duplicated where needed

- **What it is**: every vertex carries an analytic normal - the face normal for the box, normalized `(x/a², y/b², z/c²)` for the sphere / ellipsoid, the lateral radial and the cap axial for the cylinder, the slanted lateral normal for the cone.
  Where the position is the same but the normal differs (box edges, between the cylinder's side and its caps, the cone apex per segment) each gets its own vertex.
- **Why**: `G4`, `G10` - sharing one vertex would allow only one normal, or would smooth away the hard edges; the normal is this mesh's main use (lighting), and exact normals are worth more than a minimal vertex set.
  An indexed mesh still saves "one vertex set per triangle".
- **Cost**: the vertex count is not the theoretical minimum; the box has 24 rather than 8, and the cylinder's cap ring and side ring are separate.

### 2.4 Closed solids and outward winding

- **What it is**: the five analytic bodies are solids, so the mesh is closed; seen from outside the triangles are counter-clockwise (right-hand rule), so back-face culling works directly.
- **Why**: `G4`, `G12` - winding is another way of stating the normal; closure gives "inside / outside" a meaning, and it is the premise of collision and normal validation.
- **Boundary**: the cone apex is duplicated per segment, and each vertex takes its segment's slanted normal; the normal at the apex is not unique in the first place, and this is the only correct treatment.

### 2.5 Only positions, normals and indices are written

- **What it is**: `Triangulation` fills the position / normal / index arrays only; texture coordinates and colours are not written.
- **Why**: `G8` - every channel of `Mesh` is optional, the model side leaves missing channels empty when assembling, and the rendering boundary has a defined behaviour for a missing channel; an analytic solid has no unique answer to texture parameterization, and writing one would decide on the caller's behalf.

### 2.6 No OCCT dependency, no caching

- **What it is**: every call returns a new value; the geometry on the `graphics` side is neither shared nor remembered.
- **Why**: `G2`, `G10` - `geometry` does not include OCCT; a cache needs an invalidation rule (model dimensions can be changed by setters and the revision changes), and today's call frequency does not justify introducing one.
  A value type has no revision, so a cache can only be done by the caller under its own invalidation rule.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Tessellation of the five analytic solids | `tessellateSolid(concrete kind, quality)`: one overload per kind, returning `Triangulation` (positions / normals / indices) | landed (T1–T5) |
| F5 | Model-side assembly | `meshFromTriangulation(Triangulation)`: moves the three arrays into `TriangleMesh` and computes the bounding box; an empty value returns an empty mesh | landed (T7) |
| F2 | Quality and clamping | `around ≥ 3`, `along ≥ 2`; the box ignores both | landed (T5) |
| F3 | Unusable solids | a false `isValid()` returns `nullptr` and does not throw | landed (T6) |
| F4 | Adaptive subdivision / texture coordinates | error-driven segment count and a UV channel | not done (`G8`) |

### 3.2 F1 Laying out the mesh

- **Behaviour**: dispatch per kind to the box's six quads, the sphere / ellipsoid's latitude band + two pole fans, the cylinder's side band + two cap fans, the cone's side fan + base cap fan.
- **Constraints** (C1–C3): space, closure and normals are in §5.
- **Acceptance** (T1–T4): the vertex count, triangle count and point-on-the-analytic-surface criteria of each kind.

### 3.3 F3 Unusable input

- **Behaviour**: a solid whose `isValid()` is false returns an empty value; the clamped minimum quality still produces a closed triangle set.
- **Requirements** (R2).
- **Acceptance** (T5, T6).

## 4. Public Interface

| Header | Contents |
|---|---|
| `geom/spatial/SolidTessellation.hpp` | `TessellationQuality`, the result value `Triangulation<T>` (`Triangulationf` / `Triangulationd`) and the five `tessellateSolid()` overloads |
| `models/SolidTessellation.hpp` | `meshFromTriangulation(Triangulationf)`: the model-side assembly, an empty value returns an empty mesh |

The full signatures and the meaning of each parameter are in the headers; mesh channel storage and sharing are in `graphics-geometry.md`.

## 5. Constraints, Requirements and Invariants

- **C1**: the result is in the solid's canonical local space (box / sphere / ellipsoid centred, cylinder / cone along +Z spanning `[-half_height, +half_height]`).
- **C2**: the mesh is closed - every geometric edge belongs to exactly two triangles (the same position is the same edge, even when a vertex was duplicated for its normal).
- **C3**: the normal is a unit vector pointing away from the interior of the convex analytic solid; the triangle winding agrees with the normal.
- **C4**: `around` / `along` are clamped at the entry, so an illegal value produces neither a degenerate mesh nor an out-of-range index.
- **R1**: no dependency on OCCT and none on the modelling module; no persistent state is allocated beyond the result arrays.
- **R2**: an unusable solid returns an empty value (`Triangulation::empty()`) and does not throw.
- **N1**: the box's triangle count is always 12; the sphere / ellipsoid is `2 · around · (along − 1)`; the cylinder is `4 · around`; the cone is `2 · around`.
- **N2**: the cone apex is duplicated per azimuth segment, and each vertex carries the slanted normal at that segment's midpoint.

## 6. Caveats and Traps

- **P1 Duplicate vertices are not a defect**. How it goes wrong: validating the mesh against "the vertex count must equal the theoretical minimum", and concluding the box's 24 vertices are wrong. How to detect: de-duplicating by position leaves only 8 corners. How to prevent: C2's closure criterion counts edges **by position**, not by index.
- **P2 The segment count does not adapt to size**. How it goes wrong: a long thin large cylinder uses the default segment count and its side shows visible creases. How to detect: doubling `around` improves the silhouette. How to prevent: choose `around` from the ratio of the target size to the segment count.
- **P3 The cone apex normal is not a single value**. How it goes wrong: assuming there is one "true" normal at the apex, and merging the per-segment copies. How to detect: after merging, the side lighting shows a ring of wrong highlights. How to prevent: keep one vertex per segment (N2).
- **P4 Local space is not world space**. How it goes wrong: sending mesh vertices to collision as world coordinates, ignoring the node transform. How to detect: the shape's position does not match the scene. How to prevent: C1; a world box multiplies by the node transform.
- **P5 Double-precision triangles do not fit into a mesh**. How it goes wrong: demanding a `TriangleMesh` straight from a `Triangulationd` and casually `static_cast`-ing the extents. How to detect: `meshFromTriangulation()` takes `Triangulationf` only, and it does not compile. How to prevent: accept one explicit float conversion, and do not scatter casts over the call sites.

## 7. Rejected Options

- **D1** Putting tessellation into `graphics`: rendering needs a mesh, but collision, bounding-box checks and export want one too; putting it in the rendering layer would make consumers of `base` depend on `viz` backwards (`G2`).
- **D2** Adding a virtual `tessellate()` to `Primitive`: the modelling type is dragged onto the algorithm surface, every subclass gets one more virtual, and the algorithm itself has nothing to do with identity / revision (`G1`, `G8`). See `../decisions/ADR-2026-11-05-solid-value-type.md`.
- **D3** Borrowing OCCT's `BRepPrimAPI` + `Tessellation` to produce the mesh: `geometry` would depend on a backend from then on, and a backend-less build would lose the whole capability; an analytic solid is pure mathematics and needs no backend (`G2`, `G10`).
- **D4** Adaptive subdivision: no consumer takes a mesh by error, and the error criterion itself needs curvature and a screen scale (`G8`).
- **D5** Making it a member of `Solid<T>`: `TriangleMesh` is float only, so the memberized result is `Solid<double>` losing precision implicitly (`G4`). See `geometry-solid.md` D4.
- **D6** Letting the geometry side return `TriangleMesh` directly, or return three channels of `intrusive_ptr<Buffer<T>>`: the former creates a back edge between geom and models; the latter costs the model side's `Mesh::setPositions()` (which takes `std::vector` only) one more copy, while no second consumer needs the Buffer form today (`G2`, `G8`, `G10`). See `../decisions/ADR-2026-11-09-solid-tessellation-in-geom.md`.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the box: six faces, canonical bounding box, normals | `aBoxBecomesSixClosedQuads` in `tests/test_geom_spatial/SolidTessellationTest.cpp` |
| T2 | the sphere / ellipsoid: the point set lies on the analytic surface, normals | `aSphereBecomesAClosedBandBetweenItsPoles`, `anEllipsoidUsesItsThreeSemiAxes` in the same file |
| T3 | the cylinder: side band and two caps, normal classification | `aCylinderHasASideBandAndTwoCaps` in the same file |
| T4 | the cone: slanted normal and base cap | `aConeHasASlantedSideAndABase` in the same file |
| T5 | quality clamping and closure | `theQualityIsClampedInsteadOfTrusted` in the same file, plus `expectClosed` in each case (every geometric edge belongs to exactly two triangles) |
| T6 | unusable solids | `anUnusableSolidAnswersAnEmptyValue` in the same file |
| T7 | the model reaches a mesh through the conversion | `aModelReachesTheMeshThroughItsSolid` in the same file |
| T8 | precision follows the solid | `thePrecisionOfTheSolidIsThePrecisionOfTheTriangles` in the same file (`BoxSolidd` → `Triangulationd`) |

## 9. Status and Evolution

### 9.1 Current status

- The tessellation of the five analytic solids lives in `geom`, with no dependency on OCCT and none on the modelling module; `brep`'s B-rep tessellation is a different route.
- The entry is five overloads (`BoxSolid<T>` … `EllipsoidSolid<T>`) returning `Triangulation<T>`; `float` and `double` are both instantiated and exported explicitly.
  The model side hands the concrete subclass in with `visitPrimitive()` and assembles it into a `TriangleMesh` with `meshFromTriangulation()` (`geometry-parametric.md` 4).
- `graphics::geometryFromShape()` goes through this chain (`Geometry.cpp`); the intermediate mesh is assembled on the model side, and the rendering side does not build one itself.

### 9.2 Known gaps / backlog

- Adaptive subdivision and texture coordinates are not done (F4, `G8`).
- The model mesh is float only: the caller reduces precision explicitly when a `Triangulationd` is to be assembled into a `TriangleMesh` (P5).

### 9.3 Compatibility / migration

- Migration: `tessellateSolid()` moved from `vn::models` to `vn::geom::spatial` and returns `Triangulation<T>` rather than `TriangleMesh`; `TessellationQuality` moved up with it, and precision became a template parameter.
  The model-side replacement is `meshFromTriangulation(tessellateSolid(shape.solid()))`, where `shape` comes from `visitPrimitive()` (see `../decisions/ADR-2026-11-09-solid-tessellation-in-geom.md`).
- The entry is overloaded per kind rather than taking a variant: the closed set is expressed by "five kinds + `visitPrimitive()`" (`../decisions/ADR-2026-11-09-solid-closed-set-without-variant.md`).

## 10. References

- `geometry-solid.md` (the family interface and the five kinds, canonical local space)
- `../decisions/ADR-2026-11-09-solid-tessellation-in-geom.md` (why the entry and the result value live in geom, why the result is a value rather than a mesh or a Buffer)
- `geometry-parametric.md` (the modelling type and where canonical space comes from)
- `graphics-geometry.md` (mesh channels, the boundary of `geometryFromShape`)
- `brep-operations.md` 1.3 (why "solid → mesh" belongs to `geometry` and not to `brep`)
- `../backlog/geometry.md` G5
