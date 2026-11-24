# graphics review 9: the vk backend's system, concepts and framework

> **Status**: draft, non-authoritative - a working record. §2 is the framework as the tree stated it, §3 is
> findings, §4 is what the peer engines do that does NOT transfer, §5 proposed an order. Written as a read-only round;
> **steps 1-4 of §5 were implemented afterwards** (2026-11-25, in the same session): `K9`'s two sentences, `K7`'s read
> set at the claim, `K6` (a)+(c), and `K3`'s filter-then-rank. The design those steps produce now lives in
> `vk-backend.md` §2.19 / §2.20 / `F2` / `N9` / `N10`; the findings that were NOT landed are backlog `T17` (amended),
> `T67` (`K1`), `T68` (`K2`) and `T69` (`K8`).
> **Last verified**: 2026-11-25, tree `522ce5b4` on `dev` (clean working tree).
> **Scope**: `gfx_backend_vk` read as a rendering SYSTEM: which mechanisms exist, which concept owns each, and
> where the boundaries sit, compared against the backends of Filament, Diligent Engine, Unreal's RHI, Granite,
> The-Forge, bgfx, wgpu/WebGPU and `vk-bootstrap`. It does not re-walk implementation quality (review 7), the
> incremental edit path or the threading model (review 6), or the seam's concepts and capabilities with `vsg`
> leaving (review 8). One-frame-in-flight (`ADR-2026-11-15`), the device-as-pool shape (`ADR-2026-11-15`),
> VMA (`D1`), a transfer queue (`F11`) and a render graph (review 5 §2.3) are decisions; this page does not
> reopen them.
> **Sources**: `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/**` (the contracts),
> `src/plugins/gfx_backend_vk/src/internal/**` (the paths), `tests/test_vk/**` (behaviour),
> `.ai/design/vk-backend.md`, `.ai/design/graphics-sessions.md`, `.ai/design/graphics-content-lifecycle.md`.
> **Backlog**: `../backlog/graphics.md` - this round adds no entry; §5 lists the candidates.
> **Numbering**: `K#` is this page's own set (review 8 used the same prefix for its capability findings; cite
> "review 9 `K#`"). The module's `F#` / `C#` / `R#` / `N#` / `P#` / `D#` / `T#` remain `vk-backend.md`'s.

## 1. What this round is

Four rounds have looked at the graphics module from four angles: the concept/capability census (8), the
incremental edit path and the threading model (6), implementation quality (7), and the first architecture
benchmark across eight engines (5). None of them read **the backend as its own framework** - which mechanisms
it has built, which concept owns each, and whether the boundaries those mechanisms draw are the ones the peer
backends draw.

That is this round. It asks three questions of `gfx_backend_vk`:

1. **System.** What is the set of mechanisms a frame actually runs through, and does each have one owner and a
   witness? (§2)
2. **Concepts.** Where does a CONCEPT live - memory tier, shader variant, content lifecycle, device choice -
   and is the layer that owns the fact the layer that holds it? (§3 `K1`-`K4`, `K8`)
3. **Framework.** Where are the boundaries drawn: which work happens before recording, which during, which
   under which lock, and what does a second consumer (a second driver today, a second API family tomorrow) pay
   for the current line? (§3 `K5`-`K7`)

### 1.1 The peers this round reads, and for what

| Peer | Mechanism read | Why it is the right comparator |
|---|---|---|
| Filament (driver API) | a recorded `CommandBuffer` stream; offline material compilation (`matc`); staging copies for buffer updates; a descriptor-set pool | the closest "thin Vulkan driver with a product above it" in the list |
| Diligent Engine | `IRenderDevice` / `IDeviceContext` split; `USAGE_DEFAULT` + `UpdateBuffer` staging; pipeline-state cache | the closest cross-API backend framework with explicit resource states |
| Unreal RHI | `FRHICommandList` + RHI thread; PSO precache (`FShaderPipelineCache`); deferred RHI-resource deletion; bindless descriptors | the reference for "who compiles and who deletes, on which thread" |
| Granite (Vulkan) | shader-reflection-derived descriptor/pipeline layouts; render-graph-driven barriers; timeline semaphores | a Vulkan-first backend that builds the metadata machinery this one hand-writes |
| The-Forge | `Renderer` / `Cmd` / `Queue` / `Fence` / `RootSignature` / `DescriptorSet` across D3D12/Vulkan/Metal | the second API FAMILY, i.e. the shape the owner's D3D12/Metal premise will meet (review 8 §4.7) |
| bgfx | per-frame transient vertex/index streams; views; API-agnostic command buffer | the "no per-geometry upload for dynamic data" precedent |
| wgpu / WebGPU | bind-group layouts as declared values; pipeline caching; the API owns image transitions | what "the framework decides the resource states for you" costs and buys |
| `vk-bootstrap` | `PhysicalDeviceSelector`: filter by requirements, then rate | the canonical answer to device selection |

