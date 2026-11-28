# Graphics review 25 (2026-11-28): the backend's concepts, and the promises they carry

> **Status**: two fixes landed (`B1` colour-attachment ceiling, `B2` line-width gate), one finding recorded
> (`T78` point-size ceiling), and the backend's unit map written down so the next round does not have to rebuild it.
> `test_vk` **244/244** (one new case), `test_graphics` 714/714, doc gates green.
>
> **Scope**: the shipped backend as CONCEPTS rather than as behaviour - each internal unit's concept, the state it
> owns and the lock it holds - and then the PUBLIC promises the backend makes (a capability, a diagnostic, a plan),
> asking for each: is the promise carried by the unit that owns the fact?
>
> **In one line**: the unit map holds; the defects were all in the promises - two limits answered wider than the
> device can serve (colour attachments, line width), and one rasterisation state with no answer at all (point size).

## 1. Why this round, and what "a concept" means here

Reviews 19-21 read the backend's behaviour (lifetime, synchronisation, the dynamic scene); review 23 converted the
pages that describe it. What had not been written down is the backend's own **map**: which unit owns which concept,
and which state travels with it. Without that map every later finding is re-derived from the files, and a fact kept
in two places reads clean until someone compares them. The round therefore did two passes:

1. **The units**: for each internal header, one sentence of "what it IS", its state, and its lock. The table below
   is the result; nothing in it changed, which is itself the finding (the composition the sessions design states is
   the composition the code has).
2. **The promises**: `Capabilities` (a limit is a promise about what the device will accept), the diagnostic causes
   (`ContentRefusalCause`), and the plan values (`FrameHead`, `PassTarget`). For each promise, the unit that owns
   the fact behind it was asked whether it can keep the promise on every device the backend accepts.

## 2. The unit map (short form)

| Unit | The concept it owns | The state that travels with it |
|---|---|---|
| `Device` (`Impl`) | the session: instance, device, queue, clock, retirement, window system | every device-level service and its lock table (§3.1.9 of `graphics-sessions.md`) |
| `Device::FrameDriver` | ONE driver's frame machinery: command buffer, fence, scratch, recorder, frame head | the per-driver books, the programs' default, the episodes (`ReportOnce`), the `FrameGate` |
| `ContentCaches` | the device's CONTENT: compiled programs, uploaded streams, pipelines, and the planner | the four tables, `ResidencyClaims`, `FrameBuilds`, the lock every frame head takes |
| `ContentImages` | sampled images, samplers, per-driver upload batches | the texture table, the sampler intern, the batch books |
| `FrameScratch` | the frame's own constants and descriptor sets | the ring, the pool, the set cache |
| `ContentDraw` | the recorder: one packet's values become commands | none of its own (borrows the caches and the scratch) |
| `TargetTable` | off-screen targets as ONE object: map, claims, builds, release | the records, the claim books, the refusal episodes |
| `TargetObjects` | one off-screen target's Vulkan objects | images, memory, views, layout records |
| `PassTarget` | where ONE pass draws: window or off-screen, as one handle | extent, compatibility, holds-picture, the rewind |
| `WindowTarget` / `WindowSystem` | a window's swapchain / the connection surfaces are made on | the surface, images, depth; one live target per window |
| `BarrierPlan` / `DynamicRendering` | the pass' transitions / the pass' begin-end values | plan entries; attachment arrays |
| `MemoryAllocator` / `RetirementQueue` / `DeviceClock` | the pool / the park / the frame-window book | blocks and slices; parked acts; windows |

The three "claim" mechanisms earn their separate names because they answer three different questions, and the round
confirmed each is asked by its own unit: `ResidencyClaims` = *may this driver BUILD this key* (content),
`TargetTable::Lease` = *may this driver WRITE this target now* (passes), `FrameBuilds::borrow` = *does this frame
KEEP what it resolved alive* (lifetime). Merging any two would put one answer in two units (`G1`).

## 3. The findings

### 3.1 `B1` - the colour-attachment ceiling was the API's, not the device's (fixed)

`Capabilities::limits.max_color_attachments` answers the DEVICE's `maxColorAttachments`, and its own doc states the
contract: "a declaration asking for more is refused by the backend that would have to build the attachment, with a
diagnostic naming the count". The refusal in `TargetTable::ensure` compared against the API's `kMaxColorAttachments`
(8) alone - so on a device that reports 4 (Vulkan only GUARANTEES 4; mobile GPUs commonly report 4), a target with
5..8 colour attachments PASSED the check, had its images created, and reached `VkRenderingInfo` with a count above
`maxColorAttachments` - an invalid pass, and a count the recorder's eight-wide stack array cannot even name.

