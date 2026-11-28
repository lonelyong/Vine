# Graphics review 28 (2026-11-28): the device's limits, the one nobody read, and a default that was a ceiling

> **Status**: the extent ceiling is read once and refused against, the table's defaults are Vulkan's floors, and two
> limits that sit under the floor are guarded at compile time. `test_vk` **246/246**, `test_graphics` 714/714, all gates
> green. One behaviour changed: a target declaration above the device's image/framebuffer/viewport dimension is now
> refused with both numbers instead of reaching `vkCreateImage`.
>
> **Scope**: the seam review 27 §5 named - "what is this device's limit" - asked as ONE question across the four parties
> that answer it: `VkPhysicalDeviceLimits` (the driver), `Capabilities::Limits` (the SDK answer), `TargetTable::Limits`
> (the table's copy of two of them) and `DynamicRendering`'s stack arrays (which trust the API cap).
>
> **In one line**: three of the four parties answered the same question differently - the extent had no reader at all,
> the colour default was the API's ceiling while its own doc forbade exactly that, and two limits under the guaranteed
> floor were trusted without saying so - so the driver's numbers are now read in one place, forwarded as numbers, refused
> at the seam that builds the target, and defaulted to the floor.

## 1. The census (before)

| Party | What it answered |
|---|---|
| `VkPhysicalDeviceLimits` | the driver: `maxImageDimension2D`, `maxFramebufferWidth` / `maxFramebufferHeight`, `maxViewportDimensions`, `maxColorAttachments`, the sample-count bit sets, `lineWidthRange`, `maxSamplerAnisotropy`, `timestampPeriod` … (llvmpipe on this host: 16384 / 16384 / 16384 / 8) |
| `Capabilities::Limits` (SDK) | anisotropy, `max_samples`, `max_line_width`, `max_color_attachments` - **no extent** |
| `TargetTable::Limits` | the sample-count set, depth resolve, the buildable-format mask, and `max_color_attachments` **defaulting to the API's 8** |
| `DynamicRendering` stack arrays | sized by `kMaxColorAttachments` = 8 (the API ceiling), trusting the table to have refused more |

Two findings, one of them live:

- **A1 - the extent ceiling was read by nobody.** `TargetObjects::createAttachments` refused `<= 0` extents and more
  than 8 colours, and nothing else. A declaration wider than `maxImageDimension2D` reached `vkCreateImage` as
  `VUID-VkImageCreateInfo-extent-00944`, and on a driver that refused it the report was `vk.target.build_failed` -
  "see the memory report" - which names the wrong cause entirely.
- **A2 - a default that was the API ceiling.** `TargetTable::Limits::max_color_attachments` defaulted to
  `kMaxColorAttachments` (8) while the field's own comment said the API's ceiling is "never a substitute" for the
  device's answer. The live path always passed the device's number (review 25 `B1`), but `close()` resets to the
  default, and the next opener that forgot the field would have accepted 5..8 attachments on a 4-attachment device -
  the exact defect `B1` fixed, reachable again through the default.
- **A3 - two limits under the floor were trusted without saying so.** The push range is exactly 128 B and a content pass
  binds `kTextureSlots + 1` samplers; both are inside Vulkan's guaranteed minimums (`maxPushConstantsSize >= 128`,
  `maxPerStageDescriptorSamplers >= 16`), which is why no device answers was ever needed - but nothing tied the ABI to
  those floors, so a sixth slot or a third matrix would have crossed them silently.

## 2. The industry answer to the same question

