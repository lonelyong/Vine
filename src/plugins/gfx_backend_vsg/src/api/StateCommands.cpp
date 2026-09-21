#include <vine/vsg/api/StateCommands.hpp>

#include <algorithm>
#include <array>
#include <cmath>

V_VSG_NS_BEGIN

namespace
{

/** @brief Maps a depth policy onto the API's test/write enables. */
void mapDepth(vine::graphics::DepthMode mode, VkBool32& test, VkBool32& write) noexcept
{
    // No default arm: a new DepthMode is a compile-time question here, not a silent fall-through to "no
    // depth at all".
    switch (mode) {
    case vine::graphics::DepthMode::Disabled:
        test  = VK_FALSE;
        write = VK_FALSE;
        return;
    case vine::graphics::DepthMode::TestOnly:
        test  = VK_TRUE;
        write = VK_FALSE;
        return;
    case vine::graphics::DepthMode::TestAndWrite:
        test  = VK_TRUE;
        write = VK_TRUE;
        return;
    }
    test  = VK_TRUE;
    write = VK_TRUE;
}

/** @brief Maps a cull side onto the API's mask. */
VkCullModeFlags mapCull(vine::graphics::CullMode mode) noexcept
{
    switch (mode) {
    case vine::graphics::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case vine::graphics::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    case vine::graphics::CullMode::None: return VK_CULL_MODE_NONE;
    }
    return VK_CULL_MODE_NONE;
}

/** @brief Maps a polygon mode onto the API's enum. */
VkPolygonMode mapPolygon(vine::graphics::PolygonMode mode) noexcept
{
    switch (mode) {
    case vine::graphics::PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
    case vine::graphics::PolygonMode::Line: return VK_POLYGON_MODE_LINE;
    case vine::graphics::PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

/** @brief Maps a topology onto the API's enum. */
VkPrimitiveTopology mapTopology(vine::graphics::Topology topology) noexcept
{
    switch (topology) {
    case vine::graphics::Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case vine::graphics::Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case vine::graphics::Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/** @brief Maps a blend factor onto the API's enum. */
VkBlendFactor mapBlendFactor(vine::graphics::BlendFactor factor) noexcept
{
    switch (factor) {
    case vine::graphics::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case vine::graphics::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case vine::graphics::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case vine::graphics::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case vine::graphics::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case vine::graphics::BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case vine::graphics::BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case vine::graphics::BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case vine::graphics::BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case vine::graphics::BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

}  // namespace

::vsg::ref_ptr<detail::SetDynamicState> makeDynamicStateCommand(const core::DynamicState& state,
                                                                std::uint32_t color_attachments,
                                                                const detail::DynamicStateEntryPoints& entry_points)
{
    auto command = detail::SetDynamicState::create();

    mapDepth(state.depth, command->depth_test_enable, command->depth_write_enable);
    // Reverse-Z: the projection maps the near plane to 1 and the far plane to 0, so "closer" is GREATER.
    command->compare_op = VK_COMPARE_OP_GREATER;

    command->cull_mode = mapCull(state.cull_mode);
    // Clockwise, because vsg's projection inverts Y (see the file note).
    command->front_face   = VK_FRONT_FACE_CLOCKWISE;
    command->polygon_mode = mapPolygon(state.polygon_mode);
    command->topology     = mapTopology(state.topology);

    // Blending is always on; the state selects the factors (or leaves the standard pair in place).
    VkBlendFactor src = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (state.blend.enabled) {
        src = mapBlendFactor(state.blend.src);
        dst = mapBlendFactor(state.blend.dst);
    }

    command->color_attachment_count = color_attachments;
    const std::uint32_t carried = std::min(color_attachments, detail::kMaxDynamicAttachments);
    for (std::uint32_t index = 0; index < carried; ++index) {
        VkPipelineColorBlendAttachmentState& attachment = command->blend[index];
        attachment.blendEnable         = VK_TRUE;
        attachment.srcColorBlendFactor = src;
        attachment.dstColorBlendFactor = dst;
        attachment.srcAlphaBlendFactor = src;
        attachment.dstAlphaBlendFactor = dst;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    command->entry_points = entry_points;
    return command;
}

::vsg::ref_ptr<::vsg::SetViewport> makeViewportCommand(const ViewportRect& rect)
{
    VkViewport viewport = {};
    viewport.x          = rect.x;
    viewport.y          = rect.y;
    viewport.width      = std::max(rect.width, 0.0F);
    viewport.height     = std::max(rect.height, 0.0F);
    // The depth range is the API's fixed [0, 1]: reverse-Z is in the projection.
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    return ::vsg::SetViewport::create(0U, ::vsg::Viewports{ viewport });
}

::vsg::ref_ptr<::vsg::SetScissor> makeScissorCommand(const ViewportRect& rect)
{
    // An integer rectangle inside the framebuffer: the API rejects a negative origin and a zero extent is a
    // silently empty draw, so both are clamped here rather than handed over.
    VkRect2D scissor = {};
    scissor.offset.x = static_cast<std::int32_t>(std::lround(std::max(rect.x, 0.0F)));
    scissor.offset.y = static_cast<std::int32_t>(std::lround(std::max(rect.y, 0.0F)));
    scissor.extent.width  = static_cast<std::uint32_t>(std::lround(std::max(rect.width, 1.0F)));
    scissor.extent.height = static_cast<std::uint32_t>(std::lround(std::max(rect.height, 1.0F)));
    return ::vsg::SetScissor::create(0U, ::vsg::Scissors{ scissor });
}

V_VSG_NS_END
