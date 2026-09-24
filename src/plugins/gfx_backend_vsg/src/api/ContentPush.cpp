#include <vine/vsg/api/ContentPush.hpp>

#include <cstring>

#include <vsg/vk/vulkan.h>

#include <vine/vsg/api/ViewBlock.hpp>

VN_VSG_NS_BEGIN

namespace
{

/// @brief Size of one matrix in the ABI: both filled values are `mat4`, and anything else is a declaration
///        whose bytes are not that value's.
constexpr std::uint32_t kMatrixBytes = 64U;

/// @brief Writes one matrix column-major: element (row, column) lands at `column * 4 + row` (the spelling
///        api/ViewBlock and api/DrawBlock use, so the three cannot drift into different orders).
void writeMatrix(const vn::math::Mat4d& matrix, std::byte* out) noexcept
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            const float value = static_cast<float>(matrix(row, column));
            std::memcpy(out + (static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)) *
                                   sizeof(float),
                        &value, sizeof(float));
        }
    }
}

}  // namespace

ContentPushMember contentPushMemberOf(std::string_view member_name) noexcept
{
    // The names are the SDK's, not a choice of this file: the engine's programs write them
    // (`layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;`) and the L1
    // pair's realization is defined in terms of them (see the file note).
    if (member_name == "projection")
    {
        return ContentPushMember::Projection;
    }
    if (member_name == "modelView")
    {
        return ContentPushMember::ModelView;
    }
    return ContentPushMember::Unknown;
}

const char* contentPushMemberName(ContentPushMember member) noexcept
{
    switch (member)
    {
    case ContentPushMember::Projection: return "projection";
    case ContentPushMember::ModelView: return "modelView";
    case ContentPushMember::Unknown: break;
    }
    return "?";
}

bool canFillPushMember(const AbiPushMember& member) noexcept
{
    return contentPushMemberOf(member.name) != ContentPushMember::Unknown && member.size == kMatrixBytes;
}

std::uint32_t pushStagesOf(std::uint32_t stages) noexcept
{
    VkShaderStageFlags flags = 0;
    if ((stages & static_cast<std::uint32_t>(AbiStage::Vertex)) != 0U)
    {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((stages & static_cast<std::uint32_t>(AbiStage::Fragment)) != 0U)
    {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return static_cast<std::uint32_t>(flags);
}

bool packContentPush(const AbiPushRange& range, const core::CameraSnapshot& camera, const vn::math::Mat4d& model,
                     std::vector<std::byte>& out, std::string_view& unhandled)
{
    unhandled = {};
    if (range.size == 0U)
    {
        return false;   // a range nobody could size is not a range to write
    }
    out.assign(range.size, std::byte{ 0 });
    for (const AbiPushMember& member : range.members)
    {
        const ContentPushMember value = contentPushMemberOf(member.name);
        if (value == ContentPushMember::Unknown || member.size != kMatrixBytes ||
            member.offset > range.size || member.size > range.size - member.offset)
        {
            unhandled = member.name;
            return false;
        }
        // A snapshot with no camera leaves the bytes zero (the `present` flag is the same fact buildViewBlock
        // reads): "there is no view" is a value, and the caller that has no camera decides what to do.
        if (!camera.present)
        {
            continue;
        }
        writeMatrix(value == ContentPushMember::Projection ? foldToDeviceClip(camera.projection)
                                                           : camera.view * model,
                    out.data() + member.offset);
    }
    return true;
}

VN_VSG_NS_END