- **Fix**: `TargetTable::Limits::max_color_attachments` (the device's answer, defaulted to the API ceiling) and the
  check is `min(device, kMaxColorAttachments)`; the message and the `DiagnosticLimit` pair name the request against
  the ceiling really applied. `Device::initialize` passes `caps.limits.max_color_attachments`.
- **Witness**: `VkContentTargetTest.ATargetAboveTheReportedColourAttachmentLimitIsRefusedNotDrawn` - a target with
  `capabilities().limits.max_color_attachments + 1` colour attachments: the frame still answers `Skipped`, exactly
  one `vk.target.too_many_colors` is reported with `requested = ceiling + 1` / `supported = ceiling`, and the
  readback answers `NotReady` (nothing was built). The request is one over the REPORTED limit, so the case holds on
  any device - on this machine (8) it exercises the `min`, on a 4-attachment device the branch a hard-coded 8 missed.
- **Mutation**: ceiling loosened by one (`+ 1U`) - the target is accepted, no diagnostic is named and the case reddens
  at its refusal assertion (run and restored by reverse patch).

### 3.2 `B2` - a limit is only reachable with the feature that legalises it (fixed)

`max_line_width` answered `lineWidthRange[1]` unconditionally, while `max_anisotropy` in the same block is gated on
its feature bit - the two were written from different rules. A device whose `wideLines` feature is off draws 1-pixel
lines whatever the range says, so the answer promised a range the device cannot serve. The limit now follows the
same gate as anisotropy (`wideLines ? lineWidthRange[1] : 1`, `1 = fixed` being the spelling the field documents).

- **No witness on this machine, recorded**: lavapipe offers `wideLines`, so the false answer cannot be produced
  here; the change is a reading fix like `T72`'s platform branch, and the comment in `Device.cpp` states the rule
  where it can be checked.

### 3.3 `B3` - a point size has no ceiling at all (recorded, `T78`)

The census of the rasterisation qualities: anisotropy has a feature AND a limit AND a clamp in `samplerFor`; line
width has a feature (`wideLines`), a limit (`max_line_width`) and a clamp where the width is recorded
(`vkCmdSetLineWidth`); **point size has a state** (`RenderState::pointSize` → `VineDrawBlock::params.y` →
`gl_PointSize`), **a feature** (`largePoints`) and **no limit and no clamp** - the driver silently clamps to
`pointSizeRange` and a host offering a size slider has nothing to read. `Capabilities::Limits` is the vocabulary
for exactly this, and the fix costs one field (`VN_RENDER_BACKEND_ABI_VERSION` bump) - so it waits for the consumer
its trigger names: a host that draws `Topology::Points` with a user-chosen size (the point-cloud viewer `T23`).

## 4. What deliberately stays

- **The caches/scratch/recorder split** (`ContentCaches` = device, `FrameScratch` = driver, `ContentDraw` = neither):
  three lifetimes, three owners, and the locks follow the ownership - a second driver is "one more scratch", not
  one more cache.
- **`TargetTable` as one object** (map + claims + builds + release): the page's own history - three owners that
  could not answer "who may build this" without reading all three - is the reason it must stay one.
- **`ContentRefusalCause` as an enum with a `default`-less switch**: a new cause cannot miss its code.
- **The refusal episodes** (`ReportOnce` per driver / per table): a refusal is one line per episode, and the episode
  lives beside the state that knows when it is over.

## 5. Gates

- `test_vk` **244/244** (one new case; the three refusal episodes of the table re-run through the whole suite),
  `test_graphics` 714/714, `test_appfw` unaffected.
- The mutation of §3.1 run and restored by reverse patch; `grep MUTATION` empty.
- Documentation gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`,
  `check_doc_symbols.py`.

## 6. What the next round should open

- `T78` waits for its host; `B2`'s platform branch can only be witnessed by a device without `wideLines`.
- The unit map above is the index for reading a future finding: start from the unit that owns the fact ("who owns
  the attachment count?" - the table; "who owns the width?" - the caches + the recorder), and check the promise at
  its owner rather than at the caller that reported it (`G11`).
- Unread with this index: `BarrierPlan`'s transition vocabulary against `DynamicRendering`'s pass values (one pass,
  two files, one question - "which layout does this image hold at this point") is the next concept seam.
