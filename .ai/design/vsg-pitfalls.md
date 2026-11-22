# the vsg backend's trap catalogue (P1–P89)

> **Status**: current
> **Last verified**: 2026-11-23
> **Scope**: the traps the vsg backend has hit, in nine risk areas; each entry gives its symptom and its criterion.
> **Sources**: `src/plugins/gfx_backend_vsg/**`, `docs/guides/graphics-backends.md` (the user page), `.ai/bugs/`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

What these traps have in common: the code runs, the suite is green, the picture looks right — and the fact is wrong. They used to be crammed into
`vsg-backend.md`'s chapter 6, which pushed the architecture page to 427 lines; anyone changing one piece of state had to read all 82 entries to find the six that mattered
to them.

### 1.2 Goals

- One risk area per group, so a change reads only its group.
- Each entry = the trap + its symptom / criterion; prefer criteria a machine or an arithmetic case can catch.
- Stable numbers: an existing `P#` is never renumbered, new ones are only appended.

### 1.3 Non-goals

- Not repeating `vsg-backend.md`'s architecture, contract or key definitions.
- Not recording a single bug's symptoms and postmortem — that is `.ai/bugs/`' job.
- Not organised by time or by review round.

### 1.4 One-paragraph conclusion

> Before changing the backend, read the group your change touches; every entry states its symptom, so seeing the symptom tells you which entry it is.

## 2. Design Points

### 2.1 Grouped by risk area, not by time

A reader looks traps up by "what am I changing", not by "which day did we hit it". An area is a layer a change touches: protocol, keys, orientation and depth, ABI, streams, target lifetime, WSI, evidence,
the engine side. The timeline belongs to git.

### 2.2 Every entry needs a symptom that decides

"Be careful with X" has no symptom, and the reader cannot tell whether they have already hit it. A symptom must be something visible: a VUID, a counter, a pixel percentage, a refusal. The criterion must be
reproducible by a gate or an arithmetic case; a pixel can only prove "not broken", never "correct".

### 2.3 Stable numbers, append only

`P#` is cited by code comments, ADRs and commit messages, so an existing number never changes. An entry disappears when its criterion moves into a test: the criterion stays, the entry goes, the number is
not reused.

### 2.4 The division of work with the architecture page

`vsg-backend.md` says what the system is now; this page says what happens when a step is skipped. When the same thing appears on both sides, the fact belongs to the architecture page and the symptom
to this one, and neither is copied into the other.

## 3. Features

### 3.1 Risk-area index

| # | Area | When to read it | Traps |
|---|---|---|---|
| F1 | protocol and frames | changing the call order of `beginFrame` / `beginPass` / `swapBuffers` | P1–P6 |
| F2 | keys and pipelines | adding a field to `PipelineKey`, changing dynamic state | P7–P12 |
| F3 | orientation, clipping and depth | touching projection, cull, the depth range, a fullscreen draw | P13–P22 |
| F4 | blocks and descriptors (ABI) | changing a uniform block, a descriptor layout, push constants | P23–P35 |
| F5 | streams, geometry and aliasing | changing stream identity, an index key, alias registration | P36–P42 |
| F6 | targets, lifetime and device idleness | changing target creation / destruction, borrowed depth, readback | P43–P53 |
| F7 | WSI and windows | changing present, resize, the window size, the test window | P54–P60, P87–P89 |
| F8 | evidence, gates and the build | adding a case, changing a gate, adding a `.cpp` | P61–P75 |
| F9 | the engine side (the scene bridge and shading conventions) | changing declarations / bindings, materials, colour space, the G-buffer | P76–P86 |

## 4. Public Interface

### 4.1 How to look things up

| Entry point | Use |
|---|---|
| `P#` | the stable number; code comments, ADRs and commit messages cite it instead of "that earlier trap" |
| 6.1–6.9 | the nine groups matching §3.1's F1–F9 |
| symptoms | §4.2 looks them up by what you see |

### 4.2 Looking up by symptom

