# ADR-2026-11-14: the warm-up may submit one transfer-only batch of work

- Status: Accepted
- Date: 2026-11-14
- Related: `../design/graphics-frame-pipeline.md` (§3.7 F7, §5.1 `C3`, §6.11 `P11`, §7 `D11`),
  `../decisions/ADR-2026-11-12-adopt-graphics-next.md`, `RenderFrameExecutor::prepareFrame`, `PrepareReport`,
  `tests/test_vk/ResidencyTest.cpp`, `tests/test_vk/PrepareUploadTest.cpp`

## Background

`prepare()` exists for exactly one testable claim (`R23`): after a warm-up, the same declared frame's `builds` delta is 0.
But today's contract cuts away the half it could do - **the half that needs a command buffer**:

- The header says "It must not submit, present, or resize the swapchain"; the design side says `C3` / `P11`: a warm-up does not run a frame and does not
  acquire a swap-chain image.
- vk writes the consequence of that rule straight into the code (`ContentDraw::prepare`): "NO FALLBACK IMAGE, NO DESCRIPTOR SET,
  NO COMMAND: a warm-up is programs, streams and pipelines only - the 1x1 stand-in and the texture copies need a
  command buffer, which prepare() does not open."
- So a texture upload lands in **the first frame that samples it**: `report.uploads` in `Device::prepareFrame` takes only `stream_builds`,
  while that frame's `FrameUploads.texture_uploads` is 1; the copy is recorded by `executeFrame` into the frame's own command buffer
  in `ContentDraw::preparePass` (the first statement of the pass loop in `Device.cpp`).
- vsg has the same shape: `MaterialImages::acquire` → `makeImage` only stages the bytes into a data-backed `vsg::Image`, and
  the real copy is submitted by the frame's transfer task (`../design/graphics-content-lifecycle.md` 2.8 / `F16`-`F22`).

The consequence is not "one number missing" but that `R23` does not hold for the **most expensive class**: programs, streams and pipelines can be warmed up, bytes cannot;
a host budgeting its load phase from `PrepareReport` has texture bytes entirely off the books (`G4`: the report's contract is "what this call built",
yet it silently excludes a whole class, while `FrameUploads` lists streams and textures side by side). This is not the thing ADR-2026-11-12 `D11` rejected:
`D11` rejected **running a frame without submitting** (acquire an image, draw into the swap-chain image, do not present ⇒ starvation), whereas the candidate here has
no frame from beginning to end - no acquire, no render pass, no present.

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the status quo | The contract does not move, no risk | `R23` holds only for programs / streams; texture bytes are always a first-frame cost and never enter any report (`G4` / `G7`: the complexity is left for the host to guess) |
| B **A warm-up may submit one command buffer of its own containing transfers only, and waits for it before returning** | Uploads land during loading and the frame's upload delta really is 0; one rule covers all content; no new entry point (`G1` / `G8`) | A warm-up may block on its own transfers; the contract has to state "does not submit a frame" and "may submit transfers" precisely, or one word swallows the other (`G4`) |
| C Add a second verb (`flushUploads()` / `uploadNow()`) | Explicit, single semantics | Two entry points answering the same question ("build what this frame needs"), and the caller still has to know when to call it (`G1` / `G7`); no consumer today asks for the two to be separate (`G8`) |
| D Upload from a background thread / an asynchronous queue | Does not block the host | Against `C3` (only one engine thread touches the device); when `prepare` returns the upload has not landed, so `R23`'s criterion is unmeasurable (`G11`) |
| E Upload when a texture's filling completes | No new verb needed | The engine thread does not know which textures enter this frame ⇒ it uploads content nobody reads (`P8`), and touches the device on the host's texture-filling thread (`C3`) |

## Decision

Chose **B**, with the boundary written as six decidable rules:

1. **A warm-up still submits no frame**: no swap-chain image is acquired, no render pass is opened, nothing is presented, and `FrameProgress`'s
   `submitted` / `presented` do not move. Every argument of `P11` and `D11` stays valid.
