# Graphics review 26 (2026-11-28): one pass, two files, one question

> **Status**: one seam audited end to end, one rule made true in code and one gate added. `test_vk` **245/245** (one new
> case), `test_graphics` 714/714, `ctest vk_layout_literals` 0 findings, doc gates green.
>
> **Scope**: the seam review 25 §6 named - `BarrierPlan`'s transition vocabulary against `DynamicRendering`'s pass values -
> asked as ONE question: **"which layout does this image hold at this point?"** Every place the device is told a layout
> was read, not only the barriers.
>
> **In one line**: the question had one answer for the barrier and a second spelling at every other consumer; fourteen
> raw `VK_IMAGE_LAYOUT_*` constants in seven files now read the table's rows, the pass' entry goes through the batch the
> table owns, and a source gate fails the next literal placed outside the table.

## 1. How the seam was read

The review's question breaks into three asks, and each was checked separately:

1. **Who states the layout, and from where?** A barrier built by `BarrierPlan` is one consumer; the rendering attachment
   info (`imageLayout` / `resolveImageLayout`), a descriptor's `imageLayout`, a copy's layout argument and an image's
   creation `initialLayout` are others - all of them tell the device where an image is, and all of them are read against
   the same row.
2. **Who moves the record?** `TargetObjects::beginPass` / `WindowTarget::beginPass` write the usage back after the
   instance opens. That is the pass model's role-to-usage rule (a colour role is a colour attachment by definition), not
   a second spelling of a Vulkan constant - the Vulkan value behind it lives in the table alone.
3. **How is a pass' entry stated?** It was the one place that assembled `VkImageMemoryBarrier2` values by hand, because
   it needs a barrier between two IDENTICAL states (the execution dependency dynamic rendering does not insert) and
   `BarrierBatch::move` refuses exactly that case by design.

## 2. The census (before)

| File | Sites | What the constant stated |
|---|---|---|
| `DynamicRendering.cpp` | 4 | the instance's colour/depth `imageLayout` and `resolveImageLayout` |
| `ContentDraw.cpp` | 2 | the sampled slots' descriptor `imageLayout` |
| `FrameScratch.cpp` | 2 | the shadow pair's descriptor `imageLayout` |
| `ContentImages.cpp` | 3 | two creation `initialLayout`s and the upload copy's layout |
| `TargetObjects.cpp` | 1 | a target image's creation `initialLayout` |
| `WindowTarget.cpp` | 1 | the window depth image's creation `initialLayout` |
| `ReadbackStaging.cpp` | 1 | the readback copy's layout |

Every one of those was equal to a `BarrierPlan` row, and nothing made them stay equal. The table's own header already
claimed to be "the one place where a neutral usage becomes a Vulkan layout"; the claim was true for barriers and false
for every other consumer.

## 3. The fix

- **The table answers every consumer**: `stateOf`, `transferReadState` and `transferWriteState` are read at each site
  above (`.layout`), and a consumer that repeats the value - a draw's five texture slots - takes it once instead of once
  per slot. The barrier that moves an image and the command that follows it are now the same row, so they cannot
  disagree; the one deliberate pass-through is `ReadbackStaging`, which takes the caller's `transferReadState().layout`
  as an argument because that unit names no render-seam type at all (`C10` / E2). A `constexpr` spelling was tried first
  and withdrawn: the definitions live in the `.cpp`, `constexpr` implies `inline`, and the other TUs then had no
  definition (19 `-Wundefined-inline` warnings and a link error) - a property no consumer asked for.
- **`BarrierBatch` gained its second verb**: `order` appends a barrier between two usages WITHOUT writing a record and
  without skipping the identical-state case; `move` is now `order` plus the record write-back. `beginRendering` states
  every attachment of a pass through one batch (`order` per role, one `flush`), so the recorder no longer builds
  `VkImageMemoryBarrier2` values - the "same-state barrier IS the dependency" rule lives in the unit that owns barriers.
- **The gate**: `scripts/check_vk_layout_literals.py` (ctest `vk_layout_literals`) fails when `VK_IMAGE_LAYOUT_*` appears
  outside `BarrierPlan.{hpp,cpp}`, and fails when the table stops stating any of its seven rows - so a rename cannot
  silence the check. Mutation: a probe literal appended to `FrameScratch.cpp` reddens it (1 finding), removing it
  restores 0.

Witnesses: the new pure case `BarrierPlanTest.ABatchOrdersASameStateBarrierButOnlyMovesAChangedOne` pins the two verbs
(an equal record moves nothing, `order` still appends that barrier, a full batch refuses without writing a record), and
the existing `VkContentDrawTest.APassEntryIsOneDependencyWhateverItWrites` keeps `beginRendering`'s one-dependency
answer under the batch. The pixel suite is the behavioural guard for the rendering-info rows: the same value now drives
the barrier and the instance, and validation stayed silent (0 driver messages).

Mutation on the new verb: `order` made to refuse the identical-state pair (the old `move` rule) reddens exactly the new
case (1 failed / 7 passed in `BarrierPlanTest`), and the reverse patch restored the file byte-for-byte.

## 4. What deliberately stays

- **The write-backs** (`TargetObjects::beginPass` / `WindowTarget::beginPass`): they store the USAGE, which is the pass
  model's own vocabulary, not a Vulkan constant. Passing the destination usage out of `beginRendering` would replace one
  constant with a returned struct for no reader (`G8`).
- **`kMaxColorAttachments * 2 + 2` storage in `beginRendering`**: the arithmetic is the roles a pass states, and the
  batch's bound check turns a wrong size into a `false` instead of an overflow.
- **`ImageState`'s default `layout{ VK_IMAGE_LAYOUT_UNDEFINED }`** in the header: it is the table's own file, and the
  default states "an unset state means Undefined", the same value the first row holds.

## 5. Gates

- `test_vk` **245/245** (one new case), `test_graphics` 714/714, `test_appfw` unaffected.
- `ctest -R vk_layout_literals` 0 findings; the mutation of the gate run and restored by deleting the probe line.
- Documentation gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`, `check_doc_symbols.py`.

## 6. What the next round should open

- The binding numbers are the next one-question seam of the same shape: the set layouts (`FrameScratch`), the shader ABI
  (`ShaderAbi.hpp`, `vine_*.glsl`) and the descriptor writes each spell "slot 1 is the textures, 6 the lights, 7/8 the
  shadow pair" - three files, one question, and a mismatch is a validation error rather than a compile error.
- `T78` still waits for its host (a point size with no ceiling), and `B2`'s platform branch still needs a device without
  `wideLines`.
