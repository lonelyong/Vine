# meshio / brepio - the public face of shape I/O

> **Status**: current (the public face is implemented as described here; the OCCT backend is still missing, see 9.2)
> **Last verified**: 2026-10-27
> **Scope**: the public API of `src/loaders/meshio` and `src/loaders/brepio`: the format vocabulary, the loaders, the
> exporters, failure and caching.
> Excludes the geometry data itself (see `graphics-geometry.md`) and the definition of VFS / Stream (see
> `iobase-vfs-design.md`).
> **Sources**: the `sdk/` headers of both modules, `tests/test_meshio`, `tests/test_brepio`.
> **Backlog**: gaps in 9.2; the implementation list in 9.3.

## 1. Overview

### 1.1 Problem and background

The four classes came over from VMR. After the move, five structural problems surfaced, all on the public face and
none inside the implementation:

1. **Two format vocabularies.** Reading goes by extension (the assimp registry) while writing goes by writer name
   (`objnomtl`, `collada`, `gltf2`): `"dae"` is rejected, `collada` is accepted; `isSupportedFormat("a.3ds")` is true,
   yet that format appears in no writable declaration.
2. **Three failure channels.** `load` is `T* + IoError*` out-parameters, `exportAs` throws, and `createSource` defers the
   failure to `DataSource::error()`: a caller has to remember three conventions at once.
3. **One concept, five implementations.** "mesh → bytes" exists five times: `BinStlSource`, `AsciiStlSource`,
   `ObjSource` (meshio) and `StlMeshSource`, `MeshBinSource` (robotics/io); the three in meshio have no consumer outside
   the tests.
4. **State hidden in a singleton.** `defaultInstance()` returns a shared instance: its options are mutable and its cache
   is not thread-safe; both real consumers (`model_viewer`, `urdf2vine`) already switched to local instances and wrote
   the reason in a comment.
5. **brepio is half-finished.** The abstract `BrepExporter` has no implementer (it serves a test double only),
   `BrepLoader::Options` is a placeholder `char` plus three accessors, and `BrepShape` does not hold the shape it loaded
   (a raw pointer plus external ownership, `G10`).

### 1.2 Goals

- One vocabulary: a format is an enum, the capability comes from one table, and reading, writing and the file chooser
  all take it from there.
- One entry-point set: reading the headers is enough to know what is possible; there are no overloads that exist just
  for symmetry.
- Failure is a value: a read returns `vn::io::Result<T>`, a write returns `vn::io::IoError`.
- Delete the classes and methods without a consumer (`G8`), and keep what consumers actually need (read, write, cache,
  in-package writing).

### 1.3 Non-goals

- This stage does not implement the OCCT read/write details, only the "no backend" semantics and the build switch; no
  second B-rep backend is introduced.
- No native record-by-record writer for PLY / glTF: what assimp can write is left to assimp.
- No change to the data shape of `geom::spatial::Mesh` / `geom::spatial::Model`, only to the ownership of
  `BrepShape`.
- No merging of robotics/io's own STL source (see `D5`).

### 1.4 Conclusion in one sentence

**A format is an enum and the capability is one table; reads go through `Result` and writes through `IoError`; both
modules share one shape: `Loader` (options + cache) and `Exporter` (options) - four classes and five free functions.**

## 2. Design Points

### 2.1 A format is a type, not a string

- **What**: the `MeshFormat` / `BrepFormat` enums; the three free functions `formatFromPath()` / `formatName()` /
  `formatInfo()`; and `supportedExtensions()` generated from the table.
- **Why**: `G1`, `G4` - it removes the two traps `dae`/`collada` and `obj`/`objnomtl`; "can it stream" turns from a
  run-time exception into a boolean in the table; and it uses the same vocabulary as imageio's `formatFromPath()`
  (`G6`).
- **Boundary**: the table IS the **read/write contract** - every value in it can be read and written; for an extension
  outside it `load()` returns `Unsupported` instead of passing through assimp's forty-odd formats (`.3ds`/`.x`/`.x3d`
  are therefore no longer supported by default).
- **Price**: `G9` - adding a format edits the table and recompiles (one line); in return, reading, writing and the
  chooser have exactly one answer.

### 2.2 Failure is a value