| Symptom | Look at |
|---|---|
| the next `beginFrame` is refused | P1 |
| the pixels match, but the validation layer reports a format / layout VUID | P10, P35 |
| a whole piece of geometry suddenly disappears | P13, P18, P28 |
| every cull mode acts on the wrong faces | P16 |
| the descriptor set changed and the picture did not | P25, P26 |
| the buffer bytes changed and the picture did not | P36, P37 |
| after destruction the device reports 00765 / the device never reaches idle | P43, P44 |
| the suite is green and the VUIDs come in a stream | P61, P35 |
| the background has a hole when the camera is close | P21 |
| the cube map's faces are in the wrong order without an error | P34 |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | Every entry must give a symptom or a criterion; an entry that only says "be careful" does not enter the table. |
| C2 | Prefer criteria a machine can see: the validation layer, counters, arithmetic cases; pixels are a supplement only. |
| C3 | A new trap joins an existing area; only a genuinely new area adds a row to §3.1. |
| C4 | A trap appears in one area only; a cross-area one is cross-referenced by number. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | Cite `P#`, never "that trap from last time". |
| R2 | Fixing a trap adds its criterion first, then deletes the entry. |
| R3 | An entry's symptom describes what you see, not "what you did wrong". |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | `P1`–`P82` are fixed numbers; a deleted entry's number is not reused. |
| N2 | §6's nine groups are in the same order as §3.1's F1–F9. |

## 6. Caveats and Traps

### 6.1 Protocol and frames

| # | Trap | Symptom / criterion |
|---|---|---|
| P1 | closing the frame is `swapBuffers()`; `endFrame()` only leaves the pass | forget it: the next `beginFrame` is refused and `description()` is still the old frame |
| P2 | an empty frame is still a frame | an empty **pass** is not a pass; an acquired image must be walked by one render pass, or present reads UNDEFINED |
| P3 | borrowed arguments must be snapshotted on the spot | the engine's `resolved_inputs_` is a reused member; holding it = reading the next pass' data |
| P4 | a draw without a scope is refused and reported | the scope attribute belongs to the pass; per-draw attributes serve the next call only |
| P5 | a "warm-up" render outside `beginFrame` is not collected | the protocol refuses it and returns silently |
| P6 | a cycle is handled by the engine's scheduler | the backend builds no dependency graph; reintroducing one = a second ordering |

### 6.2 Keys and pipelines

| # | Trap | Symptom / criterion |
|---|---|---|
| P7 | do not put anything dynamisable into the key | a StateNode change recompiles; the key audit (`created` does not move) |
| P8 | do not leave the view that will be replaced inside a node | a resize rebuilds the whole batch of program slots; the key audit |
| P9 | topology may enter only as a "class" | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` allows switching within a class only; across classes = a new pipeline |
| P10 | the format enum is projection | sRGB / linear and RGB / BGR are part of compatibility; missing it ⇒ 02684 while the pixels still pass |
| P11 | the pool holds 65 (FIFO) | after an eviction the same key is `created` again; capacity and eviction are part of identity |
| P12 | block roles are claimed by type name | giving a role by binding index reads someone else's block |

### 6.3 Orientation, clipping and depth

| # | Trap | Symptom / criterion |
|---|---|---|
| P13 | the Reverse-Z trio | far plane 0.0, comparison GREATER, depth cleared to 0.0; clearing 1.0 + GREATER = everything dropped |
| P14 | the view block folds `proj` / `view_proj` only | x/w unchanged, y negated, z written as `0.5 - 0.5*z`; `view` / `inv_view` are not folded |
| P15 | a wrong fold does not show | with z mirrored the picture still passes its test; an arithmetic case and a pixel case each cover one half |
| P16 | front face = CLOCKWISE | copying CCW makes every cull mode act on the wrong faces (silently) |
| P17 | blend is always on for single-attachment content | `blend.enabled` only selects factors; MRT / fullscreen draws do not blend |
| P18 | a fullscreen call does not test depth | the canonical triangle's z = 0 = the Reverse-Z far plane; testing it makes the whole block disappear |
| P19 | a fullscreen push must sit right next to the draw, inside `Commands` | putting it in stateCommands reads the identity matrix and the picture "looks normal" |
| P20 | the push's stage is half the ABI | the wrong stage is 0 VUIDs, silently |
| P21 | the background criterion is "has this pass written it" | not `dot(pos,pos) < 1e-6`, which leaves a hole when the camera is up close |
| P22 | vertex counts / ABI declarations are invisible to pixels | the geometry is clipped by the viewport or draws the same picture; the criterion is an arithmetic case |

### 6.4 Blocks and descriptors (ABI)

| # | Trap | Symptom / criterion |
|---|---|---|
| P23 | `VineMaterialBlock` is an aggregate | `{}` does not touch the tail padding; use a per-member `operator==`, not a byte comparison |
| P24 | a dynamic offset must be aligned | a multiple of `minUniformBufferOffsetAlignment`; a violation gets no command and a count |
| P25 | do not bake the per-frame offset into a descriptor set | the pool hands the same handle back next frame while the previous frame's commands still use it (03047) |
| P26 | replacing a descriptor set = the old one must enter the retirement window | the pool hands a freed set's handle to the replacement; `vkUpdateDescriptorSets` lands on a pending handle |
| P27 | a layout must declare both kinds of binding (block + sampled) | the gap must be a real layout; a null pointer ⇒ an invalid handle ⇒ a segfault |
| P28 | "declares nothing" means "binds nothing" | a program that declares no block has no set to bind; do not bind the block set unconditionally (00360) |
| P29 | a declared push that is never filled ⇒ refuse that half | "the screen is black because the matrix is zero" is not a diagnosis |
| P30 | every binding of a fullscreen set comes from the program's declaration | hard-coded binding numbers read undefined data |
| P31 | compute std140 sizes from the alignment | adding member bytes misses the padding (52 → 64) |
| P32 | the `#ifdef` branch's condition decides the fact | treating every define as taken ⇒ the same `(set,binding)` with two types ⇒ Malformed |
| P33 | the white fallback image must go through an upload | with no recorder the image stays UNDEFINED and the descriptor lies = 09600 every frame |
| P34 | a material's texture kind must match its sampler | a cube map interleaved per face wrongly is silent (the byte total is the same) |
| P35 | the criterion for layout / compatibility claims is the validation layer | the pixels are all green and the VUIDs come in a stream (measured four times as r/ah/ai/al) |