None of these are dependencies and none of their code is copied (D1's rule). The comparison is about which
question each mechanism answers, not about porting it.

### 1.2 Rule of evidence

Every finding names a file and a line, or a test, or a measured number already in the tree. A peer claim is a
statement about that peer's documented design, not a benchmark. Every finding names the `G#` it serves or
trades against, and says whether the fix is a mechanism (needs an ADR), a local change (needs a case), or a
trigger-gated backlog item.

## 2. The framework as it stands

### 2.1 The twelve mechanisms, their owners and their witnesses

| # | Mechanism | Owner / file | What it answers | Peer analogue | Verdict |
|---|---|---|---|---|---|
| 1 | Device + frame drivers | `Device` / `Device.cpp` | one `VkInstance`/device/queue; N `FrameDriver`s that may run off-screen frames at the same time | Diligent's device / context split; UE's RHI object + command lists | stronger than either on cross-driver sharing; no peer has two frame bodies on one device |
| 2 | Target table | `TargetTable` | host `RenderTarget*` -> `TargetObjects`, the shape, the per-frame lease, the release/park | Diligent's `ITexture` + state; wgpu's implicit transitions | sound; the lease is Vine-specific and documented (`C5`) |
| 3 | One pass = one value | `PassTarget`, `FrameHead::Pass` | where a pass draws (off-screen set or window image) as one handle with the four facts a pass needs | bgfx's view / wgpu's render pass | sound; `D8` names why no base class |
| 4 | Dynamic rendering + layout records | `DynamicRendering`, `BarrierPlan`, `TargetObjects` | one `vkCmdBeginRendering` per pass; one dependency per pass entry; one layout record per image | Granite (graph-derived barriers), Diligent (explicit states) | sound; manual but table-driven, and the traps are MEASURED (`P15`) |
| 5 | Content caches | `ContentCaches` | programs, streams, instance streams, pipelines, layouts, images, the `VkPipelineCache` | UE/Diligent resource + PSO caches | sound; identity keys + revision; one lock |
| 6 | Per-frame scratch | `FrameScratch` | the uniform ring and the descriptor-set pool of ONE driver | Filament's per-frame command stream; Diligent's dynamic buffers | sound; per-driver ownership is the concurrency answer |
| 7 | The frame head | `resolveFrameHead` + `FrameHead` | resolves every plan and records every upload once, under the cache lock; recording reads values | UE's `FMeshDrawCommand` cache; Filament's command stream | unique and well-witnessed (`VkFrameHeadTest`) |
| 8 | Residency claims | `ResidencyClaims` + `FrameBuilds` | who may BUILD a geometry key when two drivers race | none (peers have one render thread) | unique; the three-state machine is the right answer to "never wait" |
| 9 | Retirement | `RetirementQueue` + `DeviceClock` | when a released object may be destroyed (submission watermark) | UE's deferred RHI-resource delete | sound; the second window (a frame's share) is the Vine-specific half |
| 10 | Proven uploads | `ContentImages` batches | a texture becomes resident only when its copy's fence reported | Filament/Diligent texture init | sound; `T66`'s fix, and the model `K1` asks streams to follow |
| 11 | Diagnostics | codes + `ReportOnce` | a stable code, a subject, a limit; absent is not 0 | rare in peers | AHEAD of the peers |
| 12 | Pipeline cache file | `PipelineCacheFile` | a device+driver-keyed blob, strict read, one retry | UE/Diligent cache files | sound |

### 2.2 One frame, with the boundaries marked

```
FrameDriver::executeFrame
  FrameGate (driver)             - one frame at a time, reentry REFUSED (not queued)
  body gate (device)             - SHARED for off-screen frames, EXCLUSIVE when presenting
  clock.begin() + FrameBuilds    - the frame's submission token and its admissions
  claimPasses()                  - which passes run; target CLAIMS; ensure() for draw targets;
                                   the window's image is acquired here
  vkBeginCommandBuffer           - labels, the clock's frame pair, the query pool growth
  FrameScratch::beginFrame       - ring cursor 0; a ring that ran out is grown HERE
  ContentDraw::beginFrame        - the state shadow starts empty
  resolveFrameHead()             - *** THE CONTENT CACHE LOCK, ONE HOLD ***
                                   planScreen / planCommand (compile + pipeline create + set lookup)
                                   resolveTextures (records copies + stand-in)
  record loop, per pass          - source lookups (target map lock), source/shadow transitions,
                                   vkCmdBeginRendering, record / recordFullscreen, vkCmdEndRendering
  vkEndCommandBuffer, vkQueueSubmit2, vkWaitForFences
  frame_builds.publishAll()      - the fills are provably complete
  caches.endFrame(uploadBatch)   - ContentImages: batchCompleted / batchAbandoned
  present, then retirement.advance()
```