2. **A warm-up may submit exactly one command buffer of its own, whose contents are transfers only**: buffer→image / buffer→buffer copies plus the
   companion layout transitions; no draw, dispatch or render pass instance.
3. **The batch must be "the copies the frame would have recorded before its first render pass", not a second path**: in vk that is
   `ContentDraw::preparePass` (the 1×1 white stand-in plus every material texture) run once on the warm-up's own command buffer;
   when that frame is then recorded, the same place finds the entries already resident and records nothing.
4. **Nothing to copy means nothing submitted**: warming up the same frame a second time produces no submission at all ("zero work ⇒ zero submission" is observable).
5. **One accounting rule**: `PrepareReport::uploads` counts **all** uploads this call built (streams + textures), with the same names and meanings as
   `FrameUploads`' two kinds; the wording of `PrepareReport::Outcome::Prepared` changes from "without submitting it" to
   "without submitting the frame".
6. **A failed transfer is not a new terminal state**: it is handled like `prepare`'s existing "this pass cannot serve it" - report a diagnostic, do not increment the count,
   and let the next frame retry; `Outcome` is unchanged.

## Consequences

- Good:
  - `R23` holds for all content, no longer "except textures"; a load-phase budget can see the bytes.
  - One rule: "a warm-up builds what this frame would have built anyway", no longer split into two classes by "does it need a command buffer".
  - The distinction stays observable and unmistakable: `FrameProgress::submitted` / `presented` are the frame's watermarks and the transfer batch does not move them -
    "the warm-up submitted transfers" and "the warm-up ran a frame" are two different answers in the report.
- Bad / cost:
  - `prepare()` may now block on the GPU, for as long as its own batch of bytes takes to copy (not waiting for some frame); the host has to count that into its
    load-phase budget. This is the same kind of synchronisation as the device's existing "submit at the frame boundary", and introduces no new consistency story.
  - The contract's wording must stay precise: any later proposal to "let a warm-up do more" has to answer "is it still a transfer and not a frame" first,
    or `P11` is bypassed quietly.
  - The two backends may progress at different speeds: `vk` lands it here; vsg's copies live in its own transfer task (not in the frame's command buffer),
    so it needs a separate change, see `T39` in `../backlog/graphics.md`.
- Trigger conditions (when to reopen):
  - a real host appears where "uploading during the load phase is itself a perceptible stall" (that needs an asynchronous entry point, which is `T17`'s territory and needs a new ADR);
  - or a second consumer requires the warm-up to be strictly non-blocking (again a new ADR, because `C3` changes together with the new threading story).

## Evidence

- The status quo (before, verified by reading the code): `Device::prepareFrame` creates no command buffer and `report.uploads` takes only `stream_builds`;
  the comment on `ContentDraw::prepare` says outright "the texture copies need a command buffer, which `prepare()` does not open"; `ContentDraw::preparePass`
  is the only place that records those copies, and `executeFrame` calls it at the head of every pass with the frame's own command buffer.
- After (`test_vk`): `VkPrepareUploadTest.APreparedTexturedFrameUploadsNothingWhenItRuns` - the warm-up report says
  2 stream uploads + 1 texture upload, and the following `executeFrame` reports `stream_uploads == 0`, `texture_uploads == 0`,
  with the pixels being that texture's colour (proving the upload landed during the warm-up).
  `VkPrepareUploadTest.ASecondPrepareSubmitsNothing` - a second warm-up has `uploads == 0` and `FrameProgress`'s
  `submitted` / `presented` are still 0.
- The guard (true before and after): `VkResidencyTest.PrepareBuildsWhatTheFrameNeedsAndSubmitsNothing` asserts that after a warm-up
  `progress->submitted == 0` and `frameReport().uploads` is still unset - this case now also pins "the transfer batch is not a frame".
- Design side: `../design/graphics-frame-pipeline.md` §3.7 / §5.1 `C3` / §6.11 `P11` / §7 `D13`.
