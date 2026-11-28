# Graphics review 29 (2026-11-28): the surface's answers, the two we assumed

> **Status**: the surface's capabilities and format list are now resolved into one value every field of
> `VkSwapchainCreateInfoKHR` is filled from, two unchecked asks became explicit answers (the colour-usage bit and the
> `VK_FORMAT_UNDEFINED` sentinel), and the branches no desktop produces are pinned by a synthetic table. `test_vk`
> **254/254** (48 suites), `test_graphics` 714/714, all gates green.
>
> **Scope**: the seam review 28 §6 named - "what does the SURFACE allow" - asked as ONE question: which of the swapchain
> request's fields are the surface's answers, and which are still assumptions?
>
> **In one line**: five of the seven fields a surface answers were handled inline in `createSwapchain`, one was not asked
> at all (`imageUsage`), and one answer the surface may legally give (`VK_FORMAT_UNDEFINED` = "no preferred format") was
> returned to the driver verbatim - so the answers are resolved into `SurfaceSwapchain`, and the two assumptions are now
> a checked bit and a substitution, each with a case that only a synthetic surface can produce.

## 1. The census (before)

| Field of `VkSwapchainCreateInfoKHR` | Answered by | Before review 29 |
|---|---|---|
| `imageFormat` / `imageColorSpace` | the surface's format list | `chooseSurfaceFormat` picked the sRGB pair, but returned the list's first entry verbatim - and one legal first entry is the `VK_FORMAT_UNDEFINED` sentinel |
| `imageExtent` | `currentExtent`, or the announced size clamped | handled inline |
| `minImageCount` | `minImageCount + 1`, capped by `maxImageCount` | handled inline |
| `compositeAlpha` | `supportedCompositeAlpha` | fallback inline (OPAQUE, else POST/PRE/INHERIT) |
| `imageUsage` | `supportedUsageFlags` | **never asked**: the constant `COLOR_ATTACHMENT` was requested (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276) |
| `preTransform` | `currentTransform` | the surface's own answer - correct as stated |
| `presentMode` | the present-mode list | FIFO hardcoded - guaranteed supported, so nothing to ask |
| `imageArrayLayers` | `maxImageArrayLayers` | 1 - Vulkan guarantees at least one |

The two findings:

- **S1 - the usage was assumed.** A surface whose images are not colour attachments has no window target at all (every
  window pass draws into the image), and today that fact arrived as `vkCreateSwapchainKHR` failing with the sentence
  "vk: vkCreateSwapchainKHR refused the window's surface" - which names no cause.
- **S2 - the sentinel was passed through.** `VK_FORMAT_UNDEFINED` in the format list is the spec's "the application
  chooses" answer; `imageFormat` may not be UNDEFINED (VUID-VkSwapchainCreateInfoKHR-imageFormat-01273). A surface that
  answers it today gets an invalid create.

## 2. The industry answer to the same question

- **vk-bootstrap's `SwapchainBuilder`** is the same derivation as a public value: it clamps the desired extent into
  `min/maxImageExtent`, picks the image count inside the surface's range, falls back through composite modes the surface
  states, keeps `currentTransform` when supported, validates every requested usage against `supportedUsageFlags`, and
  substitutes the classic `B8G8R8A8_UNORM` + `SRGB_NONLINEAR` pair when the answer is `VK_FORMAT_UNDEFINED`.
- **wgpu's `Surface::configure`** validates the requested size, present mode and format against what the adapter reports
  and returns an error instead of handing the backend values it already refused.
- **Diligent's Vulkan swapchain** makes the same choices inline, against `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`.
- **The spec's own two VUIDs** (01276, 01273) are exactly the two findings: both are "a surface answer you did not read".

## 3. The fix

- **`surfaceSwapchain(capabilities, formats, announced width, announced height)`** (declared in `WindowTarget.hpp`,
  defined in `WindowTarget.cpp`) resolves the answers into `SurfaceSwapchain`: the format pair (sRGB preference, sentinel
  substitution), the extent (the surface's `currentExtent`, else the announced size clamped into its range), the image
  count (`minImageCount + 1` capped), and a composite mode the surface states.
- **Refusals are values** (`SurfaceRefusal`): `NoFormats`, `ZeroExtent`, and `NoColorUsage` - the last one is S1, said
  before the create, with the target's `why` naming the missing `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`.
- **`createSwapchain` consumes the value**: it fills the info struct from `SurfaceSwapchain` and maps a refusal to the
  sentence it reports. The old inline derivation and `chooseSurfaceFormat` are gone; the comments there state the fields
  that need no resolve and why (FIFO, `currentTransform`, one layer).

## 4. What deliberately stays

- **FIFO, `currentTransform`, one image array layer**: each is guaranteed by the spec's surface requirements, so asking
  would buy nothing (G8).
- **`oldSwapchain = VK_NULL_HANDLE`, old chain destroyed first after `vkDeviceWaitIdle`**: the recreate path trades one
  stall per resize for a chain that is never aliased with the one presentation may still read. Passing `oldSwapchain`
  is an optimisation that needs its own measurements, not this review.
- **The window's pixels are read through the platform's capture**: no `TRANSFER_SRC` usage is requested, so the request
  is one bit and `surfaceSwapchain` checks exactly that bit.

## 5. Gates

- `test_vk` **254/254** (48 suites; the 8 synthetic cases are new), `test_graphics` 714/714, and the real-window cases
  (`VkWindowPresentTest.*`, 7) still present through the new value.
- Mutations: neutering the usage check (`== 0U` → `== ~0U`) reddens exactly
  `ASurfaceWhoseImagesCannotBeDrawnIntoIsRefusedWithItsCause`; making the sentinel substitution unreachable for a
  single-entry list reddens exactly `AnUndefinedFormatAnswerBecomesTheClassicPair`. Both restored byte-identically
  (`cmp` clean, mutant markers 0), rebuild warning-free, suite green.
- All static gates 0 findings (`check_include_hygiene` 0/1175, `check_api_style` 0 new / 1 baseline, `check_export_annotations`,
  `check_diagnostic_formats` 0 suspicious, `check_public_docs` 0 new / 0 stale, `check_vk_exposure`,
  `check_vk_layout_literals`, `check_vk_bindings`).
- **One observation recorded, not a change**: `VkPassTimingTest.AHeavyPassReportsMoreTimeThanACheapOne` went red twice in
  five sequential full-suite runs in a loaded window (the minutes around a rebuild), while 20/20 lone runs, 3/3 quiet
  full runs and 10/10 runs under 8-way CPU load were green. It is unrelated to this seam (the case draws off-screen),
  but it is a witness that can go red spuriously - backlog `T79` records it with the trigger "capture a failing run's
  numbers first, then widen the WORK gap, not the threshold".

## 6. What the next round should open

- **"What do the QUEUE FAMILY properties allow"**: the device-level `timestampComputeAndGraphics` is read (§2.27's
  family), but the chosen queue family's `timestampValidBits` is not - a family with 0 valid bits makes every query
  result undefined - and `minImageTransferGranularity` / the family's flags ride with it. One table, one question, the
  same shape as reviews 28 and 29 (the number read where the device is opened, refused where the work is asked for).
- `T78` (point size) still waits for its host; `T72`'s `SUBOPTIMAL` branch still needs an injection door.
