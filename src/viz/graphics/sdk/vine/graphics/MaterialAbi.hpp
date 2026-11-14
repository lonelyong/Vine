#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include "MaterialBase.hpp"
#include "ShaderAbi.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Packs a material's RESOLVED values into the shading block, field for field.
 *
 * @details
 * - THE ONE PACKING RULE for `VineMaterialBlock`: both backends fill the block through this function, so a factor added
 *   to `MaterialParameters` and to the block cannot reach one backend and miss the other (the failure mode is a
 *   material that looks right on one backend and stale on the other, with nothing in a log).
 * - The values are packed VERBATIM - no clamping and no sanitising: a material is data the host authored, and the
 *   shader clamps what it draws. The block's members keep their declared defaults, which are the default material's
 *   values (see `ShaderAbi.hpp`), so content without a material looks like content with the default one.
 * - The flag row is assembled here from the two ABI facts: bits 0-4 are the texture slots that carry a map and bits
 *   8-9 the alpha mode (`kMaterialAlphaModeShift`). The slot bits are an INPUT because when a slot "carries a map" is
 *   the caller's knowledge: a backend that only binds the maps it actually uploaded must leave a failed slot's bit
 *   clear so the shader uses that slot's constant fallback.
 * - Every field is written, the reserved flag words included.
 * - This header, not `ShaderAbi.hpp`: the block's SHAPE is the ABI (there), but filling it needs the material model,
 *   and the shape header stays dependency-free (the same split as `LightAbi.hpp` / `DrawAbi.hpp`).
 *
 * @param values                 The resolved parameters (see `MaterialParameters`; an instance's values are the
 *                               parent's with its own overlaid).
 * @param present_texture_slots  Bit 0-4: the slots the caller will bind a map at (see the flag-row note above).
 * @param out                    Receives the block; every field is written.
 */
VN_GRAPHICS_API void packMaterialBlock(const MaterialParameters& values, std::uint32_t present_texture_slots, VineMaterialBlock& out) noexcept;

VN_GRAPHICS_NS_END
