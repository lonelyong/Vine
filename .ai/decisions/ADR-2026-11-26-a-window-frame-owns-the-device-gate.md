# ADR-2026-11-26: a window frame owns the device's frame gate

- Status: Accepted
- Date: 2026-11-26
- Related: `../design/vk-backend.md` (2.3 one submission per frame, `C1`), `../design/graphics-threading.md` (2.12),
  `Device::FrameDriver::executeFrame` and `Impl::body_gate` / `queue_submit`
  (`src/plugins/gfx_backend_vk/src/internal/Device.cpp`), `ADR-2026-11-15-device-is-a-pool-sessions-own-frames`,
  `ADR-2026-11-15-vk-keeps-one-frame-in-flight`

## Background

The device serialises frame bodies with one read-write gate (`Impl::body_gate`): an OFF-SCREEN frame takes it **shared**
(two drivers record and submit at the same time - `VkConcurrentFrameTest`, `MultiSessionTest`), while a frame that
PRESENTS takes it **exclusive**. The exclusive side means a window frame and any other frame body - a second window's, or
an off-screen driver's - never overlap.

An archive review (round 12, finding A4) read that as a boundary drawn wider than the shared state: the swapchain, its
semaphores and a window's images are all PER WINDOW, and the queue already has an external-synchronisation lock
(`Impl::queue_submit`). Two windows on one device therefore render strictly one after the other, and an off-screen
driver waits behind a window frame.

## Evidence

| Fact | Where |
|---|---|
| A window frame takes the gate exclusively; an off-screen frame takes it shared | `FrameDriver::executeFrame`, the `present_to != nullptr` branch |
| `vkQueueSubmit2` and `vkDeviceWaitIdle` run under `queue_submit`; **`vkQueuePresentKHR` does not** - it is the one queue call the exclusive gate is what serialises | `Impl::queue_submit`'s note, `WindowTarget::present` |
| The window half's refusal episodes are DEVICE state (`Impl::window_reports`: presenter, acquire, attach, occupied, ...), written by frame bodies and by `createSurfaceTarget` / `initialize` | `Impl::WindowReports`, `claimPasses`, `executeFrame`, `createSurfaceTarget` |
| The surface list is read by a frame (`resolvePresenter`) and written by host verbs; both live under this one gate | `Impl::surfaces`' note |
| Host access to a `VkQueue` - including `vkQueuePresentKHR` - must be externally synchronised | Vulkan specification (threading rules) |

Two things follow. First, present cannot simply be moved under `queue_submit` to let the gate go shared: present is the
one queue call that may block on the presentation engine (FIFO, a full present queue), so holding the queue's lock across
it would serialise every other driver's submit behind a v-sync - trading one serialisation for a worse one. Second, the
window episodes would have to move into `WindowTarget` before two windows could present concurrently, because they are
device-level `ReportOnce` objects today.

## Decision

A window frame keeps the device gate exclusively. The price is stated rather than hidden: with two windows, or a window
and an off-screen driver, the frame bodies run one at a time; whatever else overlaps (the caches, the allocator, the
target table, the content halves) still does.

This is a decision, not an oversight: the mechanism that replaces it has to serialise presents anyway, and the only
consumer that would pay for it - a host that renders two windows concurrently - does not exist in this tree.

## Rejected options

| Option | Why not |
|---|---|
| Shared gate + per-window mutex, `vkQueuePresentKHR` under `queue_submit` | A blocking present (FIFO, full present queue) would hold the queue's external-synchronisation lock across the wait, serialising other drivers' SUBMITS - a worse stall than today's, on the path every frame uses. |
| Shared gate + per-window mutex, present under a separate device `present_gate` | Same stall as above (present and submit share the queue, and the spec requires host access to it to be serialised), plus a second device-wide lock whose order against `queue_submit` would need its own rule (`G6`). |
| Move the window episodes into `WindowTarget` and split present per window | The right first step IF two concurrent windows are wanted; it is a refactor of the frame's own refusals for a capability with no consumer (`G8`). Registered as the trigger below. |

## Consequences

- A second window's frames wait for the first window's whole engine half (acquire, record, submit, fence, present); so
  does an off-screen frame of another driver. The picture is correct in every case; the parallelism is what is given up.
- **Trigger to revisit**: a host that drives two windows concurrently and measures the serialisation (or a per-window
  present path asked for by a consumer). The work is one batch: move the presenter/acquire episodes into `WindowTarget`,
  give each target its own mutex, and take the device gate shared - with `vkQueuePresentKHR` reachable only while that
  target's mutex is held.
