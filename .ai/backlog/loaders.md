# loaders - open backlog

> Only items that are **not done and carry a trigger condition**; a finished item is deleted, and so is one that is
> decided against (the reason goes to an ADR).
> Current design: `../design/meshio-brepio-design.md`.

## 1. Where point-cloud I/O lives and how it is entered

- **Trigger**: a consumer that has to read or write point-cloud files appears (PCD, LAS/LAZ, E57, PTS/PTX, XYZ, or a
  PLY with vertices only), or the model viewer has to display such a file.
- **What**: open a new `pointcloudio` under `src/loaders/`, **not inside `meshio`** - that module's name says mesh, and
  letting it return a `PointCloud` violates `G4`; it is the same reason as D6 in `graphics-geometry.md` (refusing to
  make `PointCloud` a subclass of `Mesh`), only one layer up.
  - The public face has the same shape as `meshio` / `brepio` / `imageio`: a table-driven format enum, `Result` /
    `IoError`, state on the instance, no singleton.
  - Overlapping `.ply` extensions are not a conflict: the repository has no global loader registry (`defaultInstance()`
    is gone), and each module's `supportedExtensions()` only describes "what I can read". `meshio` keeps reading `.ply`
    as faces; `pointcloudio` declares `.ply` too only when a "vertices-only PLY" is really needed, and the caller
    chooses the loader by "do I want a mesh or a point cloud". A unified "just read some file" entry would need a
    content-sniffing dispatch layer - do that when it is needed (`G8`).
  - In no case does it go into `geometry` (which does not touch IO) or into `viz` (rendering is a consumer): the data is
    `geom::spatial::PointCloud` and the files live in loaders.
  - assimp (v5.4.3 in this repository) has no point-cloud importer: PLY is the only "vertices only" entry, and it reads
    back as a mesh without faces; PCD, LAS/LAZ, E57, PTS/PTX and XYZ are all outside its format family. LAS/LAZ needs
    LASzip or PDAL, E57 needs libE57Format, PCD/PTS/XYZ can be parsed by hand, and vendor raw formats (FLS/RXP/ZFS) have
    no public read path.
- **The blocking design question**: `Model::Entry` holds a `Mesh` only, so a point cloud does not fit. The default is a
  separate entry returning `Result<intrusive_ptr<PointCloud>>` (as `brepio` returns a `BrepShape` directly); adding an
  entry type to `Model` waits for a real source where one file holds both meshes and point clouds (`G8`).
- **Acceptance**: the new module's tests (`test_pointcloudio`) cover "PCD / a vertices-only PLY → `PointCloud`, with the
  point count and the normal/colour channels matching the file".
