# ADR-2026-11-27: an upload is prepared, then recorded

- Status: accepted
- Date: 2026-11-27
- Related: `../design/vk-backend.md` 2.23 / C16 / P21 / T15 / D13 / 9.3,
  `../research/graphics-2026-11-27-review-19.md` `Z15`,
  `src/plugins/gfx_backend_vk/{include/vine/backend_vk/internal/ContentImages.hpp,include/vine/backend_vk/internal/ContentCaches.hpp,src/internal/ContentImages.cpp,src/internal/ContentCaches.cpp,src/internal/Device.cpp}`,
  `tests/test_vk/ResidencyTest.cpp`

## Background

Review 19 `Z15` reported that a texture upload failing halfway destroys the image the caller's command buffer already
names, and that the frame then submits that command buffer. The tree matched the report:

- `ContentImages::viewFor` called `Impl::recordUpload`, which recorded the barrier into `TRANSFER_DST` **first** and then
  created one staging buffer per layer *inside* the copy loop. A refused allocation on layer *k* returned `false` with
  the barrier and layers `0..k-1`'s copies already recorded.
- The failure path then ran `impl->destroyEntry(*entry)` - an immediate `vkDestroyImage` plus a pool release - and the
  caller's frame went on to submit the command buffer that named that image (`VUID-vkDestroyImage-image-01000` names the
  rule; the validation layer does NOT report this shape - measured, see Evidence - and a driver that does not check reads
  freed memory).
- `ensureFallback` had the same shape one step later: the barrier, the copy and the second barrier were recorded, and
  then `vkCreateImageView` ran. A refused view destroyed the image those three commands name.

Every other release in this cache goes through `releaseLater` (the park queue) or the batch's books, so these two paths
were the only ones that destroyed an object out of hand. The review offered two remedies: build every layer's staging
before recording anything, or park the failed entry against the batch.

The concept behind the rule: **a command the driver already holds cannot be taken back.** Recording is a lifetime
statement about every object it names, so a function that both allocates and records has an obligation it can only fail:
undo recorded commands. Remove the failure from the recording phase and the obligation disappears.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Split the upload into a PREPARE half (fallible, records nothing) and a RECORD half (`void`, records the two transitions and one copy), and hold every layer and mip in ONE staging buffer | The failure that used to arrive mid-record no longer exists; the record half's signature states that it cannot fail (`G4`); one allocation, one mapping and one copy command per upload instead of one per layer (`G10`); the 1x1 stand-in takes the same record verb, so there is one copy path | Three internal signatures gain a parameter, and the prepare half owns a small value type (`StagedUpload`) whose buffer the record half hands to the batch |
| B. The review's second branch: keep the interleaving and park the failed entry through `releaseLater` instead of destroying it | Smallest diff at the failure site | Keeps the interleaving, so `recordUpload` still reads as "may fail halfway" and the next editor can add a fallible call after the first recorded command (`G1`: the rule is not stated anywhere). The batch is left holding staging for a copy whose destination nobody publishes, so the books answer "what this batch recorded" with a ghost |
| C. Record an upload into a per-upload secondary command buffer and discard it when the prepare fails | Nothing a failed upload recorded reaches the frame's buffer | A command buffer per upload, and `vkCmdExecuteCommands` cannot be un-recorded either: the discard only helps if the failure is known before `vkEndCommandBuffer`, i.e. before recording - which is option A's ordering with a second object to manage (`G8`) |
| D. Allocate every layer's staging before the loop but keep one buffer per layer | No new value type; the review's first branch, literally | N allocations, N mappings and N copy commands for one upload, and `Batch::staging` grows per face; the prepare half then has N things to give back instead of one (`G10`) |
| E. Probe first (walk the layers to validate, then allocate, then record) | No signature change | Doubles the walk and still fails if the allocation between the two walks fails - the same defect, one call later |

## Decision

Option A.

**Two halves, stated by their signatures.** `Impl::prepareUpload` builds the image's bytes: it walks the layers, sums
their sizes with every base rounded to the copy command's four-byte rule, creates ONE host-visible buffer, maps it,
copies each layer in, and builds the `VkBufferImageCopy` array. It returns a bool and a reason, and it records nothing.
`Impl::recordUpload` states `UNDEFINED -> TRANSFER_DST`, one `vkCmdCopyBufferToImage` carrying every region,
`TRANSFER_DST -> SHADER_READ_ONLY`, and hands the staging buffer to the batch - it answers `void`, because there is
nothing left to refuse.

