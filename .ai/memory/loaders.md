# loaders/ layer module index

> meshio / brepio are covered here; `imageio` is in `.ai/memory/imageio.md`.
> Current design: `../design/meshio-brepio-design.md`.

## Three-layer structure and invariants

```
base/*        ← data/algorithms (geometry holds Mesh/BrepShape, imaging holds Image)
loaders/*     ← third-party libraries read data out of files and write it back
```

**Invariant**: the data modules (`geometry` / `imaging`) **depend on no third-party library**; third-party
libraries live only under `loaders/`, linked `PRIVATE` and statically baked into each DLL. Consumers
therefore never see them and are never forced to build them.

## Public surface (redesigned 2026-10-27)

- The two modules have the same shape: a `MeshFormat` / `BrepFormat` enum + `formatFromPath()` /
  `formatName()` (meshio also has `formatInfo()`); reads return `vn::io::Result<T>`, writes return
  `vn::io::IoError`; the classes are only `Loader` (options + cache) and `Exporter` (options).
- **The format table is the only vocabulary**: `supportedExtensions()` is generated from the enum; an
  extension outside the table makes `load()` return `Unsupported` directly (assimp's registry is no longer
  passed through; `.3ds`/`.x`/`.x3d` need a row in the table). `exportAs` no longer takes a writer name
  (`objnomtl`/`collada`/`gltf2` are all folded into the enum).
- **No singleton**: `defaultInstance()` is gone; options are given at construction and read-only, the cache
  belongs to the instance and is **not thread-safe**.
- meshio has six export entries: `exportAs(mesh|model, format, path|stream)` + `createSource(mesh|model, format)`.
  - Flattening Model → STL/PLY is done by **assimp's exporter registration** (`stl`/`ply` force
    `aiProcess_PreTransformVertices` and copy the scene first); the OBJ exporter accumulates the node
    matrices itself while writing vertices. We only build a faithful aiScene; **do not flatten again on our
    side**.
  - A format with `streamable == false` (`.gltf` needs its `.bin`, `.3mf` goes through a path) returns
    `Unsupported` for a stream/source target; OBJ Model → stream drops the `.mtl` and keeps geometry and names.
- `MeshSource` and `BinStlSource` / `AsciiStlSource` / `ObjSource` are gone: `createSource()` is the only lazy
  entry, its content is produced by `exportAs(stream)` (**the whole thing in memory**, generated on first
  pull, `rewind()` replays it).
- brepio: `find_package(OpenCASCADE CONFIG QUIET)` links optionally (PRIVATE: `TKDESTEP` `TKDEIGES` `TKXSBase` `TKBRep`
  `TKTopAlgo` `TKernel` `TKMath`) and defines `VN_BREPIO_HAS_OCCT` only when found; in a build without it
  `supportedExtensions()` is empty, reads and writes return `Unsupported`, and `formatFromPath()` still answers.
  OCCT 8's stream read is `ReadStream(fileName, stream)` (no single-argument form); the path entry goes through
  `ReadFile`; `BRepTools`' Read/Write return `void`; `BRepBuilderAPI_*::Shape()` is **non-const**, so a constructor
  cannot be declared const.
