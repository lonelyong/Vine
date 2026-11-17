# the Robotics IO module: XML serialization

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the XML serialization of devices and workcells, VFS packaging, and what scene serialization relies on.
> **Sources**: `src/robotics/io/**`, `tests/test_robotics_io`.
> **Backlog**: `../backlog/`

## 1. Overview

### 1.1 Problem and background

The `Device` / `MotionDevice` / `Scanner` / `Workcell` of `vn::robotics::workcell` need XML serialization and deserialization,
and a whole scene (devices, meshes) has to be packed into a single package without a temporary directory. URDF's `<robot>` semantics are not enough (multiple DoF, scanners,
material libraries, packages inside packages), so the format is the in-house `<device>` / `<workcell>`.

### 1.2 Goals

- `RoboticsIO` as its own module: `XmlIOBase` / `DeviceIO` / `WorkcellIO`.
- XML covers devices, workcells, materials, shapes, joints/DoF, and a scanner's cameras / projectors.
- A location is `vn::io::Vfs + a virtual path`; a package is `.vdevpkg` / `.vwspkg` (zip).
- A mesh is a native bin plus an XML description; a shared mesh is de-duplicated.

### 1.3 Non-goals

- Not the URDF format; no BRep/STEP (only the reserved `<brep>`).
- No alternative format such as HDF5/CBOR/Zarr.
- No temporary directory; a package is read and written in memory / on a VFS.
- A material is not written inline in a visual (no `exportMaterial` for an inline material).

### 1.4 One-paragraph conclusion

`RoboticsIO` is a stateless XML serialization layer: `XmlIOBase` provides the parse / export primitives and `DeviceIO`/`WorkcellIO` compose them;
a location is `Vfs + path`, a package is a zip, a mesh is a native bin, a device definition is reusable and a package can nest.

## 2. Design Points

### 2.1 A stateless `XmlIOBase`

- `XmlIOBase` / `DeviceIO` / `WorkcellIO` have no class-level mutable state; a per-operation `ParseContext`/`ExportContext`
  carries the unit scale, the active `Vfs`, `vfs_dir`, the workcell pointer and the warning buffer.
- An instance is re-entrant and safe to reuse (threads included, since each operation brings its own context).
- A load failure throws `std::runtime_error`; a non-fatal problem goes into `ctx.msgs`.

### 2.2 The XML format

