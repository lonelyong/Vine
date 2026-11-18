# ADR-2026-11-16: authoring shape, view and target - values carry declarations, behaviour belongs to objects

- Status: **Accepted**. Approved on 2026-11-16 (all three behaviour changes BC-08 / BC-09 / BC-10 as one batch,
  `setRenderTarget` taking the "make it real" option); the batch order is in §10 of `../research/graphics-2026-11-16-redesign-r2.md`,
  and **everything landed on 2026-11-17**.
- Date: 2026-11-16
- Scope: `PassDesc.hpp` (`target` / `output` / `tag` / `before_draw`), `PassChain.hpp` (`OverlayLayout` /
  `update()`), `CollectView.hpp` → `View`, `RenderSurface` → `RenderArea`, `RenderDevice.hpp` <!-- drift-ok -->
  (`setRenderTarget` / `releasePass` / the read-back signatures), `Capabilities.hpp` (`Features` + `Limits`),
  `RenderDiagnostic.hpp` (`origin`), `RenderPass.hpp` (the public surface of 35 setters),
  `ChainBuilder` / the demo / the chain presets, and four normative passages of `.github/copilot-instructions.md`.
- Related: `ADR-2026-11-16-redesign-seam-and-vocabulary.md` (round one: the seam and the vocabulary; this document is its **concept and API shape** sequel),
  `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`, `ADR-2026-11-13-one-engine-one-surface.md`.
- Review and construction plan: `../research/graphics-2026-11-16-redesign-r2.md` (`E-01…E-12` / `A-01…A-08` / `I-01…I-08` /
  `S-01…S-12`, framework comparison, the order of 18 batches).

## Background

