# ADR-2026-11-14: `gfx_backend_vk` becomes the production backend and eventually replaces `vsg`

- Status: Accepted
- Date: 2026-11-14
- Related: `../backlog/graphics.md` T35, `../research/graphics-2026-11-13-gpu-resource-abstraction.md` §8-§9,
  `../research/graphics-2026-11-13-vk-exposure-boundary.md`,
  `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/WindowSystem.hpp`,
  `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/Device.hpp`, `src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`

## Background

`gfx_backend_vk`'s position today is written in three places: backlog T35 ("learning / research, **not the default backend**"), its plugin `CMakeLists.txt`
("It is NOT the production backend - nothing selects it implicitly") and research §8. Functionally it has already worked through
M0-M13: offscreen and windowed, the content ABI, shadows, the deferred chain, MSAA (with depth resolve), the exposure-boundary gate, the warm-up transfer batch, `test_vk`
62 cases, and 12 byte-for-byte parity cases against `vsg`. What it lacks is no longer "can it draw" but **the things a production backend has to answer**:

- `RenderDevice::releaseRenderTarget` is not overridden (`vsg` has it): a target the host drops keeps its attachments resident until `shutdown()`;
- `RenderDevice::readDepthBuffer` is not overridden (`vsg` has it): the same read gets two answers from the two backends;
- device loss (`VK_ERROR_DEVICE_LOST`) is folded into "submission failed", with no locatable diagnostic;
- in the platform matrix only Linux/X11 is measured (the Win32 branch has never been built, macOS is unsupported, Wayland is untested);
- there is no frame-time / load-time measurement; there is no pipeline cache;
- `docs/guides/` has no vk page.

At the same time the owner has changed the goal: **vk eventually replaces `vsg` as the default backend**. This ADR rules only on "how to get there", not on "when to switch".

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the learning-backend position | Nothing moves | The opposite of the goal; the gaps are never fixed (the reverse of `G8`: a capability with a real consumer left undone) |
| B Switch the default to `vk` right now | The goal in one step | The target-release and depth-read-back gaps would show up on the first real host; the only platform is X11; there is no performance evidence (`G10` / `G11`) |
| C **Produce first, switch the default once the bar is met** | Gaps close one by one, each with a test; `vsg` stays the default until the bar is met, so a fallback path exists by construction | Two backends have to be maintained for a while; the switch gate must be a checkable list, or things stop at "good enough" |
| D Keep both backends long-term and call neither default | No switch risk | The host has to choose; both backends pay the production cost (`G5` / `G8`) |

## Decision

Chose **C**: `gfx_backend_vk`'s goal becomes **the production backend**, pushed forward by the rules below, with the default switched once the bar is met.

1. **Platform order**: Linux/X11 is the first supported target (already measured today); **Win32 second**; other platforms (macOS included) answer
   `UnsupportedSurface` / `NoPresentation` honestly, with no silent fallback. Whether Wayland is supported is a separate item and does not block X11 shipping.
2. **The platform seam**: the window system is layered away from Vulkan - one `WindowSystem` per device session (connection, surface, self-built window for
   listeners), with the platform implementation split by file (today X11: an xcb connection plus an Xlib-created window, both `dlopen`ed, so the plugin has no build-time X dependency).
   Adding Win32 = one new implementation file plus the selection point, **without touching `Device` / `WindowTarget`**. That is the abstraction this ADR and this commit commit to.
3. **Unimplemented capabilities**: answering `Unsupported` / `NoPresentation` / `NotReady` is allowed, but it must
   - be a once-only, named diagnostic (`DiagnosticCategory::UnsupportedRequest`),
   - be listed side by side with the `vsg` differences in the user documentation,
   - and **must not** fall back to "quietly pick another count / another path and carry on drawing".
4. **The switch gate** (all green before the registry's default goes from `"vsg"` to `"vk"`):
   - `releaseRenderTarget` really releases the attachments, and `releasePass` is explicitly a no-op with its reason written down;
   - `readDepthBuffer` is implemented, or it goes into the difference table and out of the claim "same capabilities as vsg";
   - device loss: classify the submission / present `VkResult`, report `VK_ERROR_DEVICE_LOST` once and let the host take the rebuild path of
     `RenderEngine::shutdown()` + `initialize()`;
   - quality gate: `test_vk` under ASan/TSan (T34's guard is a prerequisite), validation always on and silent in the tests, the whole ctest green;
   - platform evidence: recorded X11 measurements (this machine); Win32 needs a machine, and Windows does not ship until it passes;
   - performance evidence: a frame-time and load-time baseline from a real host (including `prepareFrame`'s warm-up batch);
   - documentation: the vk user page and the difference table under `docs/guides/` (not written yet); the vk normative page in `.ai/design/` promoted out of research.
5. **What happens to `vsg` after the switch**: it stays for at least one release cycle as the fallback path; whether to delete it is another ADR, not decided here.

## Consequences

- Good: the goal turns from "learning" into an acceptable checklist; the platform abstraction lands today (`WindowSystem`), and Win32 is an addition rather than a change;
  an unimplemented capability has one answer shape (`Unsupported` plus one diagnostic), so "one request, two results across the two backends" cannot happen (`G6`).
- Bad / cost: two backends must be maintained until the bar is met; the platform and performance items of the switch gate depend on real machines and host scenarios,
  so **the code side cannot declare shipping on its own**.
- Relation to existing constraints: this ADR changes neither C7/C8 (the SDK ABI shape and the packaging surface) nor D1 (the `backend/` half does not grow because of vk);
  the escape hatch in `support/Handles.hpp` remains the only Vulkan exposure point (the V2/V4/E1 gates keep running).