- The device root is `<device name kind version>`; the workcell root is `<workcell name version>`.
- A pose is a quaternion `quat="x y z w"` (`Isometry3d`'s native quaternion, a lossless round trip; no RPY).
- A length unit is written as the metadata `length_unit`, and load / export scale by `LoadOptions`/`SaveOptions`.
- A joint `type` = `fixed/revolute/prismatic/planar`; one `<dof>` per DoF (planar = 3).
- Shapes: box/sphere/cylinder/cone/ellipsoid/triangle_mesh/indexed_triangle_mesh; `<brep>` is reserved.
- A workcell holds `<obj type="device|rigid_object">`; a device reference `file` may be `.vdev` or `.vdevpkg`.

### 2.3 VFS and packages

- A location is not a bare path but `vn::io::Vfs + a virtual path`; `DirectoryVfs` for debugging, `ZipArchive` for a package.
- `loadPkg(path)` reads lazily through `open(..., ReadOnly)`; `savePkg(cell, Vfs&, options)` writes with no source tree + `saveAs`.
- A package inside a package: the `.vdevpkg` entry of a scene package is read into bytes and opened as a nested `ZipArchive`.
- Mesh storage: `geoms/*.bin` (positions/normals/indices) plus an XML `indexed_triangle_mesh` reference;
  a shared mesh is written once (`mesh_paths` de-duplication).
- Triangle-tree cache: a `.vbtp` next to the mesh file (`geoms/mesh3.vmesh` → `geoms/mesh3.vbtp`), storing the topology and the primitive order only,
  with the boxes recomputed from the current vertices; a missing, stale or damaged file counts as “no cache”, and neither the XML nor the manifest mentions it.

### 2.4 Data mapping

| Vine model | XML |
| --- | --- |
| `DeviceMetadata` | `<metadata>` (with create/modified time, length_unit, iksolver) |
| `DeviceData.materials` | a named material library under `<material name>` |
| `DeviceData.links` | `<link>`（visual + collision） |
| `Joint` / `DofInfo` | `<joint>` + several `<dof>` |
| `DeviceKind` | `<device kind>` (Scanner → Scanner, everything else → MotionDevice) |
| `Scanner::Camera/Projector` | `<camera>` / `<projector>` |
| `SceneObject` / `RigidObject` | `<obj type>` |
| `Workcell::name` | `<workcell name>` |

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | device XML | `DeviceIO::loadXml/saveXml`, with link/joint/DoF/material/shape | landed |
| F2 | workcell XML | `WorkcellIO::loadXml/saveXml`, with obj/children/parent_frame | landed |
| F3 | data mapping | the model ↔ XML fields correspond one to one | landed |
| F4 | VFS / packaging | `loadPkg/savePkg`, `.vdevpkg`/`.vwspkg`, a package inside a package | landed |
| F5 | mesh storage | a native bin + an XML reference; shared meshes de-duplicated | landed |
| F6 | BRep/STEP | `<brep>` is reserved, not implemented here | not done |
| F7 | triangle-tree cache | a `.vbtp` travels with the package: written when a tree exists at export, restored transparently at load; a bad file counts as none | landed |

### 3.1.1 F1 device XML

- **Behaviour**: `DeviceIO::loadXml(path|string|Vfs, dir)` / `saveXml(...)`; parses metadata, materials, links,
  joints、DoF、camera/projector、parameters。
- **Constraints** (C1): the root is `<device>`; a pose is a quaternion; a length is scaled by the metadata `length_unit`.
- **Requirements** (R1): `DeviceKind` picks the factory (Scanner/MotionDevice); a material is referenced by name and never written inline in a visual.
- **Acceptance** (T1): the device XML cases in `tests/test_robotics_io`.
- **Caveats** (N1): the origin of a joint and of a DoF are `Frame::fixedTransform` and `DofInfo.origin` — not the same thing.

### 3.1.2 F2 workcell XML

- **Behaviour**: `WorkcellIO::loadXml/saveXml`; an `<obj>` may nest `<children>`, and `parent_frame` attaches to a device frame.
- **Constraints** (C2): a device reference `file` may be `.vdev` or `.vdevpkg`; one `file` may have several instances while the definition is written once.
- **Requirements** (R2): `home`/`q`/`resolutions` are instance attributes and do not enter the device definition.
- **Acceptance** (T2): the workcell / suite cases in `tests/test_robotics_io`.
- **Caveats** (N2): a `.vdevpkg` is a package inside a package, dispatched by extension.

### 3.1.3 F3 data mapping

- **Behaviour**: `DeviceMetadata`, `DeviceData`, `Link`, `Joint`, `DofInfo`, `DeviceKind`, `Scanner`,
  `SceneObject`, `RigidObject` and `Workcell` correspond field by field to XML.
- **Constraints** (C3): the mapping table is the contract; a new field needs a default or a version rule.
- **Requirements** (R3): a quaternion round-trips losslessly; `Q2Str` separates with spaces.
- **Acceptance** (T3): a round trip compared field by field.
- **Caveats** (N3): `iksolver` is a metadata string (None/Pieper/Jacobian).

### 3.1.4 F4 VFS / packaging

- **Behaviour**: `loadPkg(path)` / `savePkg(cell, Vfs&, options)`; a package is a zip and a mesh is a bin.
- **Constraints** (C4): no temporary directory; `ZipArchive` reads lazily and `saveAs` needs no source tree.
- **Requirements** (R4): a nested package is read out of the scene package into bytes and opened as a `ZipArchive`; a shared mesh is written once.
- **Acceptance** (T4): `PkgIOTest` (8 cases).
- **Caveats** (N4): a large package avoids `toBytes()`/`saveAs(ostream&)` (peak = the whole package twice).

### 3.1.5 F5 mesh storage

- **Behaviour**: `indexed_triangle_mesh` references `geoms/*.bin` (positions/normals/indices); in plain XML it is a relative path.
- **Constraints** (C5): the bin is native binary (not base64); `vertex_count`/`triangle_count` match the bin.
- **Requirements** (R5): a shared mesh is de-duplicated; reusing a device instance does not rewrite its definition.
- **Acceptance** (T5): the mesh round-trip cases inside a package.
- **Caveats** (N5): a bin path inside a package is a virtual path, not a disk path.

### 3.1.6 F6 BRep/STEP

- **Behaviour**: `<brep>` is reserved; not implemented here.
- **Constraints** (C6): a `<brep>` in the file is reported as unsupported, never silently taken for an empty shape.
- **Requirements** (R6): a future backend keeps the XML shape compatible.
- **Acceptance** (T6): none (not implemented).
- **Caveats** (N6): BRep depends on OCCT and belongs to a later stage.

### 3.1.7 F7 triangle-tree cache

- **Behaviour**: after writing the mesh file, `writeMeshFile()` writes `Bvh::topology()` (one value: the nodes, the
  primitive order and the height) into a same-named `.vbtp` when `mesh->hasBvh()`; `parseGeometry()` reads a same-named `.vbtp` after reading the mesh and calls
  `Mesh::restoreBvh(topology.view())`。
- **Layering**: the implementation has two layers only because they are two different contracts:
  - `BvhCacheFormat.hpp`: **bytes**. The header layout, `encodeBvhCache(BvhTopologyView, bytes)`,
    `decodeBvhCache(bytes, BvhTopology&)`, `bvhCacheByteCount()`. It knows neither `Vfs` nor `Mesh` — it is only about how
    one shape becomes bytes.
  - `BvhCache.hpp`: **policy**. The `bvhCachePath()` naming rule and the three VFS operations
    `writeBvhCache` / `readBvhCache` / `restoreBvhCache`.
  - One value crosses between the two: `vn::geom::spatial::BvhTopology` (owning) and its borrowed
    form `BvhTopologyView` (`geometry-bvh.md` 2.15), so no extra array copy sits between encode, decode and restore.
- **Format**: a 24-byte header (magic `VBTP`, `u16` version, `u16` flags, `u32` triangle count, `u32` node count, `u32` depth, `u32` reserved) plus
  `{u32 offset, u32 count}` per node plus one `u32` order per triangle, all little-endian.
- **Constraints** (C7): every item in the file is validated: magic/version/flags, byte count = header + nodes + order, triangle count = the mesh's triangle count,
  node count ≤ `2 × triangle count`, height ≤ `kBvhMaxDepth`; `RestoreTopology` then checks the permutation and the leaf ranges. Any mismatch counts as “no cache”:
  no error, no effect on loading the mesh.
- **Requirements** (R7): the cache may be deleted at any time and deleting it costs one tree build; a package with a cache and one without are readable both ways.
- **Acceptance** (T7): `PkgIOTest.APreparedTriangleTreeTravelsWithThePackage`, `PkgIOTest.AMeshWithNoPreparedTreeWritesNoCache`,
  `DeviceIOTest.ADamagedTreeCacheIsIgnoredAndAValidOneIsRestored`。
- **Caveats** (N7): only the topology is stored, not the boxes (see `geometry-bvh.md` 2.13); export does **not** build a tree in order to write the cache — that would change
  the `const` export path, so a caller that wants a tree calls `buildBvh()` before exporting.
- **Caveats** (N8): the format and the cache policy are separate files not to add a header, but so that “byte layout” can be tested against a `BvhTopology` alone and
  evolve with it alone (`G5`: a layout change never touches the VFS or the model layer; `G2`: the format never depends back out of `robotics/io`).

## 4. Public Interface

### 4.1 `DeviceIO`

| Interface | Contract | Notes |
| --- | --- | --- |
| `loadXml(path, LoadOptions)` | loads a device from a file | a failure throws `runtime_error` |
| `loadXml(string, ...)` / `loadXml(Vfs&, dir, ...)` | loads from a string / a VFS | a location is a VFS + path |
| `saveXml(...)` | exports a device | units follow `SaveOptions` |
| `LoadOptions` / `SaveOptions` | `source_len_unit` / `target_len_unit` |  default mm ↔ m |

### 4.2 `WorkcellIO`

| Interface | Contract | Notes |
| --- | --- | --- |
| `loadXml(path|Vfs, ...)` | loads a workcell | a device reference is `.vdev`/`.vdevpkg` |
| `saveXml(...)` | exports a workcell | a device definition is written once |
| `loadPkg(path)` / `savePkg(cell, Vfs&, options)` | packaging | zip, no temporary directory |

### 4.3 `XmlIOBase`

| Interface | Contract | Notes |
| --- | --- | --- |
| `parsePose/exportPose` | a quaternion pose | lossless |
| `parseGeometry/exportGeometry` | shapes | box/sphere/.../mesh |
| `parseMaterial` | a material-library entry | referenced by name |
| `parseLink/exportLink`, `parseJoint/exportJoint` | link / joint | shared by device and workcell |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | the root is `<device>` / `<workcell>`; a pose is a quaternion; a length is scaled by the metadata. |
| C2 | a device reference is `.vdev`/`.vdevpkg`; one file may have several instances while its definition is written once. |
| C3 | the data mapping table is the contract; a new field has a default or a version rule. |
| C4 | no temporary directory; a package reads lazily and `saveAs` needs no source tree. |
| C5 | a mesh bin is native binary; the counts match the bin. |
| C6 | an unimplemented `<brep>` is reported as unsupported. |
| C7 | every item of a `.vbtp` is validated; any mismatch degrades to “no cache” without affecting the mesh load. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | `DeviceKind` picks the factory; a material is referenced by name. |
| R2 | instance attributes do not enter the device definition. |
| R3 | a quaternion is lossless; `Q2Str` separates with spaces. |
| R4 | a nested package is dispatched by extension; a shared mesh is written once. |
| R5 | a shared mesh is de-duplicated; a reused device instance does not rewrite its definition. |
| R6 | a future BRep backend keeps the XML compatible. |
| R7 | the tree cache may be deleted at any time; a package with and without a cache are readable both ways, and the mesh file itself never changes because of the cache. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | `XmlIOBase`/`DeviceIO`/`WorkcellIO` have no class-level mutable state and are re-entrant. |
| N2 | a location is `Vfs + a virtual path`, not a bare path. |
| N3 | a device definition is written once; instance differences live in `<obj>` attributes. |
| N4 | a load failure throws `runtime_error`; a non-fatal problem goes into `msgs`. |

## 6. Caveats and Traps

### 6.1 The origin of a joint vs of a DoF

- How it fails: `<joint><origin>` is taken for the DoF's origin.
- How to detect it: a joint origin is `Frame::fixedTransform`; a dof origin is `DofInfo.origin`.
- How to prevent it: the two `<origin>` elements in the XML sit at different levels.

### 6.2 Unit scaling

- How it fails: a file in mm and memory in m are mixed and the model is 1000× too big.
- How to detect it: the metadata `length_unit` disagrees with `LoadOptions`/`SaveOptions`.
- How to prevent it: load and export both state the source / target unit; the default is mm ↔ m.

### 6.3 A package inside a package

- How it fails: a `.vdevpkg` is parsed as ordinary XML.
- How to detect it: dispatch by extension; the scene package's bytes are read out and opened as a nested `ZipArchive`.
- How to prevent it: dispatch by extension, never guess from the content.

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | use the URDF format | `<robot>` semantics are not enough (multiple DoF, scanners, material libraries). | — |
| D2 | the `IMemoryVfs` / `ZipMemoryVfs` pair of VFS types | `vn::io::Vfs` / `ZipArchive` already covers both. | — |
| D3 | HDF5/CBOR/Zarr | the ecosystem, dependencies and toolchain do not match; zip+XML is enough. | — |
| D4 | pack via a temporary directory | slow, dirty, concurrency problems; memory / VFS packs directly. | — |
| D5 | an inline material (`exportMaterial`) | a material belongs in the named library and a visual references it by name. | — |
| D6 | a base64-embedded mesh | a bin inside the package is smaller, streamable and de-duplicable. | — |
| D7 | put the triangle-tree cache into `MeshBin`'s flags | an unknown flag bit makes an older reader reject the whole file, and `byteCount()` would have to change too — a cache that may be dropped should not have that power; it became a separate `.vbtp` beside the mesh file (F7). | — |
| D8 | build the tree during export just to write the cache | the export path holds a `const Shape&` and building a tree is a write; by default only a tree that already exists is written, so a caller that wants one calls `buildBvh()` before exporting. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | a device XML round trip | `tests/test_robotics_io` |
| T2 | a workcell / nested obj | same |
| T3 | field mapping / quaternion | same |
| T4 | a package / a package inside a package / a mesh | `PkgIOTest` (8 cases) |
| T5 | shared-mesh de-duplication | same |
| T6 | BRep not implemented | reported as unsupported |
| T7 | the triangle-tree cache: a package with a cache round-trips with the tree; no tree writes no cache; a truncated, malformed, wrong-count or non-permutation order is ignored | `PkgIOTest`, `DeviceIOTest.ADamagedTreeCacheIsIgnoredAndAValidOneIsRestored` |

## 9. Status and Evolution

### 9.1 Current Status

The VFS infrastructure, scene serialization and packaging are implemented; `tests/test_robotics_io` passes.

### 9.2 Known Gaps and Backlog

- BRep/STEP is not implemented (`<brep>` is reserved).
- a mesh is a triangle mesh with normals / indices only; BRep waits for OCCT.
- the model changes scene serialization relies on are in §6; the WORKCELL pointer / frame lookup is still in the context.

### 9.3 Compatibility and Migration

There is no `IMemoryVfs`/`ZipMemoryVfs`/`ZipVfs::openZip`; new code uses `vn::io::Vfs` + `ZipArchive`.
`XmlIOBase` is stateless and an instance is reusable.

## 10. References

### 10.1 Code

- `src/robotics/io/sdk/vine/robotics/io/XmlIOBase.hpp`
- `src/robotics/io/sdk/vine/robotics/io/DeviceIO.hpp`
- `src/robotics/io/sdk/vine/robotics/io/WorkcellIO.hpp`
- `src/robotics/io/src/IoUtils.hpp`
- `tests/test_robotics_io`

### 10.2 Documents and ADRs

- `iobase-vfs-design.md`
- `robotics-proximity-design.md`
- `../backlog/`

### 10.3 External Sources

- `VDeviceIO` / `VWorkcellIO` / `VUrdfIOBase` of VMR (external repository).
- tinyxml2 10.0.0。
