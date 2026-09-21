#include <vine/vsg/core/Readback.hpp>

#include <cstring>

V_VSG_NS_BEGIN

namespace core
{

ReadbackResult readbackOf(const ReadbackState& state, const ReadbackRequest& request) noexcept
{
    // 1. Does the request name something the target has? (A request that can never be served answers the
    //    same way whatever the target currently holds.)
    if (request.kind == ReadbackKind::Color)
    {
        if (request.attachment >= state.color_attachments)
        {
            return ReadbackResult{ false, ReadbackRefusal::UnknownAttachment };
        }
        // 2. Can this backend read that format at all? (A permanent property of the target, not of this
        //    frame.)
        if (!colorReadbackOf(state.color_format).readable)
        {
            return ReadbackResult{ false, ReadbackRefusal::UnreadableFormat };
        }
    }
    else
    {
        if (!state.depth_format.has_value())
        {
            return ReadbackResult{ false, ReadbackRefusal::UnknownAttachment };
        }
        if (!depthReadbackOf(state.depth_format.value()).readable)
        {
            return ReadbackResult{ false, ReadbackRefusal::UnreadableFormat };
        }
    }

    // 3. Has a copy been handed out for recording? Reading the mapped buffer before that answers with
    //    whatever the allocation held.
    const bool captured = request.kind == ReadbackKind::Color ? state.color_captured : state.depth_captured;
    if (!captured)
    {
        return ReadbackResult{ false, ReadbackRefusal::NotCaptured };
    }

    return ReadbackResult{ true, ReadbackRefusal::None };
}

std::string_view refusalName(ReadbackRefusal refusal) noexcept
{
    switch (refusal)
    {
    case ReadbackRefusal::None: return "none";
    case ReadbackRefusal::UnknownAttachment: return "unknown-attachment";
    case ReadbackRefusal::UnreadableFormat: return "unreadable-format";
    case ReadbackRefusal::NotCaptured: return "not-captured";
    }
    return "none";
}

bool ReadbackFormat::operator==(const ReadbackFormat& other) const noexcept
{
    return bytes_per_texel == other.bytes_per_texel && readable == other.readable;
}

ReadbackFormat colorReadbackOf(vine::graphics::RenderTarget::ColorFormat format) noexcept
{
    switch (format)
    {
    case vine::graphics::RenderTarget::ColorFormat::RGBA8: return ReadbackFormat{ 4U, true };
    case vine::graphics::RenderTarget::ColorFormat::RGBA16F:
    case vine::graphics::RenderTarget::ColorFormat::RGBA32F:
        // No CPU packing here: the readback contract is tightly packed RGBA8, and pretending a float
        // attachment is one would misread every texel after the first.
        return ReadbackFormat{ 0U, false };
    }
    return ReadbackFormat{ 0U, false };
}

ReadbackFormat depthReadbackOf(vine::graphics::RenderTarget::DepthFormat format) noexcept
{
    switch (format)
    {
    case vine::graphics::RenderTarget::DepthFormat::D16: return ReadbackFormat{ 2U, true };
    case vine::graphics::RenderTarget::DepthFormat::D32:
    case vine::graphics::RenderTarget::DepthFormat::D32F: return ReadbackFormat{ 4U, true };
    case vine::graphics::RenderTarget::DepthFormat::D24:
        return ReadbackFormat{ 0U, false };  // depth AND stencil: no plain depth copy exists
    }
    return ReadbackFormat{ 0U, false };
}

std::vector<float> decodeDepth(vine::graphics::RenderTarget::DepthFormat format,
                               std::span<const std::byte>                 bytes)
{
    const ReadbackFormat packed = depthReadbackOf(format);
    if (!packed.readable || bytes.size() % packed.bytes_per_texel != 0U)
    {
        return {};
    }
    const std::size_t texels = bytes.size() / packed.bytes_per_texel;

    std::vector<float> values(texels);
    if (packed.bytes_per_texel == 4U)
    {
        // D32 / D32F: the stored bits ARE the value (the copy carries the float the attachment holds).
        std::memcpy(values.data(), bytes.data(), texels * sizeof(float));
        return values;
    }
    // D16_UNORM: the stored integer is the depth scaled by its full range - the conversion the format
    // defines, not an approximation of it.
    for (std::size_t index = 0; index < texels; ++index)
    {
        std::uint16_t stored = 0U;
        std::memcpy(&stored, bytes.data() + index * 2U, sizeof(stored));
        values[index] = static_cast<float>(stored) / 65535.0F;
    }
    return values;
}

}  // namespace core

V_VSG_NS_END