### 6.5 Streams, geometry and aliasing

| # | Trap | Symptom / criterion |
|---|---|---|
| P36 | stream identity = buffer + **that buffer's revision** + segment | a geometry revision only rebuilds the fact rows; changing bytes needs `Buffer::setRevision` |
| P37 | a refilled buffer is a new stream | an old alias must not point at new bytes; moving the revision out of identity = silently old data |
| P38 | derived channels cannot be shared | the white carrier, zero UVs and normal bytes belong to one geometry |
| P39 | all channels of one geometry start from the same vertex | otherwise "index 0" names a different vertex per channel |
| P40 | the index key is normalised to the whole buffer | slices share one upload; the alias criterion is the same bind object |
| P41 | alias registration does not own bytes | eviction ≠ release; "is it still there" asks only whether a live geometry row still declares this buffer@rev (a pin); leaving goes through parking, with no time window and no capacity |
| P42 | constructing an `intrusive_ptr` from a stack object is an invalid free | SDK objects must live on the heap, owned by an `intrusive_ptr` |
| P42.1 | replacing the `TransferTask` without two hand-overs **silently drops uploads** | the new task must inherit `transferQueue` (otherwise `transferData()` crashes outright), and `device->transferTask` must be repointed at the new object (enqueueing goes through the device's observer while the task table runs the task; not repointing = the upload lands in a task nobody records and the frame looks completely normal). Also: it cannot be swapped while a copy is in flight (`containsDataToTransfer(TRANSFER_ALL)` true ⇒ refuse), and it only holds for "one viewer, one record task" (vsg's several tasks each have their own `TransferTask`; swapping one forks the uploads). |
| P42.2 | deriving from `vsg::TransferTask` reads **and writes** its protected members | the staging release relies on `detail::StagingTask`: it reads each block's `staging` / `buffer_data` / `fence` from `_earlyDataToCopy` / `_lateDataToCopy` and **empties the first two**. Emptying them is exactly where vsg's own "too small, rebuild" path starts (`!staging` → create + map), so the next upload reallocates along its path; but two constraints cannot be skipped: ① clear only when `fence->status() == VK_SUCCESS` (otherwise memory still being read is destroyed); ② do not add "skip when the DataToCopy still has pending copies" to the sweep — such a table can stay non-empty for a long time with nothing pending (`_transferData()` returns early and cleans nothing), and once the condition holds it freezes for ever. An upstream rename = a compile error; a semantic change is caught by `OffscreenSessionTest`'s byte-count assertions. |

### 6.6 Targets, lifetime and device idleness

| # | Trap | Symptom / criterion |
|---|---|---|
| P43 | destruction has only two paths | replacing the whole session (preceded by one counted device idle) or a retirement dequeue; otherwise 00765 |
| P44 | the retirement window = slots + 1 | if the slot count is not learned ⇒ `retire()` returns false and falls back to a counted device wait |
| P45 | a dropped frame = the content is untrustworthy | the next plan answers `Repair(Bootstrap)`; only a clear fixes it, LOAD does not |
| P46 | `built` = can LOAD = `written` | a new target's first pass must clear; `written` must be reset |
| P47 | the load op is the swapped half, not part of the key | but the dependency masks must never vary with the variant (a variant is not even framebuffer-compatible) |
| P48 | only a shape change is a real rebuild | check the formats too; compatibility claims are invisible to pixels (00904) |
| P49 | borrowed depth: build no image, use the lender's view | `borrowed` is never sampleable; the capture barrier must name the real current layout (01197) |
| P50 | readback is a classification table | classification comes before any work; "never" and "not yet" are different answers; colour packs RGBA8 only |
| P51 | the plan is checked item by item against the resource world | the target shape, the input count and each input's colour count; a mismatch refuses the whole pass and names the item |
| P52 | `written()` is also true for "recorded but not submitted" | the fact is "someone recorded into it"; good enough for the bootstrap decision |
| P53 | the window's answers come from the platform | live sampling and resizing come from the platform / surface; the plan's claims are not adopted |

