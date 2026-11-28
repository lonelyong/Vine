# Graphics review 24 (2026-11-28): the public abstraction surface against peer engines

> **Status**: one dependency cut landed (`RenderBackend.hpp`), one finding recorded (`T77`), and a verdict per seam.
> Nothing behavioural changed and no suite moved. The round asks ONE question of each public abstraction: *which
> peer-engine shape answers this same question, and where do we differ?*
>
> **Scope**: the headers a host includes and the shapes they state - device / frame driver / aggregate, the
> composition root, a frame as a value, a view, a material, ownership, and the asset→geometry conversion. Peer
> references: wgpu (WebGPU), Diligent Engine, Filament and three.js, plus assimp for the import side. Each claim
> below was checked against the file it names; no peer was quoted from memory where a header could be read.
>
> **In one line**: every seam already answers its question with a shape a peer engine also uses, and the
> deviations are stated where they are made - except conversion reporting, where a peer names the dropped item and
> the cause while we name a count, which is `T77`.

## 1. Why this round, and what "compared" means here

The previous rounds read the tree's behaviour (reviews 19-23), its 3D semantics (11 / 17 / 18) and one peer-shaped
detail of the sampler (session 4, the two-axis model). What had not been read as a whole is the **public
abstraction surface**: the types a host writes against, each of which is a claim about where a responsibility
lives. A peer engine is the cheapest control for such a claim, because a wrong boundary shows up as a shape no
other engine uses - or as a shape one engine uses and another cannot.

The method is one question per seam ("who owns X?", "is X a value or an object?"), answered three ways: what the
peer does, what we do, and whether the difference is *stated* at the point where it is made (`G4`: a name means
what it says; `G12`: dependencies show the architecture). A difference with a written reason is a decision; a
difference nobody wrote down is a finding.

## 2. The seams, and the verdicts

| Seam | Peer shape | Ours | Verdict |
|---|---|---|---|
| Device vs frame driver | wgpu: `device` and `queue` are separate; Diligent: the factory returns an `IRenderDevice` and separately the `IDeviceContext`s, and the device is an interface a caller can use on its own | `RenderDevice` and `RenderFrameExecutor` are separate contracts; `RenderBackend` is an aggregate that adds only a destructor | **matches**, with one peer difference recorded as `T32` |
| Composition root | Filament: `Engine` / `Renderer` / `View` are separate; state lives with the object that has the state | `RenderEngine` composes; each mechanism (`FramePump`, `SessionShape`, `SurfaceLedger`) lives in `detail::` with its own state (`graphics-architecture.md` 2.8) | **matches** |
| A frame | wgpu: imperative command encoders the caller owns | declaration → values (`FrameDescription`, `DrawPacket`, `DrawItem`); the backend consumes values (`graphics-architecture.md` 2.2) | **deliberate difference, stated** (two-thread story) |
| A view | Filament: `View` is an object with its own state; three.js: a camera object | `View` is a value (camera snapshot + device-pixel rect + layer mask); the per-window controller is `SceneView` (2.13) | **deliberate difference, stated** |
| A material | Filament: `Material` (asset) + `MaterialInstance` (overrides); three.js: one mutable object | `Material` **is a** `MaterialValues` (asset = values + name + revision), `MaterialInstance` overrides, `MaterialDescription` is the frozen revision | **matches Filament**, plus the revision identity |
| Ownership | wgpu: handles + explicit destroy; Diligent: `RefCntAutoPtr` | four spellings (`intrusive_ptr` / `raw_ptr` / `shared_ptr<const T>` / `span`), stated in `graphics_global.hpp` and enforced by the corpus | **matches**, one vocabulary |
| Asset → geometry | assimp: `aiGetErrorString` names the failure; three.js' glTF loader warns about the dropped extension/primitive by name | `geometryFromShape` answers null for an unconvertible kind or invalid data alike; `sceneFromModel` counts `skipped_meshes` / `skipped_nodes` and nothing more | **finding** (`T77`) |

### 2.1 Device / frame driver / the aggregate

