#include <vine/graphics/MaterialAbi.hpp>

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

void packMaterialBlock(const MaterialParameters& values, std::uint32_t present_texture_slots, VineMaterialBlock& out) noexcept
{
    // Field for field from the resolved parameters; nothing is clamped or sanitised, and nothing keeps a previous
    // value: the block is data the host authored and the shader clamps what it draws.
    out.base_color = { values.base_color.r, values.base_color.g, values.base_color.b, values.base_color.a };
    out.emissive   = { values.emissive.r, values.emissive.g, values.emissive.b, values.emissive.a };
    out.params     = { values.metallic, values.roughness, values.occlusion, values.alpha_cutoff };

    // The flag row: the present texture slots (bits 0-4) and the alpha mode (bits 8-9). The other three words are
    // reserved and written as zero, so a reused block cannot carry a previous material's bits into a shader.
    out.flags = { present_texture_slots | (static_cast<std::uint32_t>(values.alpha_mode) << kMaterialAlphaModeShift), 0U, 0U, 0U };
}

VN_GRAPHICS_NS_END
