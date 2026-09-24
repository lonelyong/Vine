#include <vine/vsg/api/StateCommands.hpp>

#include <algorithm>
#include <array>
#include <cmath>

VN_VSG_NS_BEGIN

namespace
{

/** @brief Maps a depth policy onto the API's test/write enables. */
void mapDepth(vn::graphics::DepthMode mode, VkBool32& test, VkBool32& write) noexcept
{
    // The switch names every DepthMode, and the tail below is what the compiler's "not all paths return"
    // warning demands rather than a policy of its own: an enumerator added without a case here lands on "depth
    // on, and writing" - not on the "no depth at all" a fall-through usually means - and the warning that goes
    // with the missing case is the signal to come back. (The earlier comment claimed there was no tail, which
    // was simply not true of this code.)
    switch (mode) {
    case vn::graphics::DepthMode::Disabled:
        test  = VK_FALSE;
        write = VK_FALSE;
        return;
    case vn::graphics::DepthMode::TestOnly:
        test  = VK_TRUE;
        write = VK_FALSE;
        return;
    case vn::graphics::DepthMode::TestAndWrite:
        test  = VK_TRUE;
        write = VK_TRUE;
        return;
    }
    test  = VK_TRUE;
    write = VK_TRUE;
}

/** @brief Maps a cull side onto the API's mask. */
VkCullModeFlags mapCull(vn::graphics::CullMode mode) noexcept
{
    switch (mode) {
    case vn::graphics::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case vn::graphics::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    case vn::graphics::CullMode::None: return VK_CULL_MODE_NONE;
    }
    return VK_CULL_MODE_NONE;
}

/** @brief Maps a polygon mode onto the API's enum. */
VkPolygonMode mapPolygon(vn::graphics::PolygonMode mode) noexcept
{
    switch (mode) {
    case vn::graphics::PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
    case vn::graphics::PolygonMode::Line: return VK_POLYGON_MODE_LINE;
    case vn::graphics::PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

/** @brief Maps a blend factor onto the API's enum. */
VkBlendFactor mapBlendFactor(vn::graphics::BlendFactor factor) noexcept
{
    switch (factor) {
    case vn::graphics::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case vn::graphics::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case vn::graphics::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case vn::graphics::BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case vn::graphics::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case vn::graphics::BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case vn::graphics::BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case vn::graphics::BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case vn::graphics::BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case vn::graphics::BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    }
    // UNREACHABLE TODAY, AND A WRONG ANSWER IF IT EVER ISN'T: this tail turns a factor this list does not
    // know into `ONE`, which is not "no blending" but a different blend than the host asked for - a wrong
    // picture with no refusal anywhere (the same shape as the old front-face default). Closing it properly
    // means validating the factor where the state is AUTHORED (the engine's BlendState) instead of guessing
    // here, which is why the guess is only documented rather than replaced: the SDK's enum has no value this
    // list is missing. A factor added there must gain a case here.
    return VK_BLEND_FACTOR_ONE;
}

}  // namespace

VkPrimitiveTopology mapTopology(vn::graphics::Topology topology) noexcept
{
    switch (topology) {
    case vn::graphics::Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case vn::graphics::Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case vn::graphics::Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

::vsg::ref_ptr<detail::SetDynamicState> makeDynamicStateCommand(const core::DynamicState& state,
                                                                std::uint32_t color_attachments,
                                                                const detail::DynamicStateEntryPoints& entry_points,
                                                                bool draws_content)
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

    // Blending is always on for a SINGLE colour attachment OF A CONTENT DRAW (the engine's per-vertex
    // opacity can drop below 1 without a pipeline rebuild, so the one-attachment picture keeps the
    // standard pair), and always OFF for a pass with several: those attachments carry DATA, and a
    // G-buffer's normal rides with the material's shininess in its alpha, so blending would scale the
    // stored normal by its own alpha (a shininess of 32 attenuates it to 12.5%). The L2's G-buffer hit
    // exactly that and writes its attachments unblended (see applyOpaqueBlendForAttachments,
    // VsgSceneRules.cpp); the rule is re-applied at THIS end because the enable and the factors are
    // delivered by this command, so the pipeline's baked state no longer decides for either path.
    //
    // A FULL-SCREEN draw is a write rather than a surface, and it never blends whatever the attachment
    // count says (see the declaration): blending one made a copy of a transparent attachment disappear.
    const bool opaque = color_attachments > 1U || !draws_content;
    VkBlendFactor src = opaque ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dst = opaque ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (!opaque && state.blend.enabled) {
        src = mapBlendFactor(state.blend.src);
        dst = mapBlendFactor(state.blend.dst);
    }

    command->color_attachment_count = color_attachments;
    const std::uint32_t carried = std::min(color_attachments, detail::kMaxDynamicAttachments);
    for (std::uint32_t index = 0; index < carried; ++index) {
        VkPipelineColorBlendAttachmentState& attachment = command->blend[index];
        attachment.blendEnable         = opaque ? VK_FALSE : VK_TRUE;
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

VN_VSG_NS_END
