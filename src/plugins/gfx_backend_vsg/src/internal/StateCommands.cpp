#include <vine/vsg/internal/StateCommands.hpp>

#include <vine/vsg/support/RenderStateMapper.hpp>

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

} // namespace

VkPrimitiveTopology mapTopology(vn::graphics::Topology topology) noexcept
{
    switch (topology) {
    case vn::graphics::Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case vn::graphics::Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case vn::graphics::Topology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case vn::graphics::Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case vn::graphics::Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case vn::graphics::Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

::vsg::ref_ptr<detail::SetDynamicState> makeDynamicStateCommand(const core::DynamicState&              state,
                                                                std::uint32_t                          color_attachments,
                                                                const detail::DynamicStateEntryPoints& entry_points,
                                                                bool                                   draws_content)
{
    auto command = detail::SetDynamicState::create();

    mapDepth(state.depth, command->depth_test_enable, command->depth_write_enable);
    // The content's compare op is distance-semantic, so the reverse-Z inversion happens at this boundary:
    // the default (Less) maps to GREATER, which is the engine convention.
    command->compare_op = detail::mapCompareOp(state.compare);

    command->cull_mode           = mapCull(state.cull_mode);
    // Clockwise, because vsg's projection inverts Y (see the file note).
    command->front_face          = VK_FRONT_FACE_CLOCKWISE;
    command->polygon_mode        = mapPolygon(state.polygon_mode);
    // The polygon offset travels as the two factors: `units` is the constant depth term and `factor` the
    // slope-scaled one (see PolygonOffset).
    command->depth_bias_constant = state.polygon_offset.units;
    command->depth_bias_slope    = state.polygon_offset.factor;
    command->topology            = mapTopology(state.topology);
    // The width the caller already clamped to the device's lineWidthRange (see ContentPass): the API rejects a
    // value above it, so this layer never has to guard.
    command->line_width          = state.line_width;

    // The ENABLE is the resolved state's, and the compiler is what makes it true for a translucent command
    // (opacity < 1, see FrameCompiler::resolveCommands): "blend disabled" therefore really means no blending,
    // the way three.js / Filament / Unity state it (an opaque material does not blend just because a fragment
    // wrote an alpha below 1). A single colour attachment of a CONTENT draw is the only shape that can blend:
    // a pass with several attachments carries DATA (a G-buffer's normal rides with the material's shininess in
    // its alpha, so blending would scale the stored normal by its own alpha - a shininess of 32 attenuates it to
    // 12.5%; the L2's G-buffer hit exactly that and writes its attachments unblended, see
    // applyOpaqueBlendForAttachments, VsgSceneRules.cpp), and a FULL-SCREEN draw is a write rather than a
    // surface (blending one made a copy of a transparent attachment disappear). The rule is re-applied at THIS
    // end because the enable and the factors are delivered by this command, so the pipeline's baked state no
    // longer decides for either path.
    const bool    can_blend = color_attachments <= 1U && draws_content;
    const bool    blending  = can_blend && state.blend.enabled;
    VkBlendFactor src       = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst       = VK_BLEND_FACTOR_ZERO;
    if (blending) {
        src = mapBlendFactor(state.blend.src);
        dst = mapBlendFactor(state.blend.dst);
    }

    command->color_attachment_count = color_attachments;
    const std::uint32_t carried     = std::min(color_attachments, detail::kMaxDynamicAttachments);
    for (std::uint32_t index = 0; index < carried; ++index) {
        VkPipelineColorBlendAttachmentState& attachment = command->blend[index];
        attachment.blendEnable                          = blending ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor                  = src;
        attachment.dstColorBlendFactor                  = dst;
        attachment.srcAlphaBlendFactor                  = src;
        attachment.dstAlphaBlendFactor                  = dst;
        attachment.colorBlendOp                         = VK_BLEND_OP_ADD;
        attachment.alphaBlendOp                         = VK_BLEND_OP_ADD;
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    command->entry_points = entry_points;
    return command;
}

::vsg::ref_ptr<::vsg::SetViewport> makeViewportCommand(const ViewportRect& rect, float depth_min, float depth_max)
{
    VkViewport viewport = {};
    viewport.x          = rect.x;
    viewport.y          = rect.y;
    viewport.width      = std::max(rect.width, 0.0F);
    viewport.height     = std::max(rect.height, 0.0F);
    // The band the PASS stated, in buffer values: reverse-Z lives in the projection, so the default {0, 1} is
    // the whole buffer and a narrower pair forces the pass' content into that layer (see DepthRange).
    viewport.minDepth   = depth_min;
    viewport.maxDepth   = depth_max;
    return ::vsg::SetViewport::create(0U, ::vsg::Viewports{ viewport });
}

::vsg::ref_ptr<::vsg::SetScissor> makeScissorCommand(const ViewportRect& rect)
{
    // An integer rectangle inside the framebuffer: the API rejects a negative origin and a zero extent is a
    // silently empty draw, so both are clamped here rather than handed over.
    VkRect2D scissor      = {};
    scissor.offset.x      = static_cast<std::int32_t>(std::lround(std::max(rect.x, 0.0F)));
    scissor.offset.y      = static_cast<std::int32_t>(std::lround(std::max(rect.y, 0.0F)));
    scissor.extent.width  = static_cast<std::uint32_t>(std::lround(std::max(rect.width, 1.0F)));
    scissor.extent.height = static_cast<std::uint32_t>(std::lround(std::max(rect.height, 1.0F)));
    return ::vsg::SetScissor::create(0U, ::vsg::Scissors{ scissor });
}

VkSampleCountFlagBits samplesToVk(std::uint32_t samples) noexcept
{
    // Rounding DOWN is the honest projection: a target cannot draw with "three samples", and a count that is not
    // an API one would otherwise have to be refused by every caller.
    if (samples >= 64U) {
        return VK_SAMPLE_COUNT_64_BIT;
    }
    if (samples >= 32U) {
        return VK_SAMPLE_COUNT_32_BIT;
    }
    if (samples >= 16U) {
        return VK_SAMPLE_COUNT_16_BIT;
    }
    if (samples >= 8U) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if (samples >= 4U) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if (samples >= 2U) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VN_VSG_NS_END