**The stand-in takes the same road.** `ensureFallback` creates its image, memory and VIEW before recording (a view does
not need content), then the 4-byte staging buffer, then calls the same `recordUpload`. Its refused `vkCreateImageView`
therefore destroys an image no command names, which is the second half of `Z15`.

**One staging buffer per upload.** The layers of a cube map are regions of one buffer rather than six buffers, so a
refusal costs one object and the copy is one command. The region bases are rounded to four bytes, which also fixes an
older latent defect: an `R8` mip chain whose levels are not a multiple of four could state an unaligned
`bufferOffset` in the old per-mip arithmetic.

**The upload's barriers are counted.** `viewFor` / `ensureFallback` / `resolveTextures` take an optional
`RecordingTally*` and the two `recordBarrier` calls report into it, so "a refused upload records nothing" is a number
`Device::recordingNumbers()` answers. Before this the upload was the one frame-path recorder whose barriers no report
counted.

## Consequences

- A refused upload leaves the caller's command buffer exactly as it found it: no barrier, no copy, no destroyed object
  in a submitted frame. The failure is a refusal the host sees (`ContentSkipped`, the staging sentence) and nothing
  else; the next frame uploads the texture normally.
- One allocation and one copy command per upload instead of one per layer: a six-face cube map costs one pool slice and
  one `vkCmdCopyBufferToImage` where it used to cost six of each. The region array is built in the prepare phase, so the
  recording phase still allocates nothing.
- Internal-only API change (`ContentImages`, `ContentCaches::resolveTextures`, both `internal/` with no ABI promise, and
  one optional parameter threaded from `Device::FrameDriver::resolveFrameHead`). No diagnostic code was added, renamed or
  reused, and the sentence a refused staging reports is unchanged.
- Residuals. The rule governs what this cache records from host bytes; a future fill (an array texture, a compute
  writeback) must be prepared first and recorded second, and a destroy after a recorded command must go through the
  batch's books or the park - the trap `P21` states both, because the compiler can only catch half of it. The stake is
  not zero-cost: the prepare phase holds the whole upload's bytes in one pool slice, so a texture whose total exceeds
  the pool's block policy gets one block sized to the request instead of several small ones (measured by
  `MemoryAllocator`'s own numbers, not by a benchmark).

## Evidence

- `VkResidencyTest.ARefusedTextureUploadRecordsNothingSoTheFrameNamesNoDestroyedImage`: a 21 MiB texture (its image fits
  the first 64 MiB device-local block, its staging exceeds the first 16 MiB host-visible one) is uploaded with every new
  pool block refused. The frame runs, spends **exactly** the barriers of a frame that uploads nothing
  (`Device::recordingNumbers()`), reports `ContentSkipped` once, and draws the 1x1 stand-in's white; with the blocks
  allowed again the next frame uploads it, reports one `texture_uploads` and draws the texture's blue.
- Mutation, verified and restored: recording the transition before the staging allocation (the `Z15` shape, with the
  tally in place) turns both counter assertions red - the refused frame spends one dependency and one barrier more than
  the baseline - and the case goes back green when the mutation is removed.
- **The validation layer does not catch this shape, measured.** With `VK_LAYER_KHRONOS_validation` 1.4.341 installed
  (user-local, 2026-11-27, lavapipe) the same mutation produced **no** layer message: the destroy happens before the
  submission, and the layer's in-use check only sees references held by submitted command buffers. That is precisely why
  the witness had to become a number rather than the layer - the first version of the case was green under the mutation
  with the layer absent, and still green with it installed.
- The whole suite: `build/bin/test_vk` - 236 passed, 1 skipped (`VkContentDrawTest.AFrameAndItsPassesAreNamedToACaptureTool`,
  skipped because this session resolves no `VK_EXT_debug_utils`), 237 cases / 47 suites on 2026-11-27.
- `VkContentDrawTest.ACubeMapIsUploadedWithEveryFaceAndSampled` and the rest of the image cases are unchanged: the six
  faces are one copy command now, and the sampled pixel still proves every face's layout and order.
