#include <vine/vsg/internal/MaterialSlotCheck.hpp>

#include <vine/imaging/PixelFormat.hpp>

VN_VSG_NS_BEGIN

namespace
{

/// @brief The slots whose maps are sampled as COLOUR (graphics-material.md 2.9): base colour and emission.
///
/// @param slot Slot index, in `vn::graphics::TextureSlot` order.
/// @return true for the two colour slots.
[[nodiscard]] bool isColorSlot(std::size_t slot) noexcept
{
    return slot == static_cast<std::size_t>(vn::graphics::TextureSlot::BaseColor) ||
           slot == static_cast<std::size_t>(vn::graphics::TextureSlot::Emissive);
}

/// @brief The slots that take a cube map: the base colour only (the other four parameterise a surface).
///
/// @param slot Slot index, in `vn::graphics::TextureSlot` order.
/// @return true for the base-colour slot.
[[nodiscard]] bool takesCubeMap(std::size_t slot) noexcept
{
    return slot == static_cast<std::size_t>(vn::graphics::TextureSlot::BaseColor);
}

/// @brief Whether @p format is a byte colour format that a COLOUR slot should not have (it should be sRGB).
///
/// @details
/// - Only the 8-bit UNORM spellings count: a float format in a colour slot is legitimate (an HDR map is linear
///   by definition), and a depth format is not a colour at all - both are "cannot judge" and stay silent.
///
/// @param format The map's pixel format.
/// @return true when the map holds linear bytes that a colour slot reads as colour.
[[nodiscard]] bool isLinearByteColor(vn::imaging::PixelFormat format) noexcept
{
    switch (format)
    {
    case vn::imaging::PixelFormat::R8Unorm:
    case vn::imaging::PixelFormat::Rg8Unorm:
    case vn::imaging::PixelFormat::Rgb8Unorm:
    case vn::imaging::PixelFormat::Rgba8Unorm:
    case vn::imaging::PixelFormat::Bgra8Unorm: return true;
    default: return false;
    }
}

}  // namespace

SlotViolation materialSlotViolation(const MaterialFacts& material, std::size_t slot) noexcept
{
    if (slot >= material.slots.size())
    {
        return SlotViolation::None;
    }
    const vn::graphics::Texture* map = material.slots[slot].texture;
    if (map == nullptr)
    {
        return SlotViolation::None;  // an absent slot is the flags' business, not a contract violation
    }
    // The KIND first: it is the one violation the content layer cannot bind at all (a cube view where the text
    // declares sampler2D is an invalid descriptor), so it is also the one a host is most likely to see.
    if (!takesCubeMap(slot) && map->kind() == vn::graphics::Texture::Kind::Cube)
    {
        return SlotViolation::CubeInDataSlot;
    }
    const vn::imaging::PixelFormat format = map->format();
    if (isColorSlot(slot) && isLinearByteColor(format))
    {
        return SlotViolation::LinearFormatInColorSlot;
    }
    if (!isColorSlot(slot) && vn::imaging::isSrgbFormat(format))
    {
        return SlotViolation::SrgbFormatInDataSlot;
    }
    return SlotViolation::None;
}

const char* materialSlotName(std::size_t slot) noexcept
{
    switch (slot)
    {
    case static_cast<std::size_t>(vn::graphics::TextureSlot::BaseColor): return "baseColorMap";
    case static_cast<std::size_t>(vn::graphics::TextureSlot::MetallicRoughness): return "metallicRoughnessMap";
    case static_cast<std::size_t>(vn::graphics::TextureSlot::Normal): return "normalMap";
    case static_cast<std::size_t>(vn::graphics::TextureSlot::Occlusion): return "occlusionMap";
    case static_cast<std::size_t>(vn::graphics::TextureSlot::Emissive): return "emissiveMap";
    default: return nullptr;
    }
}

const char* slotViolationText(SlotViolation violation) noexcept
{
    switch (violation)
    {
    case SlotViolation::None: return "";
    case SlotViolation::CubeInDataSlot:
        return "a cube map belongs in the base-colour slot only: the other four parameterise a surface with a UV "
               "pair, so the slot is bound to its declared kind's white instead";
    case SlotViolation::LinearFormatInColorSlot:
        return "the slot is sampled as colour but the map's format is linear: declare the texture with an sRGB "
               "format, or the picture keeps a gamma shift nobody can see";
    case SlotViolation::SrgbFormatInDataSlot:
        return "the slot is read as data but the map's format is sRGB: the hardware decodes it, so a roughness "
               "of 0.5 arrives as 0.21";
    }
    return "";
}

VN_VSG_NS_END