- **What**: `load`/`loadMerged` return `vn::io::Result<vn::intrusive_ptr<T>>`; `exportAs` returns `IoError` (`Ok` means
  success).
- **Why**: `G4`, `G6` - one return value states both success and cause; `Vfs` and `ZipArchive` already use `Result`, so
  out-parameters and exceptions side by side would be two sets of rules.
- **Boundary and price**: `Result` carries the failure **classification** only, not assimp's or OCCT's error text; under
  value semantics `intrusive_ptr` costs one more atomic count, which is negligible against parsing (`G10`).

### 2.3 State belongs to the instance

- **What**: `Options` are given at construction and read-only afterwards; the cache belongs to the loader, and
  `clearCache()` releases it explicitly
  (`../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md`).
- **Why**: `G4` - a shared mutable singleton turns "whose options are in effect" into an implicit convention; both
  consumers have already switched to local instances.
- **Boundary**: `defaultInstance()` is not kept; the cache's lack of thread safety is stated on the class (`N1`)
  instead of being papered over with `mutable` + `const`.

### 2.4 One writer, three targets, two shapes

- **What**: each format has exactly one formatting implementation; `exportAs(path)`, `exportAs(stream)` and
  `createSource()` all build on it, with one set for `Mesh` and one for `Model`; a non-streaming format (`.gltf` needs
  its `.bin`, `.3mf` needs a path) is rejected by `streamable == false`.
- **Why**: `G1`, `G5` - today the assimp path and the native STL/OBJ path do **not promise the same output**; once
  gathered, "one content, three targets" becomes testable.
- **Flattening belongs to the writer, not to us**: registering the `stl`/`ply` exporters forces
  `aiProcess_PreTransformVertices` (it copies the scene first and then processes it), while OBJ accumulates the node
  matrices itself and writes the vertices through them; we only build an honest aiScene (nodes carry local transforms).
  Neither merging into a single mesh nor boolean union is part of the contract - STL/PLY already write several meshes
  into one file.
- **`createSource()` is a framework capability, not a convenience overload**: `DataSource` is iobase's in-package
  writing channel, so in-package writing touches no temporary file and does not force the caller to route around one
  (`G7`). `G8`'s consumer answer is known to be "none", yet it stays - the consumer IS this capability.

### 2.5 brepio mirrors meshio, with an optional backend

- **What**: `BrepLoader` / `BrepExporter` have the same shape as meshio (`G6`): one entry each for a path, a stream and
  a data source; neither takes options.
- **Why**: `G8` - the placeholder `Options`, `OptionsHash`, the abstract exporter base class and `canExport` have no
  consumer; there is one kind of shape, and "can it be exported" is simply `isValid()`.
- **Optional backend**: `find_package(OpenCASCADE QUIET)`; when found it is linked PRIVATE and `VN_BREPIO_HAS_OCCT` is
  defined, when not found only the contract remains: `supportedExtensions()` is empty, `load` / `exportAs` answer
  `Unsupported`, and so does the source's `error()` - **no exceptions**, since that would add a third failure channel to
  reading and writing. No FetchContent (`G10`: hundreds of MB, builds measured in tens of minutes, and only brep needs
  it).

### 2.6 `BrepShape` holds the shape it loaded

- **What**: `setShape(std::shared_ptr<const TopoDS_Shape>)` takes ownership, and `shape()` returns a borrowed view
  (`G10`).
- **Why**: with today's `setShape(TopoDS_Shape*)` nobody is responsible for releasing it, so it dangles or leaks as soon
  as the loader returns.
- **Boundary**: geometry only forward-declares `TopoDS_Shape`, it includes no OCCT header and links nothing;
  `make_shared` happens on the brepio side, and the deleter is type-erased with the `shared_ptr`, so destruction on the
  geometry side is safe.

### 2.7 Comparison with VMR