Round one redesigned the **seam and the vocabulary** (FrameDriver / FrameDriverId / FrameHead / DiagnosticDraft / the seam's 6 mechanisms),
with the 85-item capability list and the ten-batch plan in `graphics-2026-11-16-redesign.md`. At the owner's request round two examined four more things:
**whether the abstractions are sound, whether the capabilities are designed soundly, whether the concepts have better semantics, and one programming convention**, comparing against mature frameworks.

Four groups of facts decided this round (all re-checkable):

1. **The design noun `View` had no type**: `CameraSnapshot` (7 fields) and `CollectView` (5 fields, 3 of them duplicates) coexisted,
   with the layer mask passed separately; `DrawDescription` and `PassDesc` therefore each carried three fields.
2. **One word, two things**: `RenderSurface`'s documentation / tests / verbs all say "area" while the type name says "surface";
   the platform surface is expressed by `createSurfaceTarget` / `WindowPresentTarget`.
3. **A pass target had 3 declarations and 3 ways of publishing**, with 7 related fields in `PassDesc`;
   `setRenderTarget` was one name with four meanings (`RenderDevice` / `RenderPass` / `FrameRecorder` / the test double).
4. **`setRenderTarget` was dead storage in vk** (1 write, 0 reads) while vsg really built it; one verb with two semantics,
   one of which had no effect, and which left a raw pointer that could dangle. There was also **one `vector<LightValue>` allocation per pass per frame**
   (`RenderPass.cpp:242` / `ScreenPass.cpp:82`), against "zero allocation inside a frame"; and `PassDesc::tag` had 8 writes and 0 reads.

Also: 4 findings of the 2026-11-11 API audit (`graphics-2026-11-11-next-api.md`, `P1`–`P32`) were **still pinned in the baseline of
`check_api_style.py`**. This round reconciled them one by one (research document §3), and whatever is still open joins this document's batches.

## Decision

**Values carry declarations, behaviour belongs to objects; one concept, one name; a verb with no effect either becomes real or is deleted.**

### 1. One view value, `View`

`View { CameraSnapshot camera; Viewport viewport; std::uint32_t layer_mask; }` becomes the only view value:
`DrawDescription` uses it instead of `camera` + `collect_viewport` + `layer_mask`; `PassDesc`'s declared inputs are baked into it by
`describe`; `CollectView` is deleted (one-version alias). `selectSnapshot(snapshot, view)` takes one value.

Precedents: Filament's `View`, O3DE's `RPI::View`, bgfx's per-view state. Criteria `G1` / `G4`.

### 2. `RenderSurface` → `RenderArea`

"surface" means the platform surface only (`createSurfaceTarget` / `WindowPresentTarget`); the engine-side "one face to draw on" is called an
**area** (`RenderArea` / `createArea` / `areaWidth/Height`). The class's own documentation, test names and `announceAreaSize`
already said area, so this is **letting the name catch up with the semantics**. Criteria `G1` / `G12`.

### 3. A target is declared with two values: `Target` and `Output`

- `PassDesc::writes` + `writes_image` + `writes_shape` → **`Target`** (`Window | Explicit | Transient`;
  a transient's size uses an `optional<Extent>`, deleting the "0 = unspecified" magic value).
- `PassDesc::output_name` + `output_target` + `output` → **`Output`** (a name plus a coarse/fine promise).
- The **four roles** of the target vocabulary are written into the glossary: `RenderTarget` (declared by the host) / `TargetShape` (compatibility) /
  `TargetPlan` (decision) / `TargetObjects` (GPU objects, replacing `TargetImages` and vsg's `OffscreenTarget`) / `TargetLease` (a one-frame lease).
- **`setRenderTarget` must become real or disappear** (see decision 5).

Precedents: UE's `FRenderTargetBinding`, O3DE's `PassAttachment`, wgpu's `RenderPassColorAttachment`. Criteria `G1` / `G4`.

### 4. One authoring shape for a pass: `PassDesc`

`PassDesc` is the only authoring shape; `RenderPass`'s public surface shrinks to a **read-only live handle** (`declaration()` / name / cost).
`RenderPass`'s 35 setters are demoted to `detail` construction entry points; chains / presets change to "produce a `PassDesc` → `apply`".

In the same batch: delete `PassDesc::tag` (8 writes, 0 reads); delete `PassDesc::before_draw`, with per-frame updates belonging to objects -
the overlay / gizmo `update()` is driven by `PassChain::update()` / `SceneView::update()`;
`OverlayLayout` becomes a pure function returning a `Viewport` (a mutable pass reference no longer escapes).

Precedents: the immutable description plus Builder of Filament / O3DE / wgpu. Criteria `G1` / `G4` / `G8`.

### 5. A verb with no effect: become real, or retire

- `RenderDevice::setRenderTarget`: **becoming real is preferred** - both backends do "pre-build / reuse the objects" (vsg already does;
  vk calls `ensureTarget`), so "the target is pre-built (no frame needed)" becomes one capability shared by both backends (new capability ID `CAP-86`).
  The alternative is retirement (one capability fewer, needing separate approval). **Whichever it is, `Impl::target`'s dead storage must go.**
- `RenderDevice::releasePass`: the verb stays, its **contract narrowed** to "forget the state keyed on this pass's identity";
  the sentences about "release the GPU objects the pass owns / release it with the target" are deleted (the engine's `removePass` already releases them separately).
- Read-back: both verbs take an `ImageRef` (the same "which image" vocabulary as a pass input); failures still use `ReadbackResult`,
  but it is promoted from an out-parameter to part of the return (`S-10`'s pilot). **No region read-back** (no consumer, `G8`).

### 6. Capability structure and diagnostic origin

- `Capabilities` splits into **`Features` + `Limits`** (`Capabilities` kept as the aggregate, the old fields forwarded inline for one version).
  Precedents: WebGPU / wgpu / D3D12. Criteria `G4` / `G6`.
- `RenderDiagnostic` gains **`DiagnosticOrigin { Engine, Device, Host }`**; `diagnosticCount()` counts only
  engine + device by default, and host-injected problems are counted separately per origin (new capability ID `CAP-87`). Precedents: the object pointers of the D3D12 info queue,
  and Vulkan debug utils' objects. Criteria `G4` / `G11`.

### 7. No more light allocation in steady-state frames

The light set is a fact of the **scene**, not a field of a pass: step one replaces `describe`'s local vector with a member scratch (zero allocation),
and step two (optional) makes it an immutable `LightSet` shared along with the publish (`shared_ptr<const LightSet>`, rebuilt only when the revision changes),
shared by N passes. Criteria `G10` / `R24` / `N6`.

### 8. Four normative wording revisions (`S-01` / `S-05` / `S-11` / `S-09`)

1. A struct / class **field** may use one line of `///` (it must state the contract); functions still use sectioned `/** @brief … */` (the existing 545 lines of field comments).
2. Accessors are Qt-style and **a command takes a verb** (`getStringAsync` and its kin are commands, not accessors).
3. Sizes / coordinates use `int` (the platform spelling), ids / counts / bit masks use fixed-width unsigned types; not back-filled.
4. Write the ownership vocabulary at the top of `graphics_global.hpp`: `intrusive_ptr` shared ownership / `raw_ptr` borrowed /
   `std::shared_ptr<const T>` an immutable shared value / `std::span` non-owning contiguous data.

Same-batch sweep: delete 26 section comments; give the public value fields and cross-seam enums a fixed underlying type (about 10, not all 37).

## Rejected roads

| Option | Reason |
|---|---|
| Introduce a render graph (UE RDG / Frostbite FrameGraph) | Graph compilation and transient aliasing have no consumer (`G8`); barriers are already covered by one backend table (the `G10` account does not add up) |
| bindless / descriptor heap / argument buffer | No measured bottleneck; it would change the content ABI (`G8` / `G10`) |
| Convert the whole repository to `Result<T>` (old `P27`) | About 200 call sites, benefit and risk out of proportion (`G8`); new API uses values, existing code pays as it changes |
| `Session::open(config)` creating everything at once (old `P18`) | It would hide "the engine borrows a shared device" (ADR-2026-11-15); use documentation to express the order |
| Merge `PassKind` into projection values (old `P4`) | `ScreenPass` has no camera / content / layers, so it is another kind of **execution**, not a projection parameter |
| Build a `ProgramBuilder` for `ShaderProgram` (old `P15`) | No consumer (`G8`); the two halves (object / description) are already clear |
| Unify 9 description-type suffixes and rename all 15 `Content*` (old `P25`) | Pure renaming whose only benefit is looking tidy (`G8`); constrain new types only and change the three worst |
| Pull-style error scope (wgpu style) | The tests already use counters plus a sink; no consumer (`G8`) |
| Retire `setRenderTarget` outright without characterising it first | That loses a capability; make it real first, or add cases first (the reverse of `G8` is "delete it and only then find out somebody used it") |
| Keep copying the lights per pass | The opposite of `R24` / `N6` |

## Consequences

**Good**

- `DrawDescription` / `PassDesc` each lose two fields; the view's three facts have one definition and the two overlapping values disappear.
- "Target" goes from 7 fields + 4 identically named verbs to 2 values + a four-role glossary; `setRenderTarget` is no longer a seam with
  "a verb and no effect".
- `PassDesc` is a pure value again: comparable, diffable, crossable between threads; per-frame updates belong to objects, and the host can see what it called.
- A steady-state frame loses `passes` alloc/free pairs + `passes` light copies.
- `Capabilities` / `Diagnostics` line up with the WebGPU / D3D12 shapes; a host can tell "the engine's problem" from "its own problem".
- The leftovers of the 2026-11-11 API audit (`PassDesc` callbacks, `OverlayLayout`, `ReadbackResult`, `tag`) have a home,
  and the baseline of `check_api_style.py` can go to zero after the matching batches.

**Cost / risk (stated plainly)**

- Decision 4 (the authoring shape) has the widest blast radius: the declaration style in the demo, the chain presets, `PassChainTest` / `PlanTest` /
  `RenderEngineTest` all change; it must be its own batch that can be reverted as a whole.
- Three **behaviour changes** need separate approval: `before_draw` no longer called implicitly (BC-08), vk pre-building (BC-09),
  and host reports no longer counted in the engine's count (BC-10).
- The renames (`RenderArea` / `View` / `TargetObjects`) are a source-level break; hosts outside the repository need one version of migration notes.
- Step one's light scratch only stops allocating - N passes still copy one each; only step two (a shared `LightSet`) gives zero copies,
  and that touches `DrawDescription`'s public fields - so it may stop after step one.

**Trigger conditions (when to reopen a rejected item)**

1. An ROI / thumbnail consumer appears ⇒ add a region to read-back (`A-03`).
2. Measured draw counts make descriptor-set binding the bottleneck ⇒ re-evaluate bindless (the `G10` account has to be redone).
3. A consumer appears that holds a `FrameDescription` across frames ⇒ split the frame value into `FrameValue` / `FrameHandles`.
4. A third backend (outside the Vulkan family) ⇒ round one's batch 8 (`DeviceFormat` / `RenderPassKey`) together with this document's
   `Target` value as the entry to the seam's shape.

## Implementation status

- **Approved (2026-11-16)**: approved together with the seam ADR. Decision 5's `setRenderTarget` takes **becoming real** (vk pre-builds, new CAP-86)
  rather than retirement; decision 8's four normative passages and the sweep of 26 section comments land in the first batch (the convention sweep).
- **Round-two review**: done (`../research/graphics-2026-11-16-redesign-r2.md`).
- **Migration**: **done (2026-11-17)**. The positions of the merged 18 batches are in §10 of the research document; batch progress is archived in the git commit
  bodies (each batch lists its capability ID triples), and this document only records status. The landing commits for this batch's shape: B12 `efc01336` (`View`), B13 `5b22c289`
  (`RenderArea`), B14 `001b2b80` (`Target` / `Output` / `releasePass`), B15 `955f7a94` (`Features` /
  `Limits`), B16a-c `5e31e89b` + `82b4ec3f` + `49645332`, and S8's B17 `7b83a509` (the three `Content*` renames) and
  B9 (documentation truthfulness plus the user documentation regrouped by capability).
- **B16 (2026-11-17, E-04 / E-05 / E-06 + BC-08) has landed**: `PassDesc` is the only authoring shape and `RenderPass`'s public surface is read-only
  (the write gate is `detail::PassNode`); `before_draw` is replaced by the `FrameUpdate` object interface (`PassChain::update()` /
  `SceneView::update()`), and `OverlayLayout` returns a `Viewport`; `tag` is deleted. **Naming deviation**: what the research document wrote as
  `chain->update(d)` is implemented as `PassChain::restate(desc)` - `update()` is already the verb for "update the overlay each frame", and one name with two meanings would
  make a reader guess what the parameter is (`G4`).
- This ADR was changed to "Accepted" when the last batch (S8) landed; no batch was abandoned.