- **Vulkan's own limits table separates two facts**: every limit has a *Minimum* column (what a conformant device
  guarantees: `maxColorAttachments` 4, `maxImageDimension2D` / `maxFramebufferWidth` / `maxFramebufferHeight` /
  `maxViewportDimensions` 4096, `maxPushConstantsSize` 128, `maxPerStageDescriptorSamplers` 16) and a *Maximum* column
  (`maxColorAttachments` 8 - the API's cap). A default drawn from the Minimum column under-promises safely; one drawn
  from the Maximum column over-promises. A2 was the second kind.
- **WebGPU / wgpu** make the same split explicit: `Limits::default()` is the base set every implementation supports,
  `adapter.limits()` is what the adapter really answers, and `create_texture` refuses a size above
  `max_texture_dimension_2d` with a creation error that names the limit. The request is validated at the seam that owns
  the object - never handed to the backend as an unspecified failure.
- **Diligent Engine** fills one `DeviceProperties` at device initialization (`MaxTexture2DDimension`,
  `MaxTexture1DDimension`, …) and validates every `TextureDesc` against it. One struct, filled once, compared at
  creation: the shape this review gave `TargetTable::Limits`.
- **vk-bootstrap** exposes `physical_device::get_properties()` (the driver's struct) and leaves validation to the
  application - which is precisely why the application's table must *carry* the number: it is the party that compares.

The reading: the driver is the only authority; a capability struct may restate its numbers; a *second* struct may carry
them further, but a default in that struct is an answer too, and it must come from the spec's floor. The extent was the
one number that stopped at the driver.

## 3. The fix

- **`Capabilities::Limits` gains `max_target_extent`**: the largest width or height one target may declare, stated as
  the MINIMUM of `maxImageDimension2D`, `maxFramebufferWidth`, `maxFramebufferHeight` and
  `maxViewportDimensions[0..1]` - one target must fit all five, so one number is the honest answer. Neutral default 1,
  like the rest of the struct ("nothing answered"). `RenderTarget::setSize` points at it, the way `setSamples` already
  pointed at `max_samples`.
- **`Device::initialize` reads it once** (`properties.limits.` is read there and nowhere else) and forwards it to
  `TargetTable::open` beside the colour ceiling.
- **`TargetTable::ensure` refuses a declaration above it**: `vk.target.extent_above_limit`, one episode (`ReportOnce`),
  `DiagnosticLimit{ requested, supported }` with the larger side of the declaration as `requested`. Nothing is created,
  the pass is skipped, and `readColorImage` answers `NotReady` - the same shape as the colour-count refusal next to it.
  Zero extents keep their own episode (`vk.target.size_unknown`).
- **The defaults become floors**: `max_color_attachments` 4 (was the API's 8), `max_target_extent` 4096, with the rule
  stated once on the struct: a table that was never handed a device's answer must under-promise.
- **The two floor-guarded limits are `static_assert`s** in `ContentAbi.hpp`: the push range's 128 B now names the
  guarantee it rests on, and `kTextureSlots + 1 <= 16` ties the binding table to `maxPerStageDescriptorSamplers`' floor.

## 4. What deliberately stays

- **`DynamicRendering`'s 8 is still the array bound.** It is the spec's Maximum column for `maxColorAttachments`, which
  makes it the correct size for a fixed buffer; the *device answer* is what the table refuses against (review 25 `B1`).
  The two roles are different and both are now stated at their sites.
- **The sample and borrow refusals keep `UnsupportedRequest`.** `TargetBuildFailed`'s definition ("an off-screen
  RenderTarget could not be built") would cover them, but six existing cases count that category, so aligning it is a
  host-visible contract change and deserves its own decision rather than riding a limits review.
- **The extent is not clamped for the window.** The window's images come from the surface's capabilities
  (`currentExtent` / `maxImageExtent` bound the swapchain), so the off-screen seam is where the device's extent answer
  applies; the window path asks the surface, which is the party that owns that answer.

## 5. Gates

- `test_vk` **246/246** (47 suites; the new case is the 246th), `test_graphics` 714/714.
- The new case is `VkContentTargetTest.ATargetAboveTheReportedExtentLimitIsRefusedNotDrawn`: it reads the REPORTED
  ceiling and declares ceiling + 1, so it holds on any device (on lavapipe's 16384 it pins the branch a hard-coded
  number would miss). `BackendTest` asserts the answer is at least the guaranteed 4096.
- Mutation: the refusal's `>` loosened by one (`> ceiling` → `> ceiling + 1`) reddens exactly that case and nothing else
  was run against the mutant; the reverse patch restored the file byte-for-byte (`grep` for the mutant: 0), the rebuild
  is warning-free, and the full suite is green again.
- Diagnostics registry: `vk.target.extent_above_limit` registered (`115/115` codes in use and registered, 0 uncoded
  calls, 0 suspicious calls in 21 files).
- Documentation gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`,
  `check_doc_symbols.py` - all 0 findings; `check_include_hygiene`, `check_api_style`, `check_export_annotations`,
  `check_vk_exposure`, `check_vk_layout_literals`, `check_vk_bindings` - all 0 findings.

## 6. What the next round should open

- The same question one layer up: **"what does the SURFACE allow"**. `VkSurfaceCapabilitiesKHR` answers
  `currentExtent` / `minImageExtent` / `maxImageExtent` / `minImageCount` / `maxImageCount` / `supportedUsageFlags` /
  `supportedTransforms` / `supportedCompositeAlpha`, and `WindowTarget` chooses a swapchain configuration against them -
  read it as one table (what the surface says, what the swapchain asks, what a re-create does when the surface changes
  its answer) the way this review read the device's.
- `T78` still waits for its host (point-size ceiling), and `B2`'s platform branch still needs a device without
  `wideLines`.