### 6.7 WSI and windows

| # | Trap | Symptom / criterion |
|---|---|---|
| P54 | presenting to a window that is not viewable yet never arrives | reading it gives `BadMatch` (readError 8); construction waits for viewable, and a test presents after settling |
| P55 | read the window from the id in the backend log | the name / Qt container reads chrome that is always lit; `xwinresize.py` resizes the top-level parent |
| P56 | present is asynchronous | reading right after commit may get the old back buffer (all black) ⇒ run two more frames |
| P57 | changing the frame graph every frame tears down in-flight fences / semaphores / command buffers | measured 12 VUIDs; do not `assignFrameGraphs` every frame in a steady frame |
| P58 | the session must survive a dropped frame on its own | an acquired but unpresented image leaves the WSI one short; the session gives the image back |
| P59 | the startup size is not fixed | the window grows with the log dock; a gate drags the window to a fixed size before judging |
| P60 | starting the application needs `exec` and a kill by name | `pkill -x Vine`; a leftover window covers the test window |
| P87 | a Win32 pixel read is a COMPOSITOR read, and the compositor lags the present | a read right after a present answers the previous picture (the window's own black background for the first content); the fixture waits for a DWM composition pass (`DwmFlush`) and a case that asserts a specific colour waits for THAT colour (`waitForPixel`), not for "some picture arrived" |
| P88 | a Win32 test window's thread must pump its messages | a window created late in a long run never receives new content (the compositor stops serving a thread that never pumps); the fixture pumps the thread's queue inside every read |
| P89 | the Win32 read hands back the surface's BYTE ORDER, and a GDI-painted window's order is not a Vulkan one's | a three-channel assertion fails by a swap (blue where red was expected); assert a single channel, or accept both orders - `isColour` / `isRed` do |

### 6.8 Evidence, gates and the build

| # | Trap | Symptom / criterion |
|---|---|---|
| P61 | "the test passed" is not a conclusion | the suite is green while the validation layer keeps reporting VUIDs; the gate counts VUIDs / SYNC-HAZARD and fails on a skip |
| P62 | a false green must be caught by the gate itself | forgetting `swapBuffers`, an empty pass, forged VUID / skip lines; the criterion must not fool itself |
| P63 | the phase table | one line = assembly → assertions (pixels + counters) → the expected counter delta (including the invariants) |
| P64 | the steady-state criterion is "two frames allocate nothing" | one frame allocating nothing is that frame's fact only |
| P65 | a mutation proof must be verified itself first | a mutation where two frames share an identity, or a field is always zero, does not go red |
| P66 | three picture criteria for the application stage | not-near-black ≥30%, the preview's max channel ≥64, a flat background ≤5%; the VUID whitelist is empty |
| P67 | the plugin's source list is a GLOB | a new `.cpp` needs a re-configure; add it explicitly in two places in `tests/test_vsg/CMakeLists.txt` |
| P68 | the demo host is a plugin | `ninja -C build Vine` does not rebuild it; after changing the demo run `ninja -C build demo_plugin` |
| P69 | `VSG_MAX_DEVICES` = 4 | `<1` / `>4` are broken in 1.1.16; a fifth surface must be refused by us (no guard today) |
| P70 | `vn::String` is `std::u8string` | literals are `u8"..."`; a default `Mat4d` is the identity; `vsg::Image::vk(deviceID)` needs the subscript; `vsg::Exception` is not a `std::exception` |
| P71 | three living documents are governed by `check_doc_symbols.py` | a named unit must exist; a section headed "historical register" is exempt as a whole |
| P72 | the evidence gate needs a full build first | building only one target leaves stale binaries; the "evidence" is not trustworthy |
| P73 | four guardrails for scripted bulk edits | write back once after merging the cut segments; report the misses; do not patch a file you just generated; `bash -n` a gate before trusting it |
| P74 | `SRC_FILE_LIST` uses tab indentation | space matching always fails |
| P75 | clang crashes transiently now and then (exit 135) | rerun first; after a clean rerun it still goes through the full criterion, never counted as "passing" |

### 6.9 The engine side (the scene bridge and shading conventions)

| # | Trap | Symptom / criterion |
|---|---|---|
| P76 | declarations and bindings are both by name | an undeclared `assign*` is a silent no-op; "it is connected only once it is declared" |
| P77 | `vn_skyMap` and `vn_baseColorMap` bind the same | the engine's own sky program takes the material's base-colour slot as the sky; a kind that does not match its sampler |
| P78 | the shadow block is chosen by "target claim" | the same light identity + depth sampleable + a producer-published matrix; not "the first depth-sampleable input" |
| P79 | the colour-space contract | the engine is linear inside; colour maps are `*Srgb`, data maps `*Unorm`; encoding happens only at the hardware's two ends |
| P80 | the material block is a 4×16 B PBR row | the row order is `base_color / emissive / params / flags`; a hand-written shader copying the old members reads the wrong offsets (`diffuse` reads the emissive at offset 16) |
| P81 | the G-buffer albedo uses 16F | 8-bit dark ramps band under bright light; they are the same when `intensity ≤ 1` |
| P82 | an episode belongs to the caller | the half / dropped-light / refusal reports each have their own; `ContentPass` is created per frame, so state on it re-reports every frame |
| P83 | a missing slot's stand-in image only guarantees "it binds" | a missing slot is decided by the block's flags, **not** by the stand-in's value: a short-circuited normal slot with a white image tilts it 45°, and an emissive slot lights the object white | draw a material with a missing normal / emissive next to one with images: if the first looks different, the short circuit did not take | the only place that reads a slot is `if ((vn_material.flags.x & …) != 0u)` in `vine_material.glsl` |
| P84 | the green direction of a normal map | glTF normal maps are OpenGL-oriented (image +Y up) while this engine's v points down (Vulkan's origin is the top row); copying another engine's tangent basis mirrors the whole map — the lighting looks "off" with no error | a flat normal map must not change the appearance (P4); a Y-tilted map must tilt "up" / "down" on the horizontal and the vertical plane respectively | flip green after sampling, and take the tangent basis from the one copy in `vine_material.glsl` |
| P85 | sorting and the blend switch read two inputs | `isTransparent()` says "enters the transparent queue", but blending is still opened by `opacity < 1`: a Blend material with opacity = 1 is queued as transparent yet **does not blend**, and the alpha written there is unused | draw a Blend leaf by material mode: the picture differs from the value that should have blended, and `isTransparent()` is true | classify once (`DrawItem::isTransparent`), carry that bool in the plan (`CollectedCommand::transparent`), and have `FrameCompiler` read only it |
| P86 | an instance's resolved cache misses the parent revision | after editing the asset the instance still draws the old value, with no log; `revision()` counting only itself is this bug | change one base colour on the parent and the instance's pixels do not change; `revision()` is the same before and after | the cache key is (parent revision, own revision), and `revision()` is itself a side effect of "resolved once"; P3's test pins it |

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | ordering these traps by time or by review round | readers look up by "what am I changing", not by "which day"; the timeline belongs to git. |
| D2 | keeping only a "be careful with X" note | a note without a symptom is unwritten: the reader cannot tell whether they have hit it. |
| D3 | adding severity / confidence labels | none of the 82 entries may be ignored; labels would only be read as a disclaimer. |
| D4 | keeping a "fixed" entry for a fixed trap | the criterion for the trap being gone is that section of the test; the criterion stays, the entry goes. |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | every symptom lands on a gate or a case | `scripts/vsg_rewrite_gate.sh`, `test_vsg`, arithmetic cases |
| T2 | this page obeys the ten-chapter template as well | `python3 scripts/check_design_docs.py` |
| T3 | adding a trap keeps §3.1's ranges in step | checked by hand (the ranges are unique) |

## 9. Status and Evolution

### 9.1 Current status

Current. P1–P82 come from the measurements of the vsg rewrite and the embedding period; all are fixed and kept as prevention. The nine groups match §3.1's F1–F9.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`.

### 9.3 Compatibility / migration

Split out of `vsg-backend.md`'s chapter 6 with the numbers and the content unchanged; `vsg-backend.md` §6 is the index pointing here.

## 10. References

- `vsg-backend.md` — architecture, class diagram, contract, keys and state, rejected options, gates.
- `../bugs/` — postmortems of vsg's four fixed bugs (a black embedded window, an initialisation crash, a resize distortion, a black border when maximised).
- `docs/guides/graphics-backends.md` — the user-facing backend guide (contract, difference table, support matrix, build and evidence).
- `scripts/vsg_rewrite_gate.sh` — the evidence gate (VUID / SYNC-HAZARD / phases / picture).
