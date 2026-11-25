#include <vine/graphics/Readback.hpp>

#include <cstring>

VN_GRAPHICS_NS_BEGIN

std::optional<ReadbackRefusal> readbackOf(const ReadbackState& state, const ReadbackRequest& request) noexcept
{
    // 1. Does the request name something the target has? (A request that can never be served answers the
    //    same way whatever the target currently holds.)
    if (request.kind == ReadbackKind::Color) {
        if (request.attachment >= state.color_attachments) {
            return ReadbackRefusal::UnknownAttachment;
        }
        // 2. Can this backend read that format at all? (A permanent property of the target, not of this
        //    frame.)
        if (!colorReadbackOf(state.color_format).readable) {
            return ReadbackRefusal::UnreadableFormat;
        }
    }
    else {
        if (!state.depth_format.has_value()) {
            return ReadbackRefusal::UnknownAttachment;
        }
        if (!depthReadbackOf(state.depth_format.value()).readable) {
            return ReadbackRefusal::UnreadableFormat;
        }
    }

    // 3. Has a copy been handed out for recording? Reading the mapped buffer before that answers with
    //    whatever the allocation held.
    const bool captured = request.kind == ReadbackKind::Color ? state.color_captured : state.depth_captured;
    if (!captured) {
        return ReadbackRefusal::NotCaptured;
    }

    return std::nullopt; // servable: one spelling for "nothing stops this", not a None enumerator
}

namespace detail
{

std::string_view refusalName(ReadbackRefusal refusal) noexcept
{
    switch (refusal) {
    case ReadbackRefusal::UnknownAttachment: return "unknown-attachment";
    case ReadbackRefusal::UnreadableFormat: return "unreadable-format";
    case ReadbackRefusal::NotCaptured: return "not-captured";
    }
    return "unknown";
}

} // namespace detail

ReadbackStatus readbackStatusOf(ReadbackRefusal refusal) noexcept
{
    switch (refusal) {
    case ReadbackRefusal::UnknownAttachment: return ReadbackStatus::Invalid;
    case ReadbackRefusal::UnreadableFormat: return ReadbackStatus::Unsupported;
    case ReadbackRefusal::NotCaptured: return ReadbackStatus::NotReady;
    }
    return ReadbackStatus::Failed; // an unmapped case is a failure to look at, never "Ok"
}

bool ReadbackFormat::operator==(const ReadbackFormat& other) const noexcept
{
    return bytes_per_texel == other.bytes_per_texel && readable == other.readable;
}

ReadbackFormat colorReadbackOf(vn::graphics::RenderTarget::ColorFormat format) noexcept
{
    using ColorFormat = vn::graphics::RenderTarget::ColorFormat;
    switch (format) {
    case ColorFormat::R8: return ReadbackFormat{ 1U, true };
    case ColorFormat::RG8: return ReadbackFormat{ 2U, true };
    case ColorFormat::RGBA8: return ReadbackFormat{ 4U, true };
    case ColorFormat::R16F:
    case ColorFormat::RG16F:
    case ColorFormat::R32F:
    case ColorFormat::RG32F:
    case ColorFormat::RGBA16F:
    case ColorFormat::RGBA32F:
        // No byte packing here: the colour readback copies a UNORM texel (one byte per channel), and a float
        // attachment handed back as bytes would misread every texel after the first.
        return ReadbackFormat{ 0U, false };
    }
    return ReadbackFormat{ 0U, false };
}

ReadbackFormat depthReadbackOf(vn::graphics::RenderTarget::DepthFormat format) noexcept
{
    switch (format) {
    case vn::graphics::RenderTarget::DepthFormat::D16: return ReadbackFormat{ 2U, true };
    case vn::graphics::RenderTarget::DepthFormat::D32:
    case vn::graphics::RenderTarget::DepthFormat::D32F: return ReadbackFormat{ 4U, true };
    case vn::graphics::RenderTarget::DepthFormat::D24: return ReadbackFormat{ 0U, false }; // depth AND stencil: no plain depth copy exists
    }
    return ReadbackFormat{ 0U, false };
}

std::vector<float> decodeDepth(vn::graphics::RenderTarget::DepthFormat format, std::span<const std::byte> bytes)
{
    const ReadbackFormat packed = depthReadbackOf(format);
    if (!packed.readable || bytes.size() % packed.bytes_per_texel != 0U) {
        return {};
    }
    const std::size_t texels = bytes.size() / packed.bytes_per_texel;

    std::vector<float> values(texels);
    if (packed.bytes_per_texel == 4U) {
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