Two facts of this drawing are the subject of `K5` and `K7`: the cache lock is held across COMPILATION and
PIPELINE CREATION, and the read set of a pass (sampled sources, shadow maps) is resolved inside the record
loop, not at the head.

### 2.3 Where the design is already ahead of its peers

These are stated so the findings are read as refinements, not as a verdict against the framework:

1. **The head/record split with a witnesses lock count.** No peer in the list has a test that says "a frame's
   cache work does not grow with its draw count" (`VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls`).
2. **Cross-driver build admission without waiting.** Filament and Diligent serialize the render thread; this
   backend lets two drivers build different geometries at once and never waits (`ResidencyClaims`).
3. **Release without a device stall, with two windows stated** (submission watermark + a frame's share). UE's
   deferred delete is the same idea, but the "a frame holds a share" half is explicit here (`ADR-2026-11-24`).
4. **Honest reporting.** Absent != 0, one report per episode, stable codes checked both ways by a gate. Most
   peers answer a zero and leave the host to guess.
5. **A proof type for "I hold the lock"** (`ContentCaches::Lock`), which turns a comment into a compile-time
   parameter (`G4`).
6. **Measured cost model with per-mechanism witnesses** (`R4`): ring builds, set builds, pool bytes, pipeline
   count, recording numbers, retirement numbers.
7. **Dynamic rendering + sync2 with no pass objects**, and MSAA/depth resolve through the attachment info -
   the same shape the modern peers arrived at.

## 3. Findings

### 3.1 `K1` Content streams are the only resource that still lives in host-visible memory

- **What the tree does.** `ContentCaches::upload` (`ContentCaches.cpp:275-293`) asks
  `detail::createHostBuffer`, which allocates with `MemoryUsage::HostVisible`
  (`ContentAbi.hpp:150-175`, the allocation at `:171`). `MemoryAllocator::findMemoryType`
  (`MemoryAllocator.cpp:39-63`) takes the FIRST type that offers `HOST_VISIBLE|HOST_COHERENT` and never asks
  for `DEVICE_LOCAL`. Every vertex, index and instance stream is therefore host memory.
  The two other kinds are the counter-example IN THE SAME MODULE: texture images are allocated
  `DeviceLocal` (`ContentImages.cpp:310`, `:841`) and copied through a host staging buffer recorded into the
  frame (`:387`, `:859`), and targets are `DeviceLocal` (`TargetObjects.cpp:46`, `WindowTarget.cpp:176`).
- **The design text says otherwise.** `ContentCaches.hpp:70` states "Streams and staging copies live in
  host-visible coherent memory; the residency step (M6) is what moves streams to device-local memory behind
  one copy". Nothing does that today: M6 was the residency ACCOUNTING (`vk-backend.md` §2.11 / §3.1.10).
  `graphics-sessions.md` §3.1.4 is the honest version ("vk's fill today is a host-visible memcpy ... when
  uploads move to device-local"), and `graphics-content-lifecycle.md` 2.8 is about the image staging, not
  the streams.
- **Why it matters, and why no number in the tree can see it.** Every performance figure (the 8.2 ms host run,
  the T35 gate) was measured on lavapipe, where the device-local and host-visible TYPE is the same one
  (`.ai/memory/graphics.md` records that trap twice). On a discrete GPU, vertex fetch, index fetch and
  instance-matrix reads cross PCIe per draw; every peer keeps static geometry in device-local memory and only
  dynamic data in a host-visible ring.
- **What the peers do.** UE RHI creates static VB/IB in device-local memory and uploads through staging;
  Diligent uses `USAGE_DEFAULT` + `UpdateBuffer`; Filament's buffer update goes through a staging copy;
  The-Forge separates an upload buffer from a default buffer; bgfx's transient streams are the per-frame
  variant, precisely because static geometry is not supposed to live there.
- **What to do.**
  1. Correct `ContentCaches.hpp:70` now (comment only).
  2. Backlog candidate `T67` (proposed, §5): give `ContentCaches::upload` a
     `MemoryUsage::Staged` path - a host-visible staging buffer plus a `vkCmdCopyBuffer` recorded at the head,
     published by the same `FrameBuilds::publishAll` evidence (the design already states that this is where
     the evidence moves). The copy sits where `ContentImages`' copies already sit (`P5`'s window) and uses
     `BarrierPlan`'s transfer rows, so no new synchronisation vocabulary is introduced.
  3. Let the allocator prefer a type that is BOTH device-local and host-visible when the device offers one
     (integrated, ReBAR) - that removes the staging copy on those machines, not just the PCIe reads.
  4. T35's performance reading must name the device and its memory types. A software-rasteriser number cannot
     decide a question whose whole cost is PCIe traffic.
- **`G#`**: `G10` (the cost is not costed), `G6` (one resource-lifecycle rule; images already follow it),
  `G4` (a sentence that says the opposite of the code).
- **Trigger**: the first measurement on a discrete GPU, or T35's gate being claimed on real hardware.

### 3.2 `K2` The shader-variant rule has no owner in the layer that owns the shaders

- **What the tree does.** `ProgramVariant` (`ProgramVariant.hpp`, `ProgramVariant.cpp:8-60`) derives four bits
  from a drawable - `textured` (any material slot), `vertex_color` (channel 2 present), `cube_texcoord`
  (texcoord width 3) and `instanced` (the command reads one) - and `bits()` fixes their NUMBERS (1/2/4/8).
  `PipelineKey::variant` is a `std::uint32_t` in the PUBLIC SDK key, and its own documentation
  (`DrawState.hpp:242-248`) says the ONE spelling of what the number means is the built-in shaders'
  `#pragma import_defines` line, "and a backend maps the number to the define names its compiler takes".
  But nothing in the SDK constructs a `PipelineKey` or computes the number: the only producer is this
  backend's `variantOf`.
- **Why it matters.** The RULE depends entirely on content facts the SDK owns (a material's slots, a
  geometry's channel widths, the instance count), and the shaders that gate on the defines are the engine's
  (`src/viz/graphics/shaders/builtin_forward.vert`). The backend's own note says the plugins "share no header
  and must agree by contract". With the owner's second-family premise (review 8 §4.7), a D3D12/Metal backend
  must restate "3 scalars is a cube" and "a slot is textured" - the exact class of divergence
  `ADR-2026-11-23` D1 deleted when it replaced "both backends use it" with "one promise, one owner".
- **What the peers do.** UE, Unity and Filament all put shader permutations/keywords with the material
  system, not with the RHI: the backend receives a permutation id and maps it to compiler inputs.
- **What to do.** Write the trigger into the backlog (`T68`, proposed): when the second API family starts,
  move the DERIVATION into the SDK (a `ShaderVariant` value or a `shaderVariantOf(geometry, material,
  instanced)` beside `PipelineKey`), state the bit assignment there once, and leave each backend only
  `defines()` (the bit -> define-name mapping is genuinely per compiler). Do not build it now (`G8`: one
  consumer), but do not let a second backend re-derive it either.
- **`G#`**: `G1` (one definition, one owner), `G5` (the change would cross a boundary already drawn wrong),
  `G9`.
- **Note**: `PassChain.hpp:258` cites a `VariantPool.hpp` that is not in the tree; that citation is either
  removed or repointed in the same batch.

### 3.3 `K3` Device choice ranks before it filters: "best device" and "a device that can serve the session" are asked in the wrong order

- **What the tree does.** `Device.cpp:2847-2893` walks the physical devices and keeps the best-ranked one
  (discrete > integrated > virtual > CPU) among those with a graphics queue and, when a surface exists,
  present support. ONLY THEN are the session's requirements checked: API 1.3 (`:2903`), the 1.3 features
  (`:3019`), `VK_EXT_extended_dynamic_state3` (`:3052`). Each failure reports and calls `shutdown()`; the
  next candidate is never tried.
- **Why it matters.** On a machine with two devices, the top-ranked one can miss the floor while another
  meets it, and the session refuses with "this device is older" while a capable device sits in the list. The
  capability report stays honest (`C7`); what is lost is the SESSION, and the diagnostic names a choice the
  backend made rather than a machine that cannot run.
- **What the peers do.** `vk-bootstrap`'s `PhysicalDeviceSelector` filters candidates by required
  extensions/features/API version FIRST and rates the survivors; Diligent's adapter enumeration takes a
  required-features set the same way; Unreal's Vulkan RHI keeps a fallback list.
- **What to do.** Fold the three checks into the candidate loop as one `candidateServesSession(...)`
  predicate, and rate only the survivors (reporting a rejected candidate at Debug, not Error). Factor the
  predicate so a unit case can drive it with fabricated `VkPhysicalDeviceProperties` / feature structs - two
  real devices are not available in CI.
- **`G#`**: `G4` (the refusal names the wrong subject), `G9`.
- **Trigger**: a two-GPU machine where the top-ranked device misses the floor; or the next batch that touches
  `initialize` for another reason (the change is local).

### 3.4 `K4` Three content kinds, three build-admission spellings - and the vocabulary only names one

- **What the tree does.** A geometry's build is arbitrated across drivers by `ResidencyClaims`
  (claim -> publish / withdraw, `vk-backend.md` §2.17). A texture's build is a per-driver COPY recorded at
  the head, held PENDING, promoted by `ContentImages::batchCompleted` and parked by `batchAbandoned`
  (`vk-backend.md` §2.21). A program/pipeline's build is neither: it happens inside the one cache lock under
  `ContentCaches::ensureProgram` / `ensureContentPipeline`, and its only state is a `failed` flag on the
  program entry (a refused pipeline is not cached at all, so it is retried on the next draw).
- **Why it matters.** The design states one lifetime rule for all content (`vk-backend.md` §2.18: "Content
  obeys the same rule (`G12`: one rule, two tables)"), and that is true of RELEASE. The BUILD half has three
  mechanisms with three vocabularies (claim/token, pending/promote, failed-flag). The next change that moves
  any build off the lock (`K5`) has to extend exactly this, and without a shared vocabulary it will grow a
  fourth spelling.
- **What the peers do.** With one render thread, UE/Diligent/Filament do not need build admission at all;
  their analogous state is "initialised / not initialised / failed". The useful comparison is `bgfx`'s
  transient vs retained distinction and `wgpu`'s "a resource is usable when the API says so" - both are one
  state machine, not three.
- **What to do.** Name the stages once in an internal header (for example `Absent -> Building -> Proven ->
  Parked`) and express each kind's predicate in those terms, with no behaviour change. When `T17` lands, the
  program/pipeline build takes the geometry's claim/token shape rather than a new one.
- **`G#`**: `G1`, `G6`, `G12`.
- **Trigger**: the first build that leaves the cache lock (`T17`'s batch); the naming can ride with it.

### 3.5 `K5` A cold build (glslang + `vkCreateGraphicsPipelines`) runs inside the device-wide cache lock

- **What the tree does.** `resolveFrameHead` (`Device.cpp:1656-1710`) takes `ContentCaches::Lock` once and
  calls `planScreen` / `planCommand` / `resolveTextures` for EVERY pass and command under it.
  `planCommand` compiles a program on a miss (`ensureProgram` -> glslang) and creates a pipeline on a miss
  (`ensureContentPipeline` -> `vkCreateGraphicsPipelines`). `prepareFrame` - the documented warm-up - goes
  through the same head and the same lock.
- **Why it matters.** The head is meant to be a bounded critical section (the design's "a steady frame's
  cache cost does not grow with its draw count" is measured), but a cold build is not bounded at all - a
  glslang compile plus a driver pipeline creation, several orders above a map lookup - and it is held under
  a DEVICE-wide mutex: another driver's frame head stalls behind it, and a host that warms two engines at
  once serialises them. `T17` ("no asynchronous entry; one load exceeds one frame's budget") names the
  absence of an async verb; it does not name the lock scope, and the lock stall appears even when no single
  load exceeds a frame's budget.
- **What the peers do.** Unreal precaches PSOs (`FShaderPipelineCache`) so the frame does not create them
  on first use; Diligent's `PipelineStateCache` creates PSOs off the render thread; Filament compiles
  materials offline (`matc`) before the engine runs; Granite compiles pipelines on worker threads and hands
  the result to the device.
- **What to do.** When `T17` fires, the fix shape is NOT an `async` overload: move the BUILD out of the lock
  behind a per-key admission (the geometry claim is the pattern), so a frame that finds a build in flight can
  either wait for that key (a program it must draw with) or record without it. State the cross-driver stall in
  `T17`'s text so its trigger is not read as "a hitch in one frame".
- **`G#`**: `G10`, `G11`.
- **Trigger**: `T17`'s (amended), plus "a second driver shares the device" as an earlier observable.

### 3.6 `K6` The recording path allocates per draw and re-states state after every pipeline bind

- **What the tree does.**
  - `ContentCaches::ensureContentPipeline` (`ContentCaches.cpp:923-1045`) BUILDS the key before it looks it
    up: `key.identity.vertex_layout.attributes` grows per channel (`:945`, `:954`), then `bindings` and
    `attributes` are reserved and filled (`:960-961`). `buildPipeline` looks the key up at `:736` - so a HIT
    still paid for three vectors and a vector compare.
  - `ContentDraw::record` allocates two vectors PER COMMAND for the vertex-bind lists
    (`ContentDraw.cpp:535-546`).
  - `ContentDraw::bindPipeline` (`ContentDraw.cpp:183-196`) resets the whole state shadow on every pipeline
    bind, because "a pipeline may declare a different dynamic set". Every pipeline this backend builds
    declares the SAME eleven dynamic states (`buildPipeline`'s `dynamic_states[11]`), so a bind invalidates
    nothing; alternating two pipelines re-states viewport, scissor, line width, depth and blend for no reason.
  - The design already measured the cost (`vk-backend.md` §9.1: 85 allocations / 10.6 KB per steady frame
    with one draw packet, 133 with four, "mostly small vectors: the pipeline key's channel/binding/attribute
    lists and the plan's per-draw scratch"), so this is not news; what is missing is a budget and a witness
    for it.
- **What the peers do.** Filament's command stream is a bump-allocated buffer with no per-draw heap;
  Diligent's draw calls take spans; bgfx records into a command buffer arena; UE's `FMeshDrawCommand` is
  cached per mesh and reused. Their state trackers emit a command only when the value changes.
- **What to do.** One small batch, no new mechanism: (a) give the recorder two reusable arrays (or fixed
  `std::array`s sized by the maximum channels) for the bind lists; (b) cache the geometry's
  `VertexLayoutKey`/attribute list in `GeometryEntry` so a pipeline HIT builds no key vectors (the key is
  then hashed from a span); (c) keep the shadow across a pipeline bind, because the dynamic set is a
  backend CONSTANT - assert it where the pipeline is built, and pin it with a case (a pass of two alternating
  pipelines states the rectangle once).
- **`G#`**: `G10` (a cost with a witness but no budget), `G11` (a repeated state command is noise in a
  capture).
- **Trigger**: none needed for (a)/(c) (contained); (b) rides with the next change that touches
  `ensureContentPipeline`.

### 3.7 `K7` A pass' READ set is resolved during recording, under the target map's lock, after the head

- **What the tree does.** `resolveFrameHead`'s note says "the recording path takes no lock at all"
  (`Device.cpp:1660-1663`). But inside the record loop, a full-screen packet's source is looked up at
  `Device.cpp:1975` and again at `:2050`, and `resolveShadow` looks the map up at `:1216` - all three under
  `TargetTable`'s map mutex. `claimPasses` claims the sampled target and the pass' declared inputs
  (`claim_sampled` / `claim_pass_samples`, `Device.cpp:1480-1540`) but does not resolve or keep their records.
- **Why it matters.** Two consequences of one boundary:
  1. A host `releaseRenderTarget` / `releaseContent` that lands between the claim and the lookup erases the
     record, and the frame that HAD CLAIMED the target loses it: a screen pass is refused with
     `ScreenNoSource` ("named no source target" - a wrong sentence for this cause) and a shadowed pass draws
     unshadowed with NO report. The design's rule is "a release may not take away what the frame resolved";
     a claimed-but-not-yet-looked-up target is the gap in that rule.
  2. The frame body therefore takes the target map's lock per pass/source, which is the one lock the
     head/record split exists to keep off the recording path.
- **What the peers do.** A render graph resolves every resource before execution and holds them for the
  frame's life; Diligent/Filament keep the resource alive by reference from the command list. The lesson that
  transfers is only "decide the read set with the write set", not the graph itself (review 5 §2.3).
- **What to do.** Resolve the sampled-source and shadow records in `claimPasses` (the walker already visits
  `draw.packets` and `entry.inputs`) and store them in the pass slot / `FrameRecording::read_records`; the
  record loop then uses values and takes no lock. One case: claim a pass, release its source before the
  record loop (the multi-session tests already have the release machinery), and require the frame still
  draws its picture.
- **`G#`**: `G3` (one flow), `G4` (what "claimed" means), `G10` (a lock and two lookups off the cheap path).
- **Trigger**: none; the fix is local and the case is writable with the existing fixtures.

### 3.8 `K8` The descriptor-set ceiling is combinatorial and stated nowhere (watch)

- **What the tree does.** A content set is cached under the full tuple - screen flag, five views, five
  samplers, the shadow view and its sampler (`FrameScratch.cpp`'s `SetKey`), swept after 600 frames.
  `set_builds` / `sets_live` are the witnesses. Changing ONE slot of a material rebuilds the whole set.
- **Why it matters.** The cost curve is (distinct texture tuples x two layouts), so a scene with many
  materials that each bind a different texture pair approaches the cache's cap, and every new tuple is a set
  build in a steady frame. Today the SDK has five slots and few materials, so `G8` says do not pre-pay - but
  the trigger is currently unstated, which means the first scene that hits it will look like a bug.
- **What the peers do.** Filament, Diligent and Granite all have a descriptor-indexing (bindless) path for
  hardware that supports it, and their non-bindless paths still update a per-material set rather than
  rebuilding one per texture tuple; UE5 makes bindless the default. `VK_EXT_descriptor_indexing` is not
  required by this backend, so the decision is a CAPABILITY decision (a new requirement or an optional
  path), not a refactor.
- **What to do.** Backlog candidate `T69` (proposed): trigger = a steady frame whose `set_builds` moves after
  warm-up, or `sets_live` approaching the pool's `kMaxCachedSets`; the cheap half (an update template, or a
  per-material set with one slot changed) is stated there too.
- **`G#`**: `G8`, `G9`.
- **Trigger**: as stated.

### 3.9 `K9` Two sentences contradict the mechanism they describe

- `ContentCaches.hpp:70`: "the residency step (M6) is what moves streams to device-local memory" - nothing
  does (see `K1`).
- `Device.cpp:1912-1916`: "ONE DESCRIPTOR POOL RESET PER FRAME ... the pool's sets are the per-draw uniform
  bindings". `FrameScratch.cpp:553-557` states the opposite (sets are freed per entry, "rather than reset
  every frame, because a cached set has to outlive the frame that built it"), and `beginFrame` only resets
  the ring cursor. The stale sentence invites exactly the change `N1` forbids.
- **What to do.** Correct both sentences in the review's batch (comments only). They matter more than usual
  because each is the summary a reader gets of a mechanism whose number is measured elsewhere.
- **`G#`**: `G4`, `G11`.

## 4. What does not transfer (the `G8` half of the benchmark)

| Peer idea | Why it does not transfer today |
|---|---|
| Several frames in flight | `ADR-2026-11-15`: the measured ceiling is about 10%, and the price is the "read when drawn" contract |
| VMA-style suballocation library | `D1`: the hand-written pool covers every consumer; a new dependency must be paid for |
| A dedicated transfer queue / ownership transfer | `F11`: the gain under one synchronous submission is tiny |
| A render graph with transient aliasing | review 5 §2.3: the chain is a known, declared set; the transients are named outputs |
| RID-style opaque handles | review 5 §2.4: ownership in the signature is the framework's value proposition |
| An exposed command-list API for recording | the SDK's answer is one frame body per driver; no consumer wants to record a list itself |
| Bindless descriptors | `K8`: five slots, few materials; the trigger is recorded, not paid for |
| Indirect / GPU-driven draws | the SDK's unit is one item = one draw; no consumer needs a GPU-built command stream |
| Timeline semaphores | one queue, binary semaphore + fence is the whole model (`ADR-2026-11-15`) |
| Offline shader compilation (shipping SPIR-V) | a product decision the owner deferred (review 8 `P4`); the backend's job is to make the compile's COST visible (`K5`) |

## 5. Proposed order

The order this round proposed, so the next batch had a starting point; the status of every row is stated below the
table.

| Step | Item | Kind | Gate / witness |
|---|---|---|---|
| 1 | `K9` the two false sentences | comment only | doc gates; no build needed |
| 2 | `K7` resolve the read set at the claim | local change | a new multi-session case (release between claim and record); `test_vk` |
| 3 | `K6` (a) + (c): reusable bind arrays, keep the shadow across a bind | local change | `VkContentDrawTest` gains "a pipeline switch states the rectangle once"; `state_commands` delta |
| 4 | `K3` device choice filters before it ranks | local change | a predicate case over fabricated properties; `vk.init.api_version_low` still reported when NO candidate serves |
| 5 | `T17` amended with `K5`'s lock-stall sentence, `K4`'s vocabulary as its first step | backlog text + a naming header when it lands | `T17`'s own trigger |
| 6 | `T67` (`K1`) device-local streams, `T68` (`K2`) SDK-owned variant rule, `T69` (`K8`) descriptor ceiling | backlog candidates (listed for approval, not added) | their own triggers |

**What happened afterwards (2026-11-25).** Steps 1-4 landed: the two `K9` sentences were corrected, `K7` resolves the
read set with the write set (`FrameDriver::resolveReadSet`, the records kept by `FrameHead::Pass`, and
`VkMultiSessionTest.AReleaseBetweenTheClaimAndTheRecordStillDrawsTheSampledSource` forces the interleaving), `K6` (a)
and (c) landed (`VkContentDrawTest.TwoAlternatingPipelinesRestateNoState`), and `K3` became
`DeviceChoice.hpp`'s `candidateRefusal` with `VkDeviceChoiceTest`. Step 5 landed as TEXT:
`../backlog/graphics.md`'s `T17` now states the cross-driver stall. Step 6 landed as the numbers this page proposed
(`T67` / `T68` / `T69`) - `T66` was already a CLOSED item, so the stream gap had to take the next free number.

## 6. Verification recipes

| Check | What it proves |
|---|---|
| `python3 scripts/check_ai_docs.py --strict` + `check_design_docs.py` + `check_cross_refs.py` | this page's references resolve; the index names it |
| `ctest -R test_vk` | the whole backend suite after any of steps 2-4 |
| the new `K7` case (release after claim, before the record loop) | the frame keeps what it claimed; the record path takes no target-map lock |
| the new `K6` case (two alternating pipelines in one pass) | one viewport/scissor statement, not two; `state_commands` grows by the pipeline binds only |
| the new `K3` predicate case | a capable second candidate is chosen; a machine with no capable candidate still refuses with the floor's code |
| `MemoryAllocator` observation when `T67` lands | on a device with two memory types, a stream's slice is device-local and the copy is in the frame's upload batch |
| T35's performance reading (real hardware) | the number names the device and the memory type its streams live in |

The REVIEW ran no build: the tree was clean at `522ce5b4`, and every finding is a reading of the contracts and the
paths that implement them. The batch that followed it (same day) built `test_vk` and ran the whole suite green, and
ran the three documentation gates - the recipes above are what it used.

## 7. References

### 7.1 Code

- `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/`: `ContentCaches.hpp` (the memory-model
  sentence, `:70`), `ContentAbi.hpp` (`createHostBuffer`, `:150-175`), `ProgramVariant.hpp`,
  `DrawState.hpp` (`PipelineKey::variant`, in the SDK), `FrameScratch.hpp`, `TargetTable.hpp`,
  `ResidencyClaims.hpp`, `ContentImages.hpp`.
- `src/plugins/gfx_backend_vk/src/internal/`: `ContentCaches.cpp` (`upload`, `:275-293`;
  `ensureContentPipeline`, `:923-1045`; `planCommand`, `:1068`), `ContentDraw.cpp` (the bind lists, `:535`;
  `bindPipeline`, `:183-196`), `Device.cpp` (the head, `:1656-1710`; the device choice, `:2847-3053`; the
  read lookups, `:1216`, `:1975`, `:2050`), `FrameScratch.cpp` (the set cache and its pool note,
  `:553-557`), `MemoryAllocator.cpp` (`findMemoryType`, `:39-63`), `ProgramVariant.cpp`.
- `tests/test_vk/`: `VkFrameHeadTest`, `VkUniformRingTest`, `VkMemoryPoolTest`, `VkMultiSessionTest`,
  `VkContentDrawTest`, `VkPrepareUploadTest`, and the case this round's batch added (`VkDeviceChoiceTest`).

### 7.2 Documents and ADRs

- `vk-backend.md` §2.4, §2.11, §2.16, §2.17, §2.18, §2.20, §2.21, §3.1.10, §9.1 - the mechanisms this round
  reads.
- `graphics-sessions.md` §3.1.4 - the honest statement that the fill is a host memcpy today.
- `graphics-content-lifecycle.md` 2.8 - the image staging rule `K1` asks streams to follow.
- `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`,
  `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`,
  `../decisions/ADR-2026-11-24-residency-borrowing.md`,
  `../decisions/ADR-2026-11-23-retiring-the-reference-backend.md` (D1's promise rule, D4's floor).

### 7.3 External material

- Filament's driver API and material compiler (`matc`) documentation: a recorded command stream and offline
  material compilation.
- Diligent Engine's `IRenderDevice` / `IDeviceContext`, `USAGE_DEFAULT` + `UpdateBuffer`, and pipeline-state
  cache documentation.
- Unreal's RHI documentation on command lists, the RHI thread, PSO precaching and RHI-resource deferred
  deletion.
- Granite's render graph, reflection-derived descriptor/pipeline layouts and timeline-semaphore submission.
- `vk-bootstrap`'s `PhysicalDeviceSelector` (filter by requirements, then rate).
- The Vulkan specification: memory types and `bufferImageGranularity`, pipeline caches, dynamic state.
