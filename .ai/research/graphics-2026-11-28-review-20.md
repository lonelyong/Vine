# Graphics review 20 (2026-11-28): the five seams review 19 named next

> **Status**: two findings (`Z23`, `Z24`), three areas read without one. `Z23` is high: a window whose swapchain fails to
> build never gets another one. **Both fixed 2026-11-28** - §2.2 and §2.3 state the rule each fix moved and the case +
> mutation behind it; `Z24`'s branch has no witness on this machine, and `T72` names the trigger that owes one.
>
> **Scope**: exactly the five places §27.3 of `graphics-2026-11-27-review-19.md` named as never read line by line - the
> swapchain rebuild, the device-choice filter against the enable list, the recorder's shadow diff, the pool's tests as
> oracles, and the appfw surface below the paint loop.
>
> **Verified 2026-11-28**: read-only round. Every claim below cites the line it was read at; `Z23` names the seam that
> would prove it (`Device::setMemoryBlockCreator`), and no mutation was run yet - the fix batch owes both the case and
> the mutation.
>
> **Fixed 2026-11-28, in the same session**: the fix batch ran both mutations (each guard restored in turn) and the case
> went red on each, so the witness covers the target's guard and the walk's - the review's own recipe turned out to be
> half of the story, which §2.2 records.
>
> **In one line**: the backend's two riskiest mechanisms (a device's feature enable list, the recorder's state shadow)
> are correct BY CONSTRUCTION and say why; the window's rebuild is correct in every path that RUNS and wrong in the one
> path that cannot.

## 1. Why these five, and what "read" means here

Review 19's last section listed what no round had opened: `WindowTarget`'s swapchain rebuild beyond the calls a caller
needed, `DeviceChoice`'s filter against the enable list, `ContentDraw::recordCallState`'s diff, the memory tests as
oracles, and appfw's surface below the paint loop. Each is a place where a wrong answer is SILENT - a swapchain that is
absent and never rebuilt, a feature enabled for a capability nobody has, a state command skipped because a shadow says
the buffer holds it, a pool assertion that only checks a shape - so a reading round is what they were owed.

The question asked of each was the same: **where does this mechanism answer a question the code does not own, and can
two answers to that one question drift?** That is the shape of every finding in reviews 17-19, and it is the only shape
this round looked for; anything else read correctly is recorded as "clean" with the property that makes it so, so the
next round starts from a fact rather than from re-reading.

## 2. R23 - the window's swapchain rebuild

### 2.1 The rebuild, in one pass