- **Two `vn_link_opencascade` traps (fixed 2026-11-04)**: vcpkg's `OpenCASCADEConfig.cmake` only sets variables
  (`OpenCASCADE_INCLUDE_DIR` / `OpenCASCADE_LIBRARY_DIR` / `OpenCASCADE_BINARY_DIR`), and the directory-scoped
  effect and imported targets belong only to **the directory that ran find_package**, while
  `VineOpenCASCADE.cmake` has `include_guard(GLOBAL)` and runs once ⇒ the second module (`brepio`) got no
  include path and reported `BRepTools.hxx` not found. All three directories are now promoted to cache
  variables and attached to the target by `vn_link_opencascade`; `TK*.dll` is also copied to the output
  directory POST_BUILD (vcpkg's applocal only runs when configuring with its toolchain). A `check_translations`
  failure only means this machine has no `lupdate`; it is unrelated to the code.
- `BrepShape` holds a `std::shared_ptr<const TopoDS_Shape>`: `setShape()` takes ownership and `shape()` borrows.
- imageio (`src/loaders/imageio`): **stateless free functions** (`decodeImage` / `loadImage` / `encodeImage` /
  `saveImage`), reads as `Result` and writes as `IoError`, the same vocabulary as the two modules above;
  capabilities come from one `ImageFormatInfo` table (`formatInfo()` / `supportedExtensions()` both read it;
  JPEG is read-only = `can_write == false`). **There are no Loader/Exporter classes**: no options and no cache,
  so copying the shape would only add a lifetime layer (`D1` in `../design/imageio-design.md`). stb stays
  PRIVATE and the public surface gains only `IOBase` (`Result` / `IoError`).

## Current state and traps

- **A new `src/**/*.cpp` needs `cmake -S . -B build`** (`vn_add_library`'s glob has no `CONFIGURE_DEPENDS`);
  a new test `.cpp` uses `CONFIGURE_DEPENDS` and takes effect automatically.
- The path entry reads the file twice (once for the fingerprint, once to parse); the stream entry first
  `drain`s the stream into bytes, so it hits the same cache as the path entry.
- Failure classification is exactly those four: `Unsupported` (extension outside the table / no backend /
  non-streamable target), `NotFound`, `IoFailure`, `InvalidData`; `Result`'s `*` / `->` are UB on failure, so
  check `ok()` first.
- Two assimp post-process lists: `loadMerged()` carries `aiProcess_PreTransformVertices`, `load()` **must
  never** carry it (it deletes the node graph).
- **No vertex welding**: `aiProcess_JoinIdenticalVertices` is in neither post-process list and has no switch.
  A CAD-exported part has its own vertices per face (per-face colour + per-face normal), so coincident corners
  are never equal and welding cannot merge anything anyway — all that is left is the cost of "one hash-table
  entry per corner": a 1.8 GB / 37.7M-triangle STL (113M corners) with it on was OOM-killed at 31 GB, while
  without welding it read in 12.5 GiB / 77 s. The cost is that the triangle soup keeps 3 corners per triangle;
  the reading stays the same. Normals are still filled in by `aiProcess_GenNormals` (next bullet).
- Normals: both lists carry `aiProcess_GenNormals` (it fills only a missing channel and never overwrites what
  the file gives); the cost is that per-face normals split vertices (`JoinIdenticalVertices` therefore merges
  less), and the gain is that "the file gave no normals" never yields a mesh without normals.
- **Normals the file states are not rewritten**: `aiProcess_FixInfacingNormals`, like welding, is in neither
  list and has no switch — it heuristically decides "this bunch of normals points inward" and flips it, which
  replaces the file's fact with a guess; if the file points inward, hand it over pointing inward and let
  whoever needs otherwise detect it. Both excluded passes stay as comments at the end of the lists
  (`MeshLoader.cpp`'s `kLoadFlags` / `kModelFlags`), so reading the list shows why.
- Matrices: assimp's `aiMatrix4x4` and `vn::math::Matrix4x4` both use the column-vector convention with the
  translation in the 4th column, so the 16 elements correspond directly and are **not transposed**.
- The material type is chosen by assimp's generic keys: `$mat.metallicFactor|roughnessFactor` → `PbrMaterial`;
  `$mat.opacity < 1` also → Pbr (only it can hold transparency); `$clr.specular`/`$mat.shininess` →
  `PhongMaterial`; only `$clr.diffuse` → `ColorMaterial`.
- **A default material the importer invented is not a material**: `AI_DEFAULT_MATERIAL_NAME` (used by assimp
  for STL; white diffuse + **white specular**) answers empty in `makeMaterial()` — otherwise the specular folds
  into metallic 1, and `builtin_forward.frag` multiplies the ambient term by `base * (1 - metallic)`, so the
  model is all black in a scene without an environment map (visible only when selected, which swaps in the
  highlight material). A file that really wrote `Kd 0 0 0` still yields a black model; that is the author's
  intent.
- robotics/io has its own `StlMeshSource` (it does not link assimp; see `MeshBin.hpp`), deliberately
  duplicating meshio's write: merging would put assimp on robotics for one STL writer.
- An iobase trap (a real defect, fixed): libzip does not support seeking in **compressed** entries, and one
  refused seek poisons the reader — `zip_file_is_seekable()` branches, and an unseekable entry is reopened
  with `zip_fopen_index` (see `../design/iobase-vfs-design.md`).
- **OCCT runtime must not go into `build/bin`**: do not copy the DLLs; just have
  `F:/vcpkg/installed/x64-windows/bin` on `PATH`. A vcpkg-toolchain build copies them via applocal; a
  hand-written glob copy is the wrong approach (it once dumped 50 `TK*.dll` into bin).
- **Manifest mode and the global install cannot be combined**: the manifest was renamed `_vcpkg.json` (CMake /
  vcpkg only recognise `vcpkg.json`, so renaming it turns manifest mode off), and the dependency declarations
  stay inside for CI; this machine's `CMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake` is written
  in `.vscode/settings.json`'s `cmake.configureSettings` → classic mode, using the global install under
  `VCPKG_INSTALLED_DIR` (default `F:/vcpkg/installed`), and applocal copies the DLLs to the output directory,
  so no manual copy is needed.
  Why it cannot be called `vcpkg.json` again: it would re-resolve against `builtin-baseline` (opencascade
  resolves to 8.0.1#0) while the global install is 8.0.0#0; `overrides` can pin 8.0.0 back, but the dependency
  chain (`freetype` `libpng` `zlib` `brotli` `bzip2` `opengl*` `egl-registry` `vcpkg-cmake*`) is all newer in
  the current baseline → still a rebuild, and the rebuild also needs vcpkg to download CMake 4.4.3 itself (SSL
  does not work on this machine); and manifest mode **deletes every undeclared package** in
  `VCPKG_INSTALLED_DIR` (measured: it listed hundreds of "will be removed" for boost/opencv/pcl).
- **To do**: `.3ds`/`.x`/`.x3d` are not in the table; PLY has no native per-record write (both go through
  assimp's whole-image write).

## Historical record — 2026-09-12 modelio split / 2026-09-28 three content sources

- `modelio` was split into `meshio` (bound to assimp) and `brepio` (zero third-party dependencies at the time).
  Size evidence: `libviMeshIOd.so` 87.8 MB vs `libviBrepIOd.so` 708 KB.
- The 2026-09-28 `MeshSource` / `BinStlSource` / `AsciiStlSource` / `ObjSource` family
  (`sdk/vine/meshio/MeshSource.hpp`) let an in-package write avoid a temporary file; on 2026-10-27 it was
  folded into `MeshExporter::createSource()` and those classes left the SDK.
- Their choices still hold: every source is "generated on pull" and uses `DataSource` rather than a custom
  `streambuf`; a fixed layout can report its length and a text layout reports `kUnknownSize`; a modified mesh
  is refused (revision comparison).