`RenderBackend` derives from both contracts and states why the three questions are separate contracts
(`RenderBackend.hpp`'s class note): a device outlives every window, and a window outlives no frame it is not named
by. The aggregate itself has no state and no verb of its own - the whole class is a destructor over the two bases -
so it cannot become a second home for a fact, which is the failure mode an aggregate usually has.

The peer difference worth recording is in the FACTORY, not the type: a Diligent factory returns the device and its
immediate context as separate objects, so a caller may hold and use the device alone; here every factory answers a
`RenderBackend`, so "a device without a frame driver" is not a legal product of the registry. That is exactly
`T32`'s trigger ("the first factory that only wants to return a `RenderDevice`"), and nothing in this round makes
it fire.

### 2.2 The composition root and a frame as a value

These two are the shapes where a reader coming from wgpu or three.js feels the difference first, and both
differences are stated:

- **Composition root**: Filament draws the same line - `Engine` is not a view, a `Renderer` is not a scene.
  `RenderEngine` keeps composition and lifetime and forwards reports; the three mechanisms that own state live in
  `detail::` and are documented with their own consumers (2.8). The agreement is not a coincidence: both designs
  put "what does the frame run" and "where does it land" where the state is.
- **Frame as a value**: wgpu's encoder is the imperative shape; ours is a declaration (`FrameDescription`) that
  is compiled and then answered by the backend. The cost (the host states a frame instead of calling verbs) buys
  the two-thread story (`graphics-threading.md`) and the derivable-snapshot rule (`graphics-frame-pipeline.md`).
  A deviation that is written where it is made - so it is a decision.

### 2.3 The material, and the ownership vocabulary

The material is the seam where the peer split is almost literal: Filament's `Material` / `MaterialInstance` is
our `Material` / `MaterialInstance` overrides; what is ours alone is that the *asset* counts its own edits
(`revision()`), because every shading value here travels through a setter, and that the *frame* carries the
revision as an immutable `MaterialDescription` (`DrawItem::material_description`). The inheritance
(`Material : MaterialValues`) is the asset adding identity to the value, and the class note says so.

Ownership has one vocabulary in one place (`graphics_global.hpp`) and the corpus uses it: content by
`intrusive_ptr`, borrowed arguments by `raw_ptr`, published values by `shared_ptr<const T>`, contiguous data by
`span`. Peers arrive at the same four roles with different spellings; here they are stated once, where a host can
read them, instead of per header.

### 2.4 The one finding: a conversion reports a count, not a cause

`sceneFromModel` returns `SceneBuild { root, geometries, skipped_meshes, skipped_nodes }`; the lower hop,
`geometryFromShape`, answers a null pointer for a shape it cannot convert and for malformed data alike, and
states in its note that null covers "unsupported shapes or invalid input". Peers on this seam name the cause:
assimp exposes `aiGetErrorString` after a failed import, and the glTF loaders in the JavaScript ecosystem report
the unsupported extension or the dropped primitive by name. The difference matters exactly where a host has to
explain itself: a viewer that opens a document and finds a part missing can say "3 parts were skipped" and
nothing else.

Recorded as **`T77`**, trigger-gated on the first host that reports an import to its user - the same discipline
`T76` follows for the missing-normal case. The fix shape (if it fires) is one conversion report carrying the
dropped item and a cause from the diagnostics' existing vocabulary; the counts stay.

## 3. What this round changed

- **`RenderBackend.hpp` depends on exactly the two contracts it composes.** Removed the includes that named the
  pass model, the frame value, the frame report, the window target and the diagnostics: each already arrives
  through `RenderDevice.hpp` / `RenderFrameExecutor.hpp` where it is used, and a host that drives a device
  without the pass model no longer compiles it in through the aggregate. The class note states the rule
  (`review 24`). No semantics; `test_graphics` 714/714, `test_vk` 243/243, include hygiene 0/1174 files.
- **`T77`** recorded in `../backlog/graphics.md` §2.1.
- **Nothing else**: every other seam above ends with "already stated" - the decision, not a change, is the
  result. This is the expected outcome of an abstraction round once the corpus is converted: the reading cost is
  paid so a later reader does not re-litigate a boundary that was drawn on purpose.

## 4. What the next round should open

- **`T32`'s factory half** stays the one peer difference in the device seam; its trigger (a factory that only
  returns a device, or a device driving two driver kinds) is unchanged.
- **`T77`** waits for a host that reports imports.
- The sampler's two-axis model was verified in the previous session (session 4's memory record): Vulkan composes
  `minFilter` + `mipmapMode`, so nothing here re-opens it.
- A next round can now read a **capability** seam (what a host may ask a device to DO) without re-reading the
  abstraction map: `Capabilities`, `UnsupportedRequest` and the pass/attachment policy are named in §2's table
  and were not re-read here.
