# Graphics review 27 (2026-11-28): the binding numbers, and the drift they had already produced

> **Status**: the binding table completed, every literal in the backend replaced by it, one stale promise in the SDK
> corrected, and one gate added. `test_vk` **245/245**, `test_graphics` 714/714, `ctest vk_bindings` 0 findings, doc
> gates green. No behaviour changed on any passing path - the numbers are the same values, now with one home.
>
> **Scope**: the seam review 26 §6 named - the binding numbers - asked as ONE question: **"which (set, binding) is each
> descriptor slot?"** across the set layouts, the descriptor writes, the dynamic-offset order, the shipped GLSL and the
> engine's shadow generator.
>
> **In one line**: the numbers were spelled in six places and the drift had already happened - the SDK's own header
> documented the shadowed deferred program at 5/6 while every piece of code said 7/8 - so the table now lives in
> `ContentAbi.hpp`, everything in the backend reads it, and `scripts/check_vk_bindings.py` reads the GLSL side against it.

## 1. The census (before)

| Place | What it spelled |
|---|---|
| `ContentAbi.hpp` | only the shadow pair (7/8) and the full-screen sampler count (5); the material, lights, draw set and set indices had no name |
| `ContentCaches.cpp` (set layouts) | `0`, `1 + slot`, `6`, set 1's `0` as literals |
| `FrameScratch.cpp` (writes) | `0`, `1`, `6` as literals for both sets, `0` for the draw set |
| `ContentDraw.cpp` (offset order) | the order stated in a comment with the digits (`0, 6, 8`, then set 1's `0`) |
| `vine_params.glsl` / `vine_alpha.glsl` / `vine_material.glsl` | `layout(set = …, binding = …)` literals |
| `BuiltinShaders.cpp` | `shadowBindings(0u, 7u, …)` at both call sites |
| `BuiltinShaders.hpp` (the SDK's promise) | "the map at binding 5 and its `VineShadowBlock` at binding 6" - **wrong**: the code said 7/8 |

The doc sentence is the demonstration that this was a live defect and not a style preference: prose drifts first, and
a reader following that sentence would have written a program whose shadow map is read from the depth slot.

## 2. The fix

- **`ContentAbi.hpp` is the complete table**: `kContentSet`, `kMaterialBinding`, `kTextureBindingBase`, `kTextureSlots`,
  `kLightsBinding`, `kShadowMapBinding`, `kShadowBlockBinding`, `kFullscreenSamplerBindingBase`,
  `kFullscreenSamplerBindings`, `kDrawSet`, `kDrawBinding` - with the table's own arithmetic held by `static_assert`
  (the slots fit between the material and the lights; the shadow block is the map's binding + 1, which is what the
  engine's generator states; the full-screen samplers end before the lights).
- **The backend names only the table**: `ContentCaches::create` builds every set layout from the constants,
  `FrameScratch` writes every descriptor through them, and `ContentDraw`'s two offset-array comments cite the names
  instead of repeating digits.
- **The SDK's promise is true again**: `BuiltinShaders.hpp` states the shadowed deferred program's pair at 7/8, after
  the five sampler bindings and the lights at 6 - the arrangement the generator and the layout already had.
- **The GLSL side is machine-checked, not copied**: a shader cannot include a C++ header (the shipped sources must
  compile standalone through `scripts/vn_shader_check.sh`), so `scripts/check_vk_bindings.py` (ctest `vk_bindings`)
  parses the constants from `ContentAbi.hpp` and the slot order from `MaterialValues.hpp` and compares:
  `vine_params.glsl`'s three blocks, every `vn_<slot>Map` sampler, and `BuiltinShaders.cpp`'s `shadowBindings(...)`
  call. A constant, block, slot or generator call that disappears is a finding too, so a rename cannot silently
  shrink what is covered.

Mutation: the lights moved to binding 5 in `vine_params.glsl` and the generator's map moved to 5 both redden, one
finding each; the reverse patches restored both files byte-for-byte (`git diff` empty for them).

## 3. What deliberately stays

- **The recorder's offset arrays are hand-ordered** by the layouts' binding order: Vulkan matches `pDynamicOffsets` by
  order, not by name, and BOTH sets use binding 0 - a table-driven loop would need `(set, binding)` pairs plus a switch
  to pick each slice offset, more machinery than the four-line array it would replace. The numbers it names are the
  table's; the comment cites them by name now.
- **The full-screen "binding N = attachment N" definition** stays in the ABI's prose and the sampler loop: it is the
  full-screen arrangement's own rule (`kFullscreenSamplerBindingBase + N`), not a second constant.
- **No macros injected into the GLSL**: the backend's compiler could prepend `#define`s from the table, but the shipped
  shaders would then only compile under the backend - the standalone shader gate is worth more than removing the
  numbers from the texts, and the number-in-both-places risk is exactly what the new gate answers.

## 4. Gates

- `test_vk` **245/245**, `test_graphics` 714/714.
- `ctest -R vk_bindings` 0 findings; the two mutations of §2 run and restored byte-identically.
- `ctest vk_layout_literals` / `vk_exposure` still green, and the build is warning-free.
- Documentation gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_cross_refs.py`, `check_doc_symbols.py`.

## 5. What the next round should open

- The next one-question seam is "WHAT IS THIS DEVICE'S LIMIT": after review 25's `B1`, the same numbers now travel as
  `VkPhysicalDeviceLimits` (the driver), `Capabilities::Limits` (the SDK answer) and `TargetTable::Limits` (the table's
  copy of two of them), and a fourth reader (`DynamicRendering`'s stack arrays) trusts the API cap. Read the three as
  one table and ask which unit may answer.
- `T78` still waits for its host, and `B2`'s platform branch still needs a device without `wideLines`.
