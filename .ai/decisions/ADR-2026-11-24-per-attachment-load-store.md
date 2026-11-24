# ADR-2026-11-24: per-attachment load and store — the pass states the pair, the backend resolves it

- Status: **accepted** (2026-11-24).
- Date: 2026-11-24
- Scope: the SDK's attachment-policy vocabulary (`AttachmentLoadStore`, `ClearPolicy::color_ops` / `depth_op`,
  `PassDesc::color_ops` / `depth_op`) and the vk backend's per-attachment `PassLoad` (`DynamicRendering`).
- Related: `graphics-rendering.md` §2.10 and `BC-39`; `vk-backend.md` §2.7 and `F4`; `DrawState.hpp`,
  `FrameDescription.hpp`, `PassDesc.hpp`, `RenderPass.cpp`, `DynamicRendering.hpp`. Owner-directed on 2026-11-24;
  no backlog item existed for it.

## Background

A pass' attachment behaviour was stated in two layers that could not agree on ONE attachment:

- `ClearPolicy` said WHICH attachments to reset (`clear_color`, `clear_depth`) and WHAT a colour Clear writes
  (`color`, `extra_colors`) — per group, not per attachment.
- the vk recorder's `PassLoad` carried ONE `load_color` / `store_color` for every colour attachment, beside a
  per-attachment `color_clears` array.

So "clear attachment 0, keep attachment 1, discard attachment 2" — one pass, one rendering instance — had no
spelling. The comment above `ClearPolicy` already said "PER ATTACHMENT" and `BC-39` already claimed "one load policy
per attachment"; the code did not. `StoreOp` existed in the seam but no pass could state it, so `DontCare` was
reachable only through the backend's resolve downgrade.

## Decision

1. **The seam states one pair per attachment.** `vn::graphics::AttachmentLoadStore { LoadOp load; StoreOp store; }`
   is the neutral vocabulary; `ClearPolicy` gains `color_ops` (one per colour attachment, in output order) and
   `depth_op` (optional), and `PassDesc` gains the same two fields so a pass can AUTHOR them.
2. **The group spelling stays the default.** Empty `color_ops` / unset `depth_op` keep `clear_color` / `clear_depth`
   answering every attachment, so every existing pass is unchanged. A stated entry overrides its own attachment.
3. **A pass that states only ops still announces a policy.** `RenderPass::clearPolicy()` returns a `ClearPolicy` when
   either group flag clears OR an op is stated, because "load both, discard one" is a statement the backend must
   receive.
4. **The backend resolves the one fact the engine cannot know.** `DynamicRendering::decidePassLoad` applies the group
   default, the per-attachment override, and ONE safety net: a stated `Load` on a set that holds no picture becomes
   `Clear` with that attachment's value (the group rule already did that). The resolve downgrade stays in
   `beginRendering` — only it knows whether a resolve consumes a role. A BORROWED depth overrides BOTH halves
   (`Load` + `Store`): the image belongs to the lender, so a borrower may neither clear it nor discard what the lender
   still needs (`TargetObjects::beginPass`).
   **NO POLICY MEANS NO CLEAR.** `PassDraw::clear == nullopt` is the pass saying it clears NOTHING, so a later pass
   into a set that already holds a picture LOADs; only a fresh set falls back to `Clear`. The old rule read nullopt
   as "clear both", which wiped the demo's deferred-lighting result (`forward_transparent` and the window overlays all
   announce no clear) and is why the render area was black.
5. **One value per attachment in the recorder.** `PassLoad` is `span<const PassAttachment> color` + one
   `PassAttachment depth`, and each `VkRenderingAttachmentInfo` is filled from its own entry.

## Rejected options

- **Backend-only per-attachment (`PassLoad` arrays, no seam field).** It would have no author: the engine could not
  state the ops, so the capability would be dead weight (`G8`).
- **Replace `ClearPolicy` with a new `AttachmentPolicy` type now.** The name is broader than the type (`store` is not
  a clear), but the type already carries the clear values and the ops extend the same statement; a rename with no
  behaviour change is a separate word batch, and doing it here would have doubled the diff for no witness (`G8`).
- **A `std::optional<LoadOp>` / `StoreOp` per attachment instead of a pair.** Two half-stated attachments are one
  mistake ("store is not stated, so what?"), and every API family states the pair (`G4`).
- **Per-attachment `loadOp` / `storeOp` only in `PassDesc`, derived in `RenderPass::describe`.** The engine cannot
  answer "does the target hold a picture", so the fallback would have to live in the backend anyway; keeping the
  intent (policy) and the resolution (backend) separate is what the two layers already did.

## Consequences

- A pass can now state, in one rendering instance: attachment 0 `Clear`, attachment 1 `Load`, attachment 2
  `DontCare` / `DontCare`; the depth pair is independent (`Clear` + `DontCare` for a depth-only pass nobody reads).
- `ClearPolicy` grows two fields (an SDK value ABI change; every plugin is rebuilt with the SDK, as the repository
  already requires). Existing passes compile and behave identically because the defaults are the old spelling.
- The seam is ready for D3D12 / Metal: both state the same pair per attachment, and a Metal backend maps it to
  `MTLLoadAction` / `MTLStoreAction` without a second engine concept.
- Evidence: `test_vk`'s `PassLoadTest` (six device-free cases) and
  `VkContentTargetTest.EachColourAttachmentLoadsOrClearsOnItsOwn` (one pass, three answers, pixels read back);
  `test_graphics`' `RenderEngineTest.APassAnnouncesPerAttachmentOpsToTheBackend` (the policy reaches the backend
  without a group clear).
