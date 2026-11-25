# ADR-2026-11-25: a readback answers with one value — the pixels and the classification together

- Status: **accepted** (2026-11-25).
- Date: 2026-11-25
- Scope: the SDK's readback vocabulary (`Readback.hpp`), the device verbs (`RenderDevice::readColorBuffer` /
  `readDepthBuffer` and the two short spellings), the vk device's implementation, and every in-tree reader (17 test
  files, the demo chain helpers).
- Related: `graphics-rendering.md` 2.9, `F5`, `C5`, `P4`, `T7`, §9.3; `vk-backend.md` F11 / §3.1.11; `BC-41`;
  backlog `T70` (the seam's classification table has no consumer). Supersedes the answer-shape half of the audit-4
  `T-05` / `B4` record (2026-11-18), whose shims this batch deletes.

## Background

One week before this ADR, `B4` gave the readback verbs a machine-readable answer and kept the old shapes alive as
`[[deprecated]]` shims (`BC-16`). The result was three answers for one question, and the deprecation never took
effect: every in-tree caller kept using an out-parameter, so the "new" shape was exercised only by the tests that
named it.

| Shape | Spelling | What a caller had to do |
|---|---|---|
| two answers | `bool readColorBuffer(const RenderTarget*, int, std::vector<uint8_t>&, ReadbackStatus*)` | declare a vector, a status, and hope the read wrote both |
| a code | `ReadbackOutcome readColorBuffer(const RenderTarget*, int, std::vector<uint8_t>&)` | declare a vector, then read `outcome.result` |
| a code that wraps a code | `ReadbackResult` (enum) inside `ReadbackOutcome` (struct) | two names for one classification, and the outer one existed only for an implicit `bool` |

Two concrete defects followed from that shape:

1. **A refused read could be read as a picture.** The vk device writes the caller's vector only on success
   (`Device.cpp`, `outPixels.resize(...)` / `memcpy` after the fence, `outDepths = std::move(values)`), which is the
   right behaviour and the wrong contract: the vector the caller already owned keeps its previous contents, so
   `readColorBuffer(image, pixels)` followed by `pixels[0]` is a compile-clean read of last frame's picture.
2. **The payload was a side channel.** Both verbs already build the picture as a value internally (the staging copy is
   `memcpy`'d into a vector; the depth path is `std::vector<float> values = decodeDepth(...)`), so the out-parameter
   only ever re-copied a value the function already had.

Consumption said the wrapper was not earning its keep: of 99 readback call sites, 76 read the answer in a boolean
context, and the two payload-less verbs in the same family (`present` 0/114, `createSurfaceTarget` 0/24) use a boolean
context nowhere - so `ReadbackOutcome`'s implicit `operator bool` was the only reason it existed.

Industry peers answer with status + product in one value, not with a caller buffer: WebGPU's `mapAsync` reports a
`BufferMapAsyncStatus` and the mapped range is the product; Vulkan's readback is a `VkResult` plus a mapped staging
range; Filament delivers the pixels only when the read succeeded; bgfx fills caller memory but documents that the
result is only valid after a frame. None of them asks the caller to keep a buffer whose contents are "last time's".

## Options

- **A. Keep the shims.** Migrate callers to `ReadbackOutcome`, keep `bool` + out-parameter for one release. Rejected:
  it is what the previous week did, and the shims outlive their release because nothing forces the deletion; the
  vocabulary stays three-way until then, and defect 1 stays in the deprecated shape for every caller that has not
  moved (`G1`).
- **B. Status only.** Keep the caller's vector, return a code. Fixes the "two answers" but not "stale picture" and not
  the wrapper: the caller still cannot tell a refused read's buffer from a good one (`G4`).
- **C. One value, the answer owns its product.** The verb returns the classification together with the picture it
  classified. A refusal carries no picture at all, so defect 1 becomes unrepresentable.

## Decision

**C**, with these choices:

1. **The payload is a template, bounded to this family.** `template<class T> struct ReadbackResult { ReadbackStatus
   status; std::vector<T> values; }`, with `ColorReadback = ReadbackResult<std::uint8_t>` and `DepthReadback =
   ReadbackResult<float>`. Not a general `expected` (`G8`: no error alternative, no monadic operations, no second
   consumer), and not two hand-written structs (the two products differ only in the element type, and one accessor pair
   is one place for the invariant "`values` is populated exactly on `Ok`").
2. **The code is `ReadbackStatus`, the answer is `ReadbackResult<T>`.** The naming rule the family now states: a verb
   with no product returns a `*Result` code (`SurfaceTargetResult`, `BackendRegistrationResult`); a verb with a product
   returns a `*Result` carrying its `Status` (`PlanResult`, `FrameResult`, `ReadbackResult<T>`). The code lives at family
   level, not inside the template: a nested `Status` would make `ColorReadback::Status` and `DepthReadback::Status` two
   incompatible types for one vocabulary.
3. **Nothing is deprecated; the old shapes are deleted.** `ReadbackOutcome`, the `ReadbackResult` enum name, and all six
   `bool` / out-parameter overloads are gone in the same commit that moved every in-tree caller - the module promises
   no source or ABI stability (`graphics-frame-pipeline.md` 1.3) and 2.10 already set that precedent for names the
   previous batch had left as aliases. `BC-41` records the change; `BC-16`'s "one release" promise is closed by
   deletion, because nothing was released while the shims existed.
4. **The device allocates.** A verb that filled a caller's buffer would have to decide whether a refusal leaves it
   alone, and the caller cannot tell. A caller that wants to keep an allocation keeps `values` and moves it back in; no
   `readInto(std::span)` door is opened while nothing demands one. If readback ever becomes a per-frame streaming path
   (recording, video export), that door is the one to add, and it is additive.
5. **The seam loses its sentinel.** `readbackOf()` answers `std::optional<ReadbackRefusal>` (`nullopt` = servable)
   instead of `ReadbackAnswer{bool ok; ReadbackRefusal refusal;}` + `ReadbackRefusal::None`, so "served" and "which
   reason" cannot disagree. `readbackResultOf()` is renamed `readbackStatusOf()` (it returns a `ReadbackStatus`).
6. **Two short spellings stay, as conveniences.** `readColorBuffer(target, attachment = 0)` and `readDepthBuffer(target)`
   build the `ImageRef` and call the virtual, so "attachment N of that target" is one line at 70 test call sites and two
   spellings still name ONE implementation (`G1`): they are a derivation, not a second path.

## Consequences

- A host writes `const ColorReadback read = device->readColorBuffer(target, 0);` and then `read.ok()`, `read.status`,
  `read.values`. A refusal cannot be mistaken for a picture, and no caller owns a buffer the device may or may not write.
- Migration is source-breaking by design: 99 call sites (17 test files), the vk device's two verbs, and the docs. Every
  reader moved in this commit; the tests now demonstrate the answer shape rather than the shims.
- The vk device's hot path loses nothing: the picture was already built as a value and is now moved out instead of
  copied into a caller vector (`return ColorReadback{ ReadbackStatus::Ok, std::move(pixels) }`).
- `ReadbackResult<T>` is header-only and per-instantiation; the exported surface is unchanged (`ReadbackStatus`,
  `readbackStatusOf`, `readbackOf`, `refusalName`, the two format tables, `decodeDepth`).
- Open, recorded as backlog `T70`: the seam's classification table (`readbackOf` / `ReadbackState` / `ReadbackRequest` /
  `refusalName`) still has no backend consumer - the vk device classifies inline from its own `TargetTable::peek`. That
  is two implementations of one classification and a separate decision (wire it in, or delete the table).

## Rejected

- Caller-provided buffer with a status (`B`): the stale-picture trap is exactly a caller-provided buffer (`G4`).
- A general `expected<T, E>` / `std::variant` payload: a second, unrelated consumer would be needed to justify it (`G8`),
  and the failure mode here is a five-case classification a host switches on, not an arbitrary error type.
- Keeping the template unbounded (`ReadbackResult<RenderCommand>` and friends): the aliases are what puts the two products
  in the vocabulary; an unconstrained template would invite a third product before one is asked for (`G8`).
- `ReadbackStatus` nested in `ReadbackResult<T>` (mirroring `PlanResult::Outcome`): two instantiations would be two
  incompatible enums, so `readbackStatusOf()` could not return "the" status (`G1`).

## References

- `graphics-rendering.md` 2.9 (the design point and the naming rule), `F5`, `C5`, `P4`, `T7`, §9.3 (migration note).
- `vk-backend.md` F11 / §3.1.11.
- `sdk/vine/graphics/Readback.hpp`, `sdk/vine/graphics/RenderDevice.hpp`, `src/plugins/gfx_backend_vk/src/internal/Device.cpp`.
- `BC-41` in `graphics-rendering.md` §2.9; the closed `BC-16` answer-shape half.
- Evidence for the premise: `Device.cpp` wrote the caller's vector only after a whole picture existed, and both verbs
  already built the picture as a value; 76 of 99 call sites read the answer in a boolean context.