| VMR | This design |
|---|---|
| four entries `load`/`loadMerged`; `format` is a writer-name string | the entries stay; the format becomes an enum (`G4`: the name says what comes back) |
| cache grouped by `Options` + a `Default()` singleton | deleted: options and cache both belong to the instance (`G4`, `G11`) |
| `IsSupportedFormat`, the abstract `BrepExporter`, `canExport`, the placeholder `Options` | deleted (`G8`); format questions all go to `formatFromPath()` |
| `createSource(mesh \| model)` | kept (a framework capability, `G7`) |

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Format vocabulary and capability table | enum + three free functions + `supportedExtensions()` | in place |
| F2 | `MeshLoader` | path/stream → `Model` or a merged `Mesh`, cached by content | in place |
| F3 | `MeshExporter` | `Mesh`/`Model` → path / stream / data source; flat formats flattened by the writer | in place |
| F4 | `BrepLoader` / `BrepExporter` | STEP / IGES / BREP, path and stream | in place (OCCT optional) |
| F5 | `BrepShape` ownership | a `shared_ptr` takes over the OCC shape | in place |
| F6 | File chooser interface | `supportedExtensions()` is the only filter source | already used by `model_viewer` |

### 3.1.1 F1 Format vocabulary and capability table

- **Behaviour**: the `MeshFormat` / `BrepFormat` enums + `formatFromPath()` / `formatName()` / `formatInfo()` +
  `supportedExtensions()`; the table answers `extension` / `has_encoding` / `streamable`.
- **Constraints/requirements/acceptance**: `C1`, `R1`, `R3`, `T1`, `T2`.
- **Caveats**: `N1`; `Gltf`/`ThreeMf` need a second file or a path ⇒ `streamable == false`; only `Stl`/`Ply`/`Fbx`
  honour `Options::encoding`.

### 3.1.2 F2 `MeshLoader`

