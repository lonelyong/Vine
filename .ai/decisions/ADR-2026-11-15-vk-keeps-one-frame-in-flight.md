# ADR-2026-11-15: the vk backend keeps one frame in flight, and a multi-frame pipeline waits until "the host becomes the bottleneck"

- Status: Accepted
- Date: 2026-11-15
- Related: `../design/vk-backend.md` (§2.3 one submission per frame, `C1`, `P6`, the known gap in `§9.2`),
  `../research/graphics-2026-11-14-vk-industry-review.md` (F5), `RenderFrameExecutor::executeFrame`,
  `RenderDevice::readColorBuffer`, `src/plugins/gfx_backend_vk/src/internal/Device.cpp`

## Background

F5 of the industry-standard review points out that the vk backend calls `vkWaitForFences(..., UINT64_MAX)` immediately after each frame's `vkQueueSubmit`
(`executeFrame` in `Device.cpp`, plus two read-back paths), that one `completion` fence serves every submission, so **CPU and GPU are
fully serialised**; the industry practice is 2-3 frame slots, each with its own fence / command buffer / constant-ring slice, waiting only when a read-back needs that frame.

This is not an oversight: `readColorBuffer` / `readDepthBuffer` promise **read right after the draw** (the origin of `C1`), and "this frame is done" can only be
proven by a fence wait. Moving to several frames in flight changes the **framework semantics of "who guarantees this frame is done"**, which is why it is a decision
rather than an optimisation.

The first thing such a decision needs is numbers: **of this frame's time, how much is waiting for the device and how much is the host's work.**

## Evidence (lavapipe, the real host `Vine --open demo`, 60-frame benchmark)

| Quantity | Value | How it was measured |
|---|---|---|
| Median wall clock of one `renderFrame()` | **8.63-8.94 ms** (three runs: 8.94 / 8.86 / 8.76) | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=60`, the demo's benchmark log |
| **Device** time of the same frame | **7.95 ms** | `FrameReport::timing` (a pair of `VK_QUERY_TYPE_TIMESTAMP` queries, sampled frame) |
| The difference = **the host-side work** | **≈ 0.7-0.9 ms (≈10%)** | wall clock − device time; `vkWaitForFences` is inside `executeFrame`, so the wall clock necessarily contains the device execution |

**The conclusion is the opposite of what was recorded earlier and must be corrected**: §2.3 of `../design/vk-backend.md` once said "CPU median 8.2-9.0 / GPU 7.15-7.77,
i.e. the CPU is the bottleneck" - that 8.2-9.0 ms **is the wall clock including the fence wait**, not the host's work. The host side is only about 0.9 ms, so
**the device is the bottleneck** (about 90%).

That correction decides this ADR: the upper bound of the multi-frame gain is

$$
T_{\text{throughput}} = \max(T_{\text{host}},\, T_{\text{device}}) = 7.95\ \text{ms}
\quad\text{not}\quad T_{\text{host}} + T_{\text{device}} = 8.9\ \text{ms}
$$

that is **about 10%** on this machine, at the cost of changing the SDK contract "read right after the draw". And if the host really does become the bottleneck (common on a real GPU:
device 1-2 ms, host 4-6 ms), the same change would be worth 2-3x - that is when it earns its keep.

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A **Keep one frame in flight and write the cost into the normative page** | The contract does not move; "read right after the draw" and single-frame semantics are the same rule (`G3` / `G4`); one fence, one command buffer, one place where failures are classified (`G11`) | Host and device are serialised, and throughput is capped at the sum of the two (≈10% loss here) |
| B 2-3 frame slots, each with its own fence / command buffer / ring slice, waiting for that frame only on read-back | Throughput = max(host, device); the biggest gain on a real GPU | **Changes the framework semantics**: `readColorBuffer`'s "read right after the draw" no longer holds unconditionally - either a new "wait for this frame" verb, or a read-back returning `NotReady` (splitting `G4`'s contract in two); the constant ring, descriptor-set cache, `retirePoint`, content retirement and the vsg parity comparison all have to be recomputed per slot (`G5`: the change spreads across five modules); `G10`: video memory and ring residency multiply by the slot count |
| C Make only **read-back** asynchronous (`readColorBuffer` may return `NotReady`; fetch it next frame) | Keeps single-frame submission and keeps the host waiting | Read-back is a test / tool path, not a frame-path cost; changing it does nothing to the 8.9 ms (`G8`: no consumer pays for it) |
| D Shave the 0.9 ms of host time | Directly reduces throughput (under a serial model, host time is pure loss) | Ceiling 10%; and the 0.9 ms would first have to be broken down before knowing what to shave (there is no per-stage host timing today) |
| E Wait each frame only for "the previous frame" and not for this one (two slots, no read-back semantic change) | Throughput close to max(host, device) | "Read right after the draw" becomes "read one frame after the draw" - **still a contract change**, only with B's cost hidden (`G4`: changing the meaning without changing the name is worse than B) |

## Decision

**Adopt A**: the vk backend keeps **one frame in flight**, and §2.3 and §9.2 of `../design/vk-backend.md` record it as a **known cost** (with the numbers in the table above),
not as a defect waiting to be fixed. Both B and E change the semantics of `RenderFrameExecutor` / `RenderDevice`, so they must wait for a trigger condition and go through their own ADR
(which may then cite the arithmetic here instead of re-arguing it).

## Consequences

- **Good**: the contract is simple (one frame, one submission, one wait, one failure path); read-back, content release, ring growth and pipeline-cache persistence all rest on
  "returned means idle", which `C1` alone covers; the parity tests do not need to work out "which frame has completed".
- **Bad / cost**: host and device are serialised. Measured loss on this machine (lavapipe) is ≈10% (8.9 vs 7.95 ms); on a real GPU where the device is far faster than the host,
  the loss becomes several times that - **and that is exactly the trigger condition**.
- **Trigger conditions (the bar for reopening this ADR, written as observable facts)**:
  1. on any real host, the measured `FrameReport::timing` **device time is less than half the wall-clock time** (the host is the bottleneck) and the frame time
     falls outside the 60 fps budget;
  2. or an output form appears that can only meet its target by overlapping with the GPU (offline baking, video export, roaming a large scene);
  3. or a second backend needs the same "which frame do we wait for" semantics (then it is a framework question, not a vk one).
- **What is not done**: no multi-slot pipeline "to look more industrial" - `G8`: an extension point without a consumer immediately multiplies the bill of five modules by the slot count.

## Evidence

- Numbers: the "Evidence" table above (three runs plus one sampled frame's device time).
- Code: `executeFrame` in `src/plugins/gfx_backend_vk/src/internal/Device.cpp` (`vkWaitForFences` inside it),
  `readColorBuffer` / `readDepthBuffer` (the other two waits), and §2.3 / `C1` / `P6` of `../design/vk-backend.md`.
- Tests: `tests/test_vk/*` (the whole suite implicitly relies on the one-frame semantics: `readColorBuffer` right after every `executeFrame`);
  `tests/test_vsg/BackendParityTest.cpp` (both backends compared byte for byte under the same frame semantics).
