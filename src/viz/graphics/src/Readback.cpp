#include <vine/graphics/Readback.hpp>

#include <cstring>

VN_GRAPHICS_NS_BEGIN

std::optional<std::uint32_t> colorBytesPerTexel(const vn::graphics::RenderTarget::ColorFormat format) noexcept
{
    using ColorFormat = vn::graphics::RenderTarget::ColorFormat;
    switch (format) {
    case ColorFormat::R8: return 1U;
    case ColorFormat::RG8: return 2U;
    case ColorFormat::RGBA8: return 4U;
    case ColorFormat::R16F:
    case ColorFormat::RG16F:
    case ColorFormat::R32F:
    case ColorFormat::RG32F:
    case ColorFormat::RGBA16F:
    case ColorFormat::RGBA32F:
        // No byte packing here: the colour readback copies a UNORM texel (one byte per channel), and a float
        // attachment handed back as bytes would misread every texel after the first.
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> depthBytesPerTexel(const vn::graphics::RenderTarget::DepthFormat format) noexcept
{
    switch (format) {
    case vn::graphics::RenderTarget::DepthFormat::D16: return 2U;
    case vn::graphics::RenderTarget::DepthFormat::D32:
    case vn::graphics::RenderTarget::DepthFormat::D32F: return 4U;
    case vn::graphics::RenderTarget::DepthFormat::D24: return std::nullopt; // depth AND stencil: no plain depth copy exists
    }
    return std::nullopt;
}

std::vector<float> decodeDepth(const vn::graphics::RenderTarget::DepthFormat format, const std::span<const std::byte> bytes)
{
    const std::optional<std::uint32_t> texel_width = depthBytesPerTexel(format);
    if (!texel_width.has_value() || bytes.size() % *texel_width != 0U) {
        return {};
    }
    const std::size_t texels = bytes.size() / *texel_width;

    std::vector<float> values(texels);
    if (*texel_width == 4U) {
        // D32 / D32F: the stored bits ARE the value (the copy carries the float the attachment holds).
        std::memcpy(values.data(), bytes.data(), texels * sizeof(float));
        return values;
    }
    // D16_UNORM: the stored integer is the depth scaled by its full range - the conversion the format
    // defines, not an approximation of it.
    for (std::size_t index = 0; index < texels; ++index) {
        std::uint16_t stored = 0U;
        std::memcpy(&stored, bytes.data() + index * 2U, sizeof(stored));
        values[index] = static_cast<float>(stored) / 65535.0F;
    }
    return values;
}

VN_GRAPHICS_NS_END
