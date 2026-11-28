# Graphics review 30 (2026-11-28): the queue family's answers, the one we asked of the device

> **Status**: the frame clock and `GpuTimings` are now the CHOSEN queue family's answer (`timestampValidBits`, with the
> device's period) instead of the device-wide `timestampComputeAndGraphics` floor, and the family's two copy answers are
> recorded as pinned by the bit that chooses it. `test_vk` **258/258** (48 suites), `test_graphics` 714/714, all gates
> green.
>
> **Scope**: the seam review 29 §6 named - "what do the QUEUE FAMILY properties allow" - asked as one question: which
> of `VkQueueFamilyProperties`' four answers does this backend read, and which is it entitled not to?
>
> **In one line**: the family's timestamp bits were read by nobody while the clock was gated on a device-wide promise,
> so a device that supports timestamps on the queue we actually use could not time a frame; the copy answers turned out
> to be pinned by the same `GRAPHICS_BIT` that selects the family, which is now said where the choice is made.

## 1. The census (before)

| `VkQueueFamilyProperties` member | Read by | Before review 30 |
|---|---|---|
| `queueFlags` | the candidate filter | `GRAPHICS_BIT` required; `vkGetPhysicalDeviceSurfaceSupportKHR` asked when a window is named |
| `queueCount` | nobody | one queue created at index 0 - legal because the spec guarantees at least one queue per family |
| `timestampValidBits` | nobody | the clock was gated on `VkPhysicalDeviceLimits::timestampComputeAndGraphics` |
| `minImageTransferGranularity` | nobody | every copy records at offset 0 with the image's own extent - legal for a graphics family |

The one finding:

- **Q1 - the clock asked the device, not the queue.** The spec defines `timestampComputeAndGraphics` as a FLOOR: "If
  this limit is VK_TRUE, all queues that advertise the GRAPHICS or COMPUTE bit support `timestampValidBits` of at least
  36". A false therefore describes a MIXED machine, not this queue - while the family's own `timestampValidBits` is the
  answer for the queue every frame records on, defined as 0 (no timestamps) or 36..64. The backend read the floor alone,
  so a device whose chosen graphics family answers 64 bits still left `FrameReport::timing` DISENGAGED and
  `Features::GpuTimings` off for the whole session: a capability refused, the mirror of review 28's limit trusted.

Examined and cleared:

- **Q2 - the copy answers are pinned by the selecting bit.** `VkQueueFamilyProperties` states that queues supporting
  graphics and/or compute (and, with maintenance11, transfer) must report (1,1,1) `minImageTransferGranularity`, and
  that each family supports at least one queue. The selection requires `GRAPHICS_BIT`, and every upload and readback is
  recorded in a command pool of that family (`FrameDriver`'s pool, `ReadbackStaging::open(impl->queue_family)`) - so the
  regions are inside the granularity by construction and queue 0 exists. No runtime ask: an answer below (1,1,1) is
  impossible on a conformant device and has no consumer today (G8); the invariant is stated where a future transfer-only
  family would have to read it.

## 2. The industry answer to the same question

- **wgpu's Vulkan backend** (`wgpu-hal/src/vulkan/adapter.rs`) enables `TIMESTAMP_QUERY` and its two siblings from
  `queue_props.timestamp_valid_bits >= 36`, with the comment "Vulkan strictly defines this as either 36-64, or zero".
  `queue_props` is the adapter's queue-family choice, and `expose_adapter` refuses an adapter whose first family does not
  carry `GRAPHICS` - so the value is the chosen graphics family's answer, the same question asked here.
- **The spec's two pages** are the two halves: `VkPhysicalDeviceLimits` states the device-wide floor above,
  `VkQueueFamilyProperties` states the per-family answer and its valid range.
- **vk-bootstrap** selects queue families by flags and does not surface timestamp support separately, so nothing to copy
  from it for this seam.

## 3. The fix

- **`queueWritesTimestamps(timestamp_valid_bits, timestamp_period)`** (`DeviceChoice.hpp`, beside `candidateRefusal`):
  the second decision that file pins over fabricated answers. It answers `valid_bits >= 36 && period > 0` - the spec's
  smallest usable count, and the period without which ticks are not nanoseconds.
- **Read where the family is chosen**: `Device::initialize`'s candidate loop carries `families[serving_family]
  .timestampValidBits` on the `Candidate`, and the chosen value lands in `Impl::timestamp_valid_bits` beside
  `queue_family`.
- **One rule, two consumers**: the query-pool engagement and the `GpuTimings` bit both call `queueWritesTimestamps`, so a
  feature that answers YES always has a report behind it (`Capabilities.hpp`'s own rule).
- **The SDK doc now says which queue**: `Features::Feature::GpuTimings` reads "The device can write timestamps on the
  queue this backend runs on".
- **The pin is stated at the selection**: the comment above `serving_family` names the two copy answers the
  `GRAPHICS_BIT` settles and what a future transfer-only family would have to read.

## 4. What deliberately stays

- **No runtime check of `minImageTransferGranularity` / `queueCount`**: the spec pins both for a graphics family, and
  no conformant device can fail them - a check would be a guard nothing can turn red (G8). The comment and this page are
  the record; a transfer-only family would be a different selection and would have to ask.
- **The period stays the device's answer**: `timestampPeriod` is a device property; only the per-family question (the
  bits) was read at the wrong level.
- **No `VkQueueFamilyProperties2` / maintenance11 optimal granularity**: no consumer, and the spec's floor for the
  chosen family makes the optimal hint a performance question, not a correctness one.
- **The clock stays sampled** (`kTimestampSampleFrames`): this round changes which families may engage it, not how often
  a frame pays for it.

## 5. Gates

- `test_vk` **258/258** (48 suites; the four `VkDeviceChoiceTest` clock cases are new), `test_graphics` 714/714, and the
  real-device clock cases (`VkPassTimingTest.*`, `VkDeviceClockTest.AFrameReportsItsDeviceTime`) green.
- Mutations, both restored byte-identically (`cmp` clean, no mutant markers, rebuild warning-free, suite green):
  - **M1** - the floor weakened (`timestamp_valid_bits >= 36U` → `> 0U`): exactly
    `ABitCountVulkanDoesNotDefineIsNotAClock` reddens, the other eight device-choice cases stay green.
  - **M2** - the wiring broken (the clock gate asked `queueWritesTimestamps(0U, period)` while `GpuTimings` kept the
    real rule): both `VkPassTimingTest` cases redden - the feature advertises a clock the report never carries.
- **Why M2 is the wiring witness, not the carried value**: zeroing `Impl::timestamp_valid_bits` would turn the feature
  OFF, and the real-device case would SKIP instead of redden - a silent loss of coverage. The rule itself is pinned by
  the synthetic cases; the real-device case pins the agreement between the two consumers.
- All static and doc gates 0 findings (`check_ai_docs --strict`, `check_design_docs`, `check_cross_refs`,
  `check_doc_symbols`, `check_include_hygiene`, `check_api_style`, `check_export_annotations`,
  `check_diagnostic_formats`, `check_public_docs`, `check_vk_exposure`, `check_vk_layout_literals`,
  `check_vk_bindings`, `measure_graphics_claims`).

## 6. What's next

- **"What does the MEMORY heap allow"**: `bufferImageGranularity` is read and obeyed, but the heap sizes
  (`VkPhysicalDeviceMemoryProperties::memoryHeaps[].size`) and `maxMemoryAllocationCount` are read by nobody - a pool
  that never compares a block request against them can only learn "out of device memory" from the driver, after the
  attempt. The same shape as reviews 28-30: read where the device opens, refuse (with numbers) where the block is asked
  for. Trigger: a block request sized near the heap, measured first.
- The load-sensitive pass-timing witness stays backlog `T79` (review 29 §5).