- **Behaviour**: `load()` keeps the structure (node tree / several meshes / materials), `loadMerged()` flattens it into
  one `IndexedTriangleMesh`; one entry for a path and one for a stream; `clearCache()`. A default material the importer
  invents because the file names none (assimp's `AI_DEFAULT_MATERIAL_NAME`, which for STL comes with a white specular)
  is **not reported to the caller**: the entry stays "no material", the same as the file (once a white specular folds
  into metallic 1, a model renders black in a scene without an environment map, see `graphics-material.md` 3.6).
- **Scale**: neither post-processing set **welds vertices** (`aiProcess_JoinIdenticalVertices` is not in the list and
  there is no switch for it). A CAD-exported part carries its own vertices per face (per-face colour + per-face normal),
  coincident corners are never equal, and welding cannot merge them - so all it would cost is "one hash entry per
  corner", and a triangle soup (STL, 3 corners per triangle) is exactly where it is most expensive: a 1.8 GB / 37.7M
  triangle part was measured being OOM-killed with welding on and read in 12.5 GiB without it. The price is that such a
  document carries one copy of duplicated vertices too many; the way it is read does not change.
- **Values the file states are not rewritten**: `aiProcess_FixInfacingNormals` is likewise not in the list - it
  heuristically decides a mesh faces inward and flips its normals, which replaces the file's fact with a guess; if the
  file says inward, inward is what is handed back, and a consumer that needs otherwise detects it itself. Each
  post-processing set does only two things: fill in missing channels (`Triangulate`, `GenNormals`) and drop values
  provably not geometry (`FindInvalidData`); the excluded passes stay in the code as a comment at the end of the list.
- **Constraints/requirements/acceptance**: `C2`, `C3`, `R2`, `R4`, `T3`, `T4`. **Caveats**: `N2`, `N3`, `N4`, `N6`.

### 3.1.3 F3 `MeshExporter`

- **Behaviour**: six entries: `exportAs(mesh|model, format, path|stream)` and `createSource(mesh|model, format)`.
- **Constraints/requirements/acceptance**: `C4`, `C5`, `R1`, `R2`, `T1`, `T5`, `T7`.
- **Caveats**: a source in an assimp format puts the whole file in memory on the first pull (`size()` is known); only
  native STL/OBJ is record by record.

### 3.1.4 F4 `BrepLoader` / `BrepExporter`

- **Behaviour**: `load(path)`, `load(stream, format)`, `exportAs(shape, format, path | stream)`,
  `createSource(shape, format)`.
- **Constraints/requirements/acceptance**: `C6`, `R2`, `R4`, `T6`.
- **Caveats**: the format of a stream is mandatory - OCCT's reader picks by format and does not sniff the content the
  way assimp does.

### 3.1.5 F5 `BrepShape` ownership

- **Behaviour**: `setShape(std::shared_ptr<const TopoDS_Shape>)` takes over, `shape()` borrows, `isValid()` = non-null.
- **Constraints/acceptance**: no OCCT header and no link on the geometry side; `T8`.

### 3.1.6 F6 File chooser interface

- **Behaviour**: `supportedExtensions()` is generated from the table (lowercase, with the leading dot);
  **constraints/acceptance**: `C1`, `T1`.

## 4. Public Interface

The format vocabulary (both modules use the same names and shapes; `BrepFormat` has no capability table, all three of
its formats are single-file):

```cpp
// vn::meshio
enum class MeshFormat : std::uint8_t { Unknown = 0, Stl, Obj, Ply, Gltf, Glb, Fbx, Collada, ThreeMf };
struct MeshFormatInfo { const char* extension; bool has_encoding; bool streamable; };  // Unknown → all empty
const char*           formatName(MeshFormat) noexcept;                                 // "stl"
MeshFormat            formatFromPath(const std::filesystem::path&) noexcept;           // unknown → Unknown
const MeshFormatInfo& formatInfo(MeshFormat) noexcept;

// vn::brepio - same names and shapes; no capability table (all three are single-file)
enum class BrepFormat : std::uint8_t { Unknown = 0, Step, Iges, Brep };
const char* formatName(BrepFormat) noexcept;                                           // "step"
BrepFormat  formatFromPath(const std::filesystem::path&) noexcept;
```

### 4.1 `vn::meshio` - `MeshLoader` / `MeshExporter`

| Interface | Contract | Notes |
|---|---|---|
| `static supportedExtensions()` | generated from the table, lowercase with dots | the file chooser's only filter source (`C1`) |
| `Options{ scale_mode, auto_scale_output_unit, custom_scale_factor }` | given at construction, `options()` read-only | two policies = two loaders |
| `Result<intrusive_ptr<Model>> load(path)` / `load(istream&, MeshFormat hint = Unknown)` | keeps the node tree/meshes/materials | a path and a byte stream hit the same cache (`C3`) |
| `Result<intrusive_ptr<Mesh>> loadMerged(same)` | flattened into one `IndexedTriangleMesh` | the only entry that carries `PreTransformVertices` (`N6`) |
| `void clearCache()` | releases the geometry in the cache | explicit release, no cap |
| `Options{ scale_factor, encoding }` | given at construction, `options()` read-only | `encoding` only affects formats with `has_encoding` |
| `IoError exportAs(mesh\|model, MeshFormat, path)` | writes the file and creates parent directories; the writer flattens flat formats | a `Model`'s companion parts (`.mtl`) go to the same directory |
| `IoError exportAs(mesh\|model, MeshFormat, ostream&)` | one self-contained content into one stream | `streamable == false` ⇒ `Unsupported` (`C4`); losing only a companion part is dropped and stated |
| `shared_ptr<DataSource> createSource(mesh\|model, MeshFormat)` | the lazy version of `exportAs(stream)`: produced on the first pull, replayed by `rewind()` | a framework capability: in-package writing touches no temporary file (`G7`) |

### 4.2 `vn::brepio` - `BrepLoader` / `BrepExporter`

| Interface | Contract | Notes |
|---|---|---|
| `static supportedExtensions()` | `.stp` `.step` `.igs` `.iges` `.brep` | the same shape as meshio (`G6`) |
| `Result<intrusive_ptr<BrepShape>> BrepLoader::load(path)` | picks the reader by extension | content-fingerprint cache, `clearCache()` |
| `Result<...> BrepLoader::load(istream&, BrepFormat)` | the stream's format is **mandatory** | OCCT does not sniff |
| `IoError BrepExporter::exportAs(shape, BrepFormat, path \| ostream&)` | both targets are one implementation | no options |
| `shared_ptr<DataSource> BrepExporter::createSource(shape, BrepFormat)` | the same lazy contract as meshio; without the backend `error()` is `Unsupported` | a framework capability (`G7`) |

### 4.3 `vn::models::BrepShape` (ownership only changes)

| Interface | Contract |
|---|---|
| `const TopoDS_Shape* shape() const` | borrowed; null when unset |
| `void setShape(std::shared_ptr<const TopoDS_Shape>)` | takes ownership (replacing today's raw pointer) |
| `bool isValid() const` | `shape() != nullptr` |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints

- `C1` The enum and the table are the only format vocabulary: `supportedExtensions()`, `formatFromPath()` and both ends
  of reading and writing take it from there, and no second copy is hand-written.
- `C2` The cache key is the file's content fingerprint; options belong to the instance, so the key carries none.
- `C3` A stream entry first lands the stream as one run of bytes and then parses it, hitting the same cache entry as the
  path entry.
- `C4` A format with `streamable == false` returns `IoError::Unsupported` for the stream / source targets; it does not
  start producing and fail later.
- `C5` Writing a `Model` to a path may write companion files; `stream` / `source` hold one self-contained content only:
  a format that must have a second file (`.gltf`, `.3mf`) returns `Unsupported`, while one that loses only a companion
  part (OBJ's `.mtl`) drops it and states so.
- `C6` Without OCCT, `supportedExtensions()` is empty and `load` / `exportAs` / the source's `error()` all answer
  `Unsupported`; `formatFromPath` answers as usual (a format is a format, a capability is a capability). The empty
  answer must be a **failure value**, never `Ok` + a null pointer.

### 5.2 Requirements

- `R1` Every enum value can be written and read back (round-trip cases, `T1`); a `streamable` value must also let
  `createSource()` produce the same content (`T5`).
- `R2` A public header uses base-module types only (iobase / geometry / runtime / crypto); no third-party library leaks
  out.
- `R3` Reading, writing and the chooser give one consistent answer for the same format (`T1` pins both sides).
- `R4` A failure must be classified: `NotFound` / `InvalidData` / `Unsupported` / `IoFailure`, never just an empty
  answer.

### 5.3 Invariants

- `N1` A loader's cache is not thread-safe; one loader instance is used from one thread at a time (the class comment
  carries the `@warning`).
- `N2` The path entry reads the file twice: once for the fingerprint, once for the parse. That is the price of "never
  return stale geometry".
- `N3` `ScaleMode::Auto` judges mm/m by default from a 10-source-unit AABB diagonal and then converts to
  `auto_scale_output_unit`.
- `N4` A stream entry has no real file name: OBJ's `mtllib` is not resolved (use the path entry when a companion file is
  needed).
- `N5` `Result`'s `*` / `->` are undefined behaviour on failure; test `ok()` first.
- `N6` `aiProcess_PreTransformVertices` appears only in `loadMerged()`'s post-processing; `load()` with it would destroy
  the node tree.

## 6. Caveats and Traps

| # | How it fails | How to detect it | How to prevent it |
|---|---|---|---|
| P1 | adding `PreTransformVertices` to `load()`: the node graph is deleted and transforms are baked into vertices | `ModelLoadTest`'s "the transform stays on the node" | `N6`; keep the two post-processing lists separate |
| P2 | dereferencing an empty `Result` (`m->name()`) | `N5`; `[[nodiscard]]` warns about the missed check | `if (!r)` before `r.value()` |
| P3 | assuming `createSource(mesh, Gltf)` writes record by record, when in fact the first pull holds the whole file in memory | `T5` pins only STL's exact length and record-by-record behaviour | state it per format in the `createSource()` comment |
| P4 | adding a singleton back to save one local object | review rejects it on `G4` | 2.3 is a design decision |
| P5 | flattening once yourself and then handing a transformed scene to assimp ⇒ STL/PLY are transformed twice | export a model with a translation and read it back: the bounding box is offset by double | the scene stores local transforms only, and flattening is done by the writer alone (`stl`/`ply` already force it) |

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | keeping two format vocabularies (read by extension, write by writer name) | `G1`: two names for one format inevitably drift | - |
| D2 | keeping the `IoError*` out-parameter on `load` | `G4`: success and result split across two parameters | - |
| D3 | keeping the abstract `BrepExporter` base class | `G8`: its only implementer is a test double | - |
| D4 | keeping the four public source classes | `G1`/`G8`: four classes express one `createSource()` | - |
| D5 | merging robotics' STL source into meshio | `G10`: it would make robotics carry assimp for one STL writer | - |
| D6 | turning off stream / source for `Model` (path only) | `G4`: a model that fits one file should not be forced to hit the disk first; the price is one flatten | - |
| D7 | type-erasing `BrepShape` with `shared_ptr<const void>` | `G4`: every access needs a `static_cast`, leaving type safety to a convention | - |
| D8 | keeping the assimp registry as the read contract | `G1`: a read table and a write table, back to item 1 of 1.1 | - |
| D9 | a cross-module format-reader registry (`registerLoader()` + plugins) | no consumer (`G8`); it brings back a global mutable registry whose registration order and lifetime nobody owns (`G11`); the modules' options and returned shapes already differ (`Model` / `Mesh` / `BrepShape` / `PointCloud` / `Image`), so a unified entry could only degrade into `any` and push the type decision onto every caller (`G4`, `G7`). When "just read some file" is really wanted, the answer is a **static dispatch table** at the caller's layer (one function that explicitly lists "extension → concrete loader" plus a priority), not a registry. | - |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | value by value: every enum value can be `exportAs(path)`-ed and read back with `loadMerged`; `supportedExtensions()` equals the table item by item | `MeshIoTest.FormatVocabularyIsTheReadContract`, `TheExtensionTableNamesWhatTheLoaderActuallyReads` |
| T2 | extension and enum give one answer: `formatFromPath("a.DAE") == Collada`, `exportAs(..., Collada, "a.dae")` succeeds | `MeshIoTest.DaeAndColladaAreOneFormat` |
| T3 | failure classification: a missing file is `NotFound`, bad bytes are `InvalidData`, an extension outside the table is `Unsupported` | `MeshIoTest.LoadSaysWhyItFailed` |
| T4 | cache: two loads of the same bytes hit the same object; the object is rebuilt after `clearCache()` | `MeshIoTest.LoadCachesSameFile` |
| T5 | source: for every `streamable` format, `createSource()` matches `exportAs(stream)` byte for byte | `MeshSourceTest.MatchesTheStreamEntry` |
| T6 | brepio: without OCCT `load` returns `Unsupported`; `formatFromPath` / `supportedExtensions` are correct | `BrepIoTest.LoadRefusesWithAValueNotAnException` |
| T7 | `Model` → STL: a node with translation/scaling is exported and read back, and the bounding box equals the world-space result (the transform is applied exactly once), with materials and hierarchy dropped | `MeshIoTest.ModelToStlAppliesTransformsOnce` |
| T8 | `BrepShape` with no shape set has `isValid()` false; `setShape({})` clears it | `BrepIoTest.AnEmptyShapeIsNotValid` |
| T9 | OCCT round trip: box → STEP / IGES / BREP → read back `isValid`; a stream entry of the same bytes hits the same cache | `BrepIoTest.ExportsABoxAndReadsItBack` |

## 9. Status and Evolution

### 9.1 Current state

The public face is implemented as described here: meshio is complete (the format table, `Result`/`IoError`, six export
entries, `createSource()`), and brepio's API and failure semantics are in place; OCCT reading and writing is still the
gap.

### 9.2 Known gaps

- OCCT is only connected when the configuration finds it: `find_package(OpenCASCADE CONFIG QUIET)` is not forced, and a
  build that does not find it has an empty extension list and answers reading and writing with `Unsupported` (`C6`).
- `.3ds` / `.x` / `.x3d` are outside the table; add one enum value and one table entry when needed.
- PLY has no native record-by-record writing; `createSource()` holds an assimp format's whole content in memory.
- robotics/io's `StlMeshSource` and meshio's STL writer are a deliberate duplication (`D5`).

### 9.3 Compatibility / migration

The public face is a breaking change: `format` became an enum, the read entries return `Result`, the write entries
return `IoError`, and `defaultInstance()` / `isSupportedFormat()` plus the four source classes are gone. Every call site
inside the repository (`model_viewer`, `urdf2vine`, both test suites) has migrated; a caller outside the repository
follows §4 for the new signatures.

## 10. References

- VMR reference implementation: `VMeshLoader.h`, `VMeshExporter.h`, `VBrepLoader.h`, `VBrepExporter.h` under
  `D:\PROJS\VMR\src\scene_core\include\vmr\io`.
- Current implementation: `src/loaders/meshio`, `src/loaders/brepio`; vocabulary sources:
  `src/base/iobase/sdk/vine/io/IoError.hpp` (`Result` / `IoError`) and `src/base/iobase/sdk/vine/io/Stream.hpp`
  (`DataSource`).
- Decisions and notes: `../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md`, `../memory/loaders.md`.