`WindowTarget::createSwapchain` (`WindowTarget.cpp:294`) is the only place a swapchain is made, and its order is: refuse
when the target has no surface/device, **wait the device idle and destroy the old chain** (`:303`), read the surface's
capabilities, choose a format (`chooseSurfaceFormat`, `:39`) and an extent (`currentExtent` when the surface states one,
else the announced size clamped to the device's range), count images (`minImageCount + 1`, clamped), pick a composite
alpha mode that exists, create the chain at FIFO with `oldSwapchain = VK_NULL_HANDLE`, read the images, then
`createSwapchainObjects` (`:115`): the acquire semaphore, one view per image, one present-wait semaphore PER IMAGE, the
shared depth image (created, allocated from the pool, bound, viewed) and the per-image layout records.

Every consumer reaches it through `acquire` (`:523`), which is also the only caller that consults `needs_recreate`
(`:537`); `abandonFrame` (`:734`) and `resize` (`:745`) are the two other places that raise that flag, and
`setWindowHandle` (`:754`) is the only place that rebuilds outside `acquire`.

### 2.2 Z23 (high) - a chain that failed to build is never rebuilt

`acquire()` asks `ready()` **before** it asks `needs_recreate` (`WindowTarget.cpp:531` then `:537`), and `ready()`
(`:494`) is false whenever there is no swapchain. `createSwapchain` destroys the old chain as its FIRST act (`:303`),
so every failure it can answer - no capabilities, no formats, a zero extent, `vkCreateSwapchainKHR` refused, no images,
or anything inside `createSwapchainObjects` including **a refused depth-image allocation** (`:197`) - leaves the target
with no chain at all. From that moment `acquire` refuses at `:531` with "the window target has no live swapchain" and
**never reaches the branch that would rebuild it**: the target is dead for the rest of the session.

Three consequences, each of which makes the finding worse rather than smaller:

- The header's own contract is the opposite: `resize` is documented as "Announces a surface size; the swapchain follows
  it on the next acquire" (`WindowTarget.hpp:229`). The next acquire refuses before consulting the flag that says so.
- The repair review 19 built `Z19`/C17 on is therefore dead exactly when it is needed: the surface's lost-picture line
  says "the next frame re-announces the size so the backend rebuilds the swapchain"
  (`SurfaceWindow.cpp:946-949`), and a re-announced size only sets the flag the guard has already skipped
  (`SurfaceLedger::applyFrameSize` also skips an unchanged size, `SurfaceLedger.cpp:113-125`). A lost PRESENT still
  repairs, because its chain is intact; a lost CHAIN never does.
- Only a NEW handle rebuilds (`setWindowHandle`, `:754`), so the user-visible failure is a render area that goes dark
  and stays dark while the application keeps running - on a live resize that touches a zero extent, on a dock reparent
  that keeps the same handle, or on any transient pool refusal, which is the memory-pressure case `P21` documents.

**The fix is the order, not a new path**: the rebuild branch has to be reachable when `ready()` is false, i.e. "no chain,
and one is owed" is an ordinary state of a target that was attached. **The witness already has a door**: refuse the
depth image's block through `Device::setMemoryBlockCreator` once, let the resize ask for a rebuild, then disarm and
require the next frame to answer `Presented` - today it answers `Skipped` for the rest of the run. The mutation that
proves the assertion load-bearing is moving the `needs_recreate` branch above the `ready()` guard.

**Fixed 2026-11-28, at the rule, and in TWO places where the review named one.**

- `WindowTarget::acquire`'s guard asks the WINDOW (`attached && !detached`), never the chain; `destroySwapchain` - the
  one function that removes a chain - raises `needs_recreate`, so "there is none" is a state the acquire repairs rather
  than a reason to refuse. `destroySwapchainWhenIdle` also skips the device idle when there is nothing left to stand
  down, because the retry would otherwise pay a stall per frame.
- **The frame walk was the other half, and the review did not name it**: `claimPasses` asked `ready()` before it ever
  called `acquire`, so the target's own guard was not the first stop - the walk never reached it. A RECORDED frame now
  asks for its IMAGE (the acquire, which repairs); a WARM-UP still asks for the CHAIN, because it acquires nothing
  (`P11`). A failed acquire is recorded once per frame (`FrameRecording::window_refused`) so a later window pass neither
  starts a second rebuild nor draws.
- **The witness, as it actually works**: `VkWindowPresentTest.ARebuildThatFailsIsRetriedByTheNextFrame` - present, resize
  the host window to 4096x4096, refuse every new block, one frame (the target is left `NoSwapchain`), disarm, and require
  the next frame to present. **The review's own recipe needed a correction**: a rebuild of the SAME extent re-uses the
  depth slice it has just released (the pool places the new request in the hole, granularity page and all), so the
  request has to grow past the pool's largest first-fill block - 64 MiB, i.e. 4096x4096 D24S8 - before a block creation
  (the only refusable step) happens.
- **Both mutations were run**: restoring `acquire`'s `ready()` guard turns the case red at "the next frame presents", and
  restoring the walk's guard turns it red the same way - the state stays `NoSwapchain` in both runs.

### 2.3 Z24 (low) - `VK_SUBOPTIMAL_KHR` means two things in two calls

`vkAcquireNextImageKHR` answering `VK_SUBOPTIMAL_KHR` is a SUCCESS: the image index and the semaphore signal are valid.
`acquire` treats it as a failure (`:570-573`), marks the flag, rebuilds the chain inside the same call and retries - so
a perfectly usable acquired image is dropped and the frame pays a `vkDeviceWaitIdle` plus a full rebuild. `present`
treats the same code the other way (`:715-718`): it counts the frame as `Presented` and rebuilds on the NEXT frame,
which is the documented meaning of suboptimal ("a picture arrived; the chain no longer matches the surface").

Nothing is corrupted either way (the dropped image is given back by the rebuild, exactly as `abandonFrame` does), so
this is a cost and a consistency finding rather than a correctness one - but it is also a SECOND route into `Z23`: a
suboptimal acquire can start a rebuild, and a rebuild that fails leaves the target in Z23's permanent state.

**Fixed 2026-11-28, as one branch.** `acquire` takes `VK_SUCCESS` and `VK_SUBOPTIMAL_KHR` through the same path: the
index and the signal are used, the frame draws and presents from the chain it has, and `needs_recreate` is raised so the
NEXT acquire builds the replacement - exactly the meaning `present` gives the same code. The out-of-date answer keeps its
rebuild-and-retry-once branch, which is the answer a surface that no longer matches actually needs.

**NO WITNESS ON THIS MACHINE, and that is recorded rather than papered over**: X11's lavapipe answers
`VK_ERROR_OUT_OF_DATE_KHR` when the extent changed, and no door injects an acquire result the way
`Device::setMemoryBlockCreator` injects an allocation failure. The branch is stated by inspection plus the consistency
argument with `present`; `T72` in `../../backlog/graphics.md` is the trigger-gated case that owes it a driver.

### 2.4 What the rebuild already gets right

- The per-image present-wait semaphore (`render_finished`, `:92-99`) with its measured reason - a single semaphore
  re-signalled before the presentation engine consumed it is `VUID-vkQueueSubmit-pSignalSemaphores-00067`, found in the
  app, not in a test.
- The layout record is per image (`image_layouts`, `:81`), and `recordPresentTransition` writes it through the same
  `transitionImage` call that records the barrier, so the record cannot disagree with the command.
- The window's colour attachment forces `StoreOp::Store` whatever the pass stated (`beginPass`), because the image is
  about to be handed to the compositor - a `DontCare` there would discard the picture itself.
- `attach` re-checks `vkGetPhysicalDeviceSurfaceSupportKHR` per surface (`:475`), so a second window on a surface the
  session's queue family cannot present to is refused with a sentence instead of presenting nothing.

## 3. R24 - device selection: does the filter equal the enable list?

### 3.1 In one pass

`DeviceOffer` + `candidateRefusal` (`DeviceChoice.hpp:31-77`) are a VALUE and a pure predicate, so the floor is
testable without a driver. `Device::initialize` fills one per candidate from three queries
(`vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceFeatures2` chained through the 1.3 and EDS3 feature structs, and
the device extension list), keeps the refusals of the highest-ranked device that failed, and picks the best SURVIVOR
(`Device.cpp:3061-3126`). Creation then enables exactly what the filter proved: the same five 1.3/EDS3 features, the
EDS3 extension, `VK_KHR_swapchain` only when the device offers it and a window is wanted, and the five 1.0 features the
SDK can express copied from what the device reports (`:3165-3172`).

### 3.2 No finding - and the one sentence that overstates

The filter and the enable list are one statement for every field the creation path reads: nothing can pass the filter
and then be created without the feature, and nothing is enabled that was not asked of the device first
(`wanted_features.x = available_features.x`, one by one). The capabilities are then built from the SAME `wanted_features`
values, so "advertised" and "enabled" cannot drift (`Device.cpp:3327-3346`) - the property the section was named for holds.

One comment overstates what the SDK can express: `:3158-3163` names "MRT with per-attachment blends (`independentBlend`)"
as a switch a host may throw, but the seam carries ONE blend state per draw (`DrawState::blend`,
`DrawState.hpp:290`), and the recorder states one equation for every attachment (`ContentDraw.cpp:305-330`). Enabling
`independentBlend` is harmless; claiming it serves a capability that does not exist is the kind of sentence that later
justifies code. It is recorded here, not as a finding: correcting it is a comment.

## 4. R25 - the recorder's shadow diff

### 4.1 In one pass

`ContentDraw::RecordedState` (`ContentDraw.hpp:246-272`) is "what the last commands left bound", and
`recordCallState` (`ContentDraw.cpp:203`) states each item only when it differs: viewport, scissor, line width, depth
bias, then the raster half (polygon mode, cull, front face, depth test/write/compare) and the blend equation, which is
`vkCmdSetColorBlendEnableEXT` + `vkCmdSetColorBlendEquationEXT` in chunks of eight attachments. `bindPipeline` keeps the
shadow across a bind because every pipeline comes from one builder declaring the same twelve dynamic states.

### 4.2 No finding; the two rules an edit must obey

The shadow is reset exactly once per command buffer: `ContentDraw::beginFrame` (`ContentDraw.cpp:176-180`) assigns a
fresh `RecordedState`, and one frame records into one command buffer (`kFramesInFlight = 1`). Both maintenance rules it
depends on are written where an editor would break them: a SECOND pipeline builder with a narrower dynamic list must
invalidate the shadow where it binds (`ContentDraw.hpp:240-241`), and nothing else invalidates it because dynamic state
is command-buffer state, not render-instance state (`:242-243`). The blend early-exit compares the attachment COUNT as
well as the values (`ContentDraw.cpp:300-304`), which is what makes a depth-only pass between two colour passes safe.

## 5. R26 - the pool's tests as oracles

No finding. The cases assert NUMBERS and not shapes: `FreeNeighboursMergeSoThePoolIsReused` (`MemoryTest.cpp:411-448`)
requires four quarters to fit one block, the merge to be reused at offset 0, still one block, and `used_bytes == 0` at
the end - so a pool that allocated a second block, or forgot to clear a released slice, is red; the two refusal cases
assert the CLASS and the room left (`:491`, `:544`), the exhausted-heap case asserts one report with its numbers
(`:621`), and the lost-device case asserts the session ending (`:730`). What remains read-only is `placeAt`'s
granularity rule beyond its one case (`LinearAndOptimalDoNotShareAGranularityPage`, `:449`): the rule is tested on one
pair of neighbours, not as a table.

## 6. R27 - the appfw surface below the paint loop

No finding. `RenderControl` is a forwarding face (`RenderControl.cpp:120-227`), and the two facts it owns are stated
where they are decided: the window container is hidden until a frame is in the surface, and showing it is the only way
to give a platform that wants an on-screen surface one - so `on_state_changed` maps `Presenting` onto visibility and
`on_needs_visible_surface` shows the area hole and all (`:64-116`). Both callbacks are cleared in the destructor before
the base releases the widget (`:118-128`), which is the callback-lifetime rule: the surface may report going away
during teardown, and a lambda that names this control must already be gone.

## 7. Registry and proposed order

### 7.1 Every finding

| # | Round | Severity | One line | Evidence |
|---|---|---|---|---|
| `Z23` | R23 | high | a swapchain that failed to build is never rebuilt: `acquire` asks `ready()` (no chain) before `needs_recreate` (rebuild it), and `createSwapchain` destroys the old chain first, so one failed rebuild - a zero extent, a refused `vkCreateSwapchainKHR`, or a refused depth-image allocation - ends the window for the session - **fixed 2026-11-28** (`VkWindowPresentTest.ARebuildThatFailsIsRetriedByTheNextFrame`; both guards mutated back, both red) | `WindowTarget.cpp:531`, `:537`, `:303`, `:197`, `:494`; `WindowTarget.hpp:229`; `SurfaceWindow.cpp:950-956`; `SurfaceLedger.cpp:113-125` |
| `Z24` | R23 | low | `VK_SUBOPTIMAL_KHR` from `vkAcquireNextImageKHR` is a success, but `acquire` treats it as a failure, drops the acquired image and pays a device idle + full rebuild inside the same call, while `present` treats the same code as "presented, rebuild next time" - and a rebuild started there is a second route into `Z23` - **fixed 2026-11-28, UNWITNESSED here** (no driver on this machine answers `SUBOPTIMAL` from an acquire; `T72` holds the trigger) | `WindowTarget.cpp:570-573`, `:715-718` |

### 7.2 Proposed order (for the owner; no backlog entry added by this round)

1. **`Z23`** - one guard order, plus the witness the round names. The case: attach a Probe window, refuse one block
   through `Device::setMemoryBlockCreator`, resize (so the rebuild is asked for), disarm, and require the next frame to
   be `Presented`. The mutation: move the `needs_recreate` branch above the `ready()` guard and the case goes green.
   What the fix must NOT do is rebuild on a target that was never attached or that is detached - "no chain" is a defect
   only when a chain is OWED, so the guard splits in two rather than moving.
2. **`Z24`** - one branch: `VK_SUBOPTIMAL_KHR` from the acquire is a success with a rebuild owed, exactly like
   `present`'s answer. It lands with `Z23` because the two touch the same four lines, and the same case then also pins
   that a suboptimal acquire does not thrash.

**Both landed 2026-11-28.** Order 1 brought a second call site the round had not named (the frame walk), and order 2
rides on the same lines; the case that pins order 1 is `VkWindowPresentTest.ARebuildThatFailsIsRetriedByTheNextFrame`,
with one mutation per guard. The "does not thrash" half of order 2 is the piece that still has no case (`T72`).

### 7.3 What this round did not cover

- The rebuild under a DEVICE LOSS in the middle of it (`vkCreateSwapchainKHR` / `vkAllocateMemory` answering
  `VK_ERROR_DEVICE_LOST`): the pool's sink ends the session, but the target's own state after that is unread.
- The surface seam itself: `WindowSystem`'s per-platform files were read only where `Z23` needed the surface's answer,
  and the Wayland question is recorded in `vk-backend.md` 1.3 rather than here.
- The recorder under a second pipeline builder: the rule is written, the code cannot yet violate it, and no case would
  catch a violation - a future builder has to bring its own.
- The pool's first-fit SEARCH under a fragmented heap: `largest_free_run` reports the result, nothing measures the walk.

## 8. Method and verification

- Read whole: `WindowTarget.cpp` (777 lines), `DeviceChoice.hpp` (78), `ContentDraw.cpp` (690) with `ContentDraw.hpp`
  (the `RecordedState` block), `RenderControl.cpp` (227), plus the parts of `Device.cpp` the three touch
  (`:3040-3126` candidate scan, `:3160-3230` enable list, `:3329-3346` capabilities) and `SurfaceLedger.cpp`
  (the size announcement).
- Read as oracles: `MemoryTest.cpp`'s twelve cases, with `FreeNeighboursMergeSoThePoolIsReused` read in full.
- Not run: no mutation, no test, no driver session for this round - `Z23`'s witness is named, not written. The next
  batch owes both the case and the mutation before either finding is called closed.
