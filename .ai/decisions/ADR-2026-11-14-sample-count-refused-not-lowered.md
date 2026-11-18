# ADR-2026-11-14: the sample count is a **count**, and a request the device cannot build is refused once rather than lowered

- Status: Accepted
- Date: 2026-11-14
- Related: `../design/graphics-rendering.md` §3.2 (F6–F10), `Capabilities::max_samples`, `RenderTarget::setSamples`,
  `../backlog/graphics.md`, `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`,
  `src/plugins/gfx_backend_vsg/src/internal/HostTargets.cpp`, `src/plugins/gfx_backend_vk/src/internal/Device.cpp`, <!-- drift-ok -->
  `tests/test_vsg/MsaaTargetTest.cpp`, `tests/test_vk/MsaaTest.cpp`

## Background

One public contract has three contradictory answers today:

- **The documentation**: `Capabilities::max_samples` is "the maximum **count** a target may ask for"; `RenderTarget::setSamples`
  says a request above it is refused (reported once) and **never** silently lowered; F10 of `graphics-rendering.md` also says "over the limit reports
  `SamplesUnsupported`".
- **`vsg`**: the behaviour matches (`HostTargets::State::SamplesUnsupported`, no object built), but `capabilities().max_samples`
  and `setMaxSamples(...)` receive the **bitmask** `framebufferColorSampleCounts & framebufferDepthSampleCounts`:
  a device that supports 1/2/4/8 answers **15**. A host that reads 15 and calls `setSamples(15)` passes the "`> max_samples`" comparison, and then
  the number is floored to 2 by `samplesToVk` while staying in `shape().samples` - **the same target's shape and object
  disagree**. And "a count the device does not offer at all" (3, say) is never checked by anybody.
- **`vk`** (M11, 2026-11-14): `max_samples` is a count (8 on this machine, correct), but an over-limit request is **lowered** to the nearest supported
  count with one report; and a samplable depth plus multisampling lowers the target to single-sample outright. Both are the opposite of the documentation above.

Two backends, three documents, two outcomes: this is the thing this ADR closes (`G1` / `G4` / `G6`).

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| A Keep the status quo (the `vsg` bitmask + the `vk` downgrade) | No code changes | The "limit" a host receives is not a count (`G4`: name and semantics disagree); the same `setSamples` gives two outcomes on two backends (`G6`); `shape().samples` can be a number the device does not have |
| B **Follow the documentation: the limit is a count, and a request the device cannot build is refused once** | Value, documentation and both backends give one answer; a host may trust that `samples()` is what is drawn (`G4`); the refusal happens before any object is built, and `SamplesUnsupported` is a shape that already exists | A "quality knob" parameter now loses the picture: a host that writes 16 while the device only goes to 8 gets nothing drawn on `vk` (and always did on `vsg`); the host must read `max_samples` (as the documentation already required) |
| C Change the contract to "lower and report once" and make `vsg` follow | A picture always appears | The reference backend's target path changes (519 cases); worse, the resolved picture differs from the shape the host declared while `RenderTarget::samples()` still answers the declared value - a lie **no number would tell** (`G4` / `G10`) |
| D Add "the set of supported counts" to `Capabilities` | The host can choose exactly | Vulkan's bit-set vocabulary enters an API-neutral value (`G13`); no consumer today (`G8`); "a limit plus counts that are powers of two" already describes it fully |
| E Round up to the nearest supported count | The picture is at least as good as requested | More samples than requested is another cost the host did not agree to (`G10`); the same sin as "silently changing the shape I declared" |

## Decision

Chose **B**, and states "a request the device cannot build" once and for all:

1. **`Capabilities::max_samples` is a count** (`1` = no multisampling), not a bitmask; **the legal counts are powers of two**
   (1/2/4/8/16/32/64), so "a limit plus powers of two" fully describes the set this device can build.
2. **A request = the declared shape**: `RenderTarget::setSamples(n)` means n is n. If the device cannot build n (over the limit, or n not a power of two),
   the target is **not built**, a named diagnostic reports it **once**, `readColorBuffer` answers `NotReady`, and it is **never** drawn
   with another count. The contract's "never silently lowered" stays as it is: `vk` moves towards it, not the other way round.
3. **Depth plus multisampling is stated as "can it resolve"**: a multisampled target's **colour** is always resolved; its **depth**, when it has to be handed to a
   shader (`setDepthPromoted`) or to another pass (`shareDepth`), must be a single-sampled image - a backend that can resolve depth resolves it (`vsg`'s
   `depthResolveMode = sample0`), and one that cannot (`vk` today) **refuses** the target with one report.
   **Borrowed depth plus multisampling** is refused by every backend (one framebuffer's attachments share one count).
4. **Once, one category**: both backends use `DiagnosticCategory::UnsupportedRequest`, the message carries the requested value and the limit
   ("asked for 16 samples per pixel, above this device's limit of 8"), and the backend itself guarantees it does not flood.
5. **No new `Capabilities` field** (rejecting D), and no change to `RenderTarget::setSamples`'s signature.

## Consequences

- Good: `max_samples` is finally a number that can be handed straight to `setSamples`; the two backends give the same answer (a refusal) to the same request;
  `shape().samples` and the object's count can no longer differ; the two "silent downgrades" in `vk` are gone; F8 / F10 of the design page and the code say the same thing.
- Bad / cost:
  - a host that writes a number beyond the device's ability gets no picture (one warning + `NotReady`). That is **deliberate**: `Capabilities` is the entry point that
    exists for exactly this, and "I declare 16, you draw 4" cannot be expressed by any number in this shape. A host that really wants "lower automatically" needs
    a new negotiation entry point and a new ADR.
  - `vk`'s "multisampling plus a samplable depth" changes from "draws (lowered to single-sample)" to "does not draw": `vk` does not resolve depth. The real fix
    is to add depth resolve to `vk` (backlog `T40`), and then this combination passes naturally and the two backends agree again.
  - `vsg`'s `capabilities().max_samples` goes from 15 to 8: a host that read the bitmask (there is none today) sees a change, while a host that read a count
    goes from "may crash" to correct.
- Trigger conditions (when to reopen):
  - a real host appears that must "sample as much as possible" (that is a negotiation entry point, not a default downgrade);
  - or a third API-family backend appears and the shape of `max_samples` no longer holds (the cross-API round, see `T30`).

## Evidence

- Before (verified by reading the code): `VsgBackend::capabilities()` / `setMaxSamples(...)` pass
  `std::min(framebufferColorSampleCounts, framebufferDepthSampleCounts)` straight through; `HostTargets::build` only compares
  `description.samples > max_samples`; `Device::ensureTarget` calls `clampSampleCount` and has two downgrade branches.
- After (tests):
  - `tests/test_vsg/MsaaTargetTest.cpp`: over the limit (`setMaxSamples(4)` + `setSamples(8)`) and "not a count"
    (`setSamples(3)`) both answer `HostTargets::State::SamplesUnsupported`;
  - `tests/test_vsg/BackendParityTest.cpp`: `TheSampleLimitsAgreeBetweenTheBackends` - both backends on the same machine
    answer the same count, and it is a power of two;
  - `tests/test_vk/MsaaTest.cpp`: over the limit is **refused** (`draws` 0, `readColorBuffer` fails, reported once and only once),
    3 samples is refused, and samplable depth plus multisampling is refused (the state of `T40`).
- Design side: `../design/graphics-rendering.md` §3.2 F8 / F10; the contract text of `Capabilities.hpp` / `RenderTarget.hpp`.
