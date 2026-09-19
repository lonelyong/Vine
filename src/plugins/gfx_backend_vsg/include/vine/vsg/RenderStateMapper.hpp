#pragma once
#include "vsg_global.hpp"

#include <algorithm>

#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>

#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/VsgDynamicState.hpp>

V_VSG_NS_BEGIN

/**
 * @brief The four vsg pipeline-state objects derived from one resolved state.
 *
 * Produced from a vine::graphics::ResolvedRenderState by
 * makeRenderStateObjects(); installing them replaces the corresponding
 * default states of a vsg::GraphicsPipelineConfigurator. Object construction
 * is device-free, so the mapping is unit-testable without a Vulkan device.
 */
struct RenderStateObjects
{
    ::vsg::ref_ptr<::vsg::DepthStencilState> depthStencil;   ///< Depth test/write/compare.
    ::vsg::ref_ptr<::vsg::RasterizationState> rasterization; ///< Culling + polygon mode.
    ::vsg::ref_ptr<::vsg::ColorBlendState> colorBlend;       ///< Alpha blending equation.
    ::vsg::ref_ptr<::vsg::InputAssemblyState> inputAssembly; ///< Primitive topology.
};

namespace detail
{

/**
 * @brief Maps a distance-semantic compare op onto the backend's Vulkan op.
 *
 * The Vine compare ops describe closeness ("Less = the closer fragment
 * wins"), which is projection-agnostic. The vsg backend uses a reverse-Z
 * perspective (near maps to NDC depth 1, far to 0, buffer cleared to 0), so
 * "closer" corresponds to a LARGER NDC depth and the less/greater family is
 * inverted at the Vulkan boundary. The default op (Less) therefore maps to
 * VK_COMPARE_OP_GREATER, which equals the vsg default — a scene without
 * StateNodes keeps today's exact pipeline.
 *
 * @param op Distance-semantic compare operation.
 * @return Corresponding Vulkan compare operation for a reverse-Z backend.
 */
inline VkCompareOp mapCompareOp(vine::graphics::CompareOp op)
{
    switch (op) {
        case vine::graphics::CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case vine::graphics::CompareOp::Less: return VK_COMPARE_OP_GREATER;
        case vine::graphics::CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case vine::graphics::CompareOp::LessEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case vine::graphics::CompareOp::Greater: return VK_COMPARE_OP_LESS;
        case vine::graphics::CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case vine::graphics::CompareOp::GreaterEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case vine::graphics::CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_GREATER;
}

/**
 * @brief Maps a face-culling mode to a Vulkan cull-mode bit mask.
 *
 * The mask is the SDK's meaning, and `frontFace` is what makes it that: the SDK calls a triangle front
 * when its vertices read counter-clockwise in WORLD space, and this backend declares its front faces
 * CLOCKWISE in framebuffer space because vsg's projection inverts Y (see makeRenderStateObjects). The two
 * lines are one decision — a mode mapped without the matching front face culls the wrong side — so a
 * backend that changed the projection convention has to change both.
 *
 * A scene without StateNodes keeps today's two-sided (VK_CULL_MODE_NONE) behaviour.
 *
 * @param mode Face-culling mode.
 * @return Vulkan cull-mode flag (VK_CULL_MODE_NONE when no culling).
 */
inline VkCullModeFlags mapCullMode(vine::graphics::CullMode mode)
{
    switch (mode) {
        case vine::graphics::CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case vine::graphics::CullMode::Back: return VK_CULL_MODE_BACK_BIT;
        case vine::graphics::CullMode::None: return VK_CULL_MODE_NONE;
    }
    return VK_CULL_MODE_NONE;
}

/**
 * @brief Maps a polygon rasterisation mode to a Vulkan polygon mode.
 *
 * @param mode Polygon rasterisation mode.
 * @return Corresponding Vulkan polygon mode.
 */
inline VkPolygonMode mapPolygonMode(vine::graphics::PolygonMode mode)
{
    switch (mode) {
        case vine::graphics::PolygonMode::Fill: return VK_POLYGON_MODE_FILL;
        case vine::graphics::PolygonMode::Line: return VK_POLYGON_MODE_LINE;
        case vine::graphics::PolygonMode::Point: return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

/**
 * @brief Maps a primitive topology to a Vulkan primitive topology.
 *
 * @param topology Primitive topology.
 * @return Corresponding Vulkan primitive topology.
 */
inline VkPrimitiveTopology mapTopology(vine::graphics::Topology topology)
{
    switch (topology) {
        case vine::graphics::Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case vine::graphics::Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case vine::graphics::Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/**
 * @brief Maps a blend factor to the matching Vulkan blend factor.
 *
 * @param factor Blend factor.
 * @return Corresponding Vulkan blend factor.
 */
inline VkBlendFactor mapBlendFactor(vine::graphics::BlendFactor factor)
{
    switch (factor) {
        case vine::graphics::BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case vine::graphics::BlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case vine::graphics::BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case vine::graphics::BlendFactor::OneMinusSrcAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case vine::graphics::BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case vine::graphics::BlendFactor::OneMinusDstAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case vine::graphics::BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case vine::graphics::BlendFactor::OneMinusSrcColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case vine::graphics::BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
        case vine::graphics::BlendFactor::OneMinusDstColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

}  // namespace detail

/**
 * @brief Derives the four vsg pipeline-state objects for a resolved state.
 *
 * The resolved state is backend-agnostic; this is where it becomes concrete
 * Vulkan state. Depth, culling/polygon mode, blending and topology each map
 * onto their vsg state object; the default resolved state reproduces the
 * backend's current default pipeline (reverse-Z depth with GREATER, two-sided
 * fill, standard SrcAlpha blending, triangle list), so a scene without
 * StateNodes is unaffected.
 *
 * Blending note: the backend always enables alpha blending because the
 * effective per-geometry opacity is carried by the per-vertex color alpha and
 * may drop below 1 at any time. A StateNode blend item therefore selects the
 * blend factors (enabled=true uses the requested src/dst; the default uses
 * SrcAlpha / OneMinusSrcAlpha); blend.enabled=false does not disable blending
 * in this backend.
 *
 * @param state Resolved render state to translate.
 * @return The four vsg pipeline-state objects.
 */
inline RenderStateObjects makeRenderStateObjects(
    const vine::graphics::ResolvedRenderState& state)
{
    RenderStateObjects out;

    // Depth: test/write map 1:1; the comparison op is inverted for the
    // backend's reverse-Z convention (see detail::mapCompareOp).
    out.depthStencil = ::vsg::DepthStencilState::create();
    out.depthStencil->depthTestEnable = state.depth.test ? VK_TRUE : VK_FALSE;
    out.depthStencil->depthWriteEnable = state.depth.write ? VK_TRUE : VK_FALSE;
    out.depthStencil->depthCompareOp = detail::mapCompareOp(state.depth.compare);

    // Culling + polygon rasterisation.
    out.rasterization = ::vsg::RasterizationState::create();
    out.rasterization->cullMode = detail::mapCullMode(state.cullMode);
    // FRONT FACES ARE DECLARED CLOCKWISE, and the sign is the whole point: the SDK's rule is that a
    // triangle is front-facing when its vertices read counter-clockwise in world space (StateNode.hpp),
    // while Vulkan decides that in FRAMEBUFFER space -- and vsg's projection INVERTS Y (see
    // vsg/maths/transform.h: `perspective` negates the m[1][1] term, documented as "Y NDC coordinates
    // are inverted in Vulkan"). The inversion flips the screen-space orientation, so the SDK's front
    // faces arrive here as clockwise ones. Declaring COUNTER_CLOCKWISE -- what this line did until
    // 2026-09-19 -- made every mapped cull mode act on the WRONG faces (the demo's `cullMode = Back`
    // box drew its interior: its outward faces, wound CCW by construction, were the ones culled), with
    // no validation error and nothing to report: exactly the silent failure the SDK's contract warns
    // about. A backend that renders without the Y inversion would declare counter-clockwise here.
    out.rasterization->frontFace = VK_FRONT_FACE_CLOCKWISE;
    out.rasterization->polygonMode = detail::mapPolygonMode(state.polygonMode);

    // Blending: always-on for the per-vertex opacity path; the StateNode
    // selects the factors when it opts in. A single color attachment covers
    // the window target; multi-render-target pipelines are future work.
    out.colorBlend = ::vsg::ColorBlendState::create();
    out.colorBlend->attachments.clear();
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    VkBlendFactor src = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (state.blend.enabled) {
        src = detail::mapBlendFactor(state.blend.src);
        dst = detail::mapBlendFactor(state.blend.dst);
    }
    attachment.srcColorBlendFactor = src;
    attachment.dstColorBlendFactor = dst;
    attachment.srcAlphaBlendFactor = src;
    attachment.dstAlphaBlendFactor = dst;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    out.colorBlend->attachments.push_back(attachment);

    // Primitive topology.
    out.inputAssembly = ::vsg::InputAssemblyState::create();
    out.inputAssembly->topology = detail::mapTopology(state.topology);

    return out;
}

/**
 * @brief Installs the mapped state objects onto a pipeline configurator.
 *
 * Replaces the default depth / rasterisation / blend / input-assembly states
 * of the configurator's pipeline state list with the freshly mapped ones, so
 * the pipeline is assembled from the resolved state. The remaining default
 * states (multisampling, viewport) are left untouched.
 *
 * @param config Configurator being set up for a geometry.
 * @param states Mapped state objects (see makeRenderStateObjects()).
 */
inline void applyRenderStateObjects(::vsg::GraphicsPipelineConfigurator& config,
                                    const RenderStateObjects& states)
{
    auto& pipeline_states = config.pipelineStates;
    pipeline_states.erase(
        std::remove_if(pipeline_states.begin(), pipeline_states.end(),
                       [](const ::vsg::ref_ptr<::vsg::GraphicsPipelineState>& s) {
                           return s->cast<::vsg::DepthStencilState>() != nullptr ||
                                  s->cast<::vsg::RasterizationState>() != nullptr ||
                                  s->cast<::vsg::ColorBlendState>() != nullptr ||
                                  s->cast<::vsg::InputAssemblyState>() != nullptr;
                       }),
        pipeline_states.end());
    pipeline_states.push_back(states.depthStencil);
    pipeline_states.push_back(states.rasterization);
    pipeline_states.push_back(states.colorBlend);
    pipeline_states.push_back(states.inputAssembly);
}

/**
 * @brief Collapses the dynamically delivered items of @p mapped to what a pipeline bakes.
 *
 * Under the dynamic-state layer a pipeline must not be a function of these four states any more: two
 * drawables that differ only in depth policy, culling side, front face or topology have to be served by ONE
 * pipeline, or the layer has bought nothing (vsg content-dedups pipelines, so different create-info values
 * are different VkPipelines). So the depth, rasterisation and input-assembly objects become the constants
 * named in VsgDynamicState.hpp — the SAME values the command starts from — and the delivered ones come from
 * the command each drawable carries.
 *
 * The colour blend state and the rasterisation polygon mode are collapsed too: they are delivered as well
 * (they need the two extension-backed states — see VsgDynamicState.hpp — and this backend requires them).
 * Their attachment COUNT is kept from the mapped state, because a pipeline must declare one blend entry per
 * colour attachment of its subpass (VUID-VkGraphicsPipelineCreateInfo-renderPass-07609); the values in those
 * entries are the constants.
 *
 * @param mapped State objects mapped from a resolved state (see makeRenderStateObjects()).
 * @return The state objects to install on a configurator.
 */
[[nodiscard]] inline RenderStateObjects makePipelineStateObjects(const RenderStateObjects& mapped)
{
    RenderStateObjects baked = mapped;

    auto depth                     = ::vsg::DepthStencilState::create();
    depth->depthTestEnable         = detail::kBakedDepthTestEnable;
    depth->depthWriteEnable        = detail::kBakedDepthWriteEnable;
    depth->depthCompareOp          = detail::kBakedCompareOp;

    auto raster            = ::vsg::RasterizationState::create();
    raster->cullMode       = detail::kBakedCullMode;
    raster->frontFace      = detail::kBakedFrontFace;
    raster->polygonMode    = detail::kBakedPolygonMode;

    auto assembly    = ::vsg::InputAssemblyState::create();
    assembly->topology = detail::kBakedTopology;

    // One entry per attachment, all of them the constants: the count is what the pipeline must match, the
    // values are delivered per drawable.
    const auto            attachment_count = mapped.colorBlend->attachments.size();
    ::vsg::ColorBlendState::ColorBlendAttachments baked_blend;
    baked_blend.reserve(attachment_count);
    for (std::size_t i = 0; i < attachment_count; ++i) {
        VkPipelineColorBlendAttachmentState entry{};
        entry.blendEnable         = detail::kBakedBlendEnable;
        entry.srcColorBlendFactor = detail::kBakedBlendFactor;
        entry.dstColorBlendFactor = detail::kBakedBlendFactor;
        entry.colorBlendOp        = VK_BLEND_OP_ADD;
        entry.srcAlphaBlendFactor = detail::kBakedBlendFactor;
        entry.dstAlphaBlendFactor = detail::kBakedBlendFactor;
        entry.alphaBlendOp        = VK_BLEND_OP_ADD;
        entry.colorWriteMask      = mapped.colorBlend->attachments[i].colorWriteMask;
        baked_blend.push_back(entry);
    }
    baked.colorBlend = ::vsg::ColorBlendState::create(baked_blend);

    baked.depthStencil  = depth;
    baked.rasterization = raster;
    baked.inputAssembly = assembly;
    return baked;
}

/**
 * @brief Builds the dynamic-state command for a mapped state (see VsgDynamicState.hpp).
 *
 * Every field is read from @p states — the very objects applyRenderStateObjects() installs on the
 * configurator — which is what makes the dynamic command and the pipeline's baked state agree by
 * construction: there is one mapping from a resolved state to Vulkan values, and both consumers read its
 * output. A field the command has and this function forgets would be a state the driver takes from the
 * create-info, so the tests pin the correspondence field by field.
 *

 * @param states Mapped state objects (see makeRenderStateObjects()).
 * @return The command a variant carries ahead of its draws.
 */
[[nodiscard]] inline ::vsg::ref_ptr<detail::SetDynamicState> makeDynamicState(const RenderStateObjects& states)
{
    auto command = detail::SetDynamicState::create();

    command->depth_test_enable  = states.depthStencil->depthTestEnable;
    command->depth_write_enable = states.depthStencil->depthWriteEnable;
    command->compare_op         = states.depthStencil->depthCompareOp;

    command->cull_mode    = states.rasterization->cullMode;
    command->front_face   = states.rasterization->frontFace;
    command->polygon_mode = states.rasterization->polygonMode;

    command->topology = states.inputAssembly->topology;

    // The blend half is delivered per attachment (the API takes an array): the count is the pipeline's, and
    // the entries past what the command can carry keep the pipeline's constants.
    command->color_attachment_count = static_cast<std::uint32_t>(states.colorBlend->attachments.size());
    const auto carried = std::min<std::size_t>(states.colorBlend->attachments.size(), detail::kMaxDynamicAttachments);
    for (std::size_t i = 0; i < carried; ++i) {
        command->blend[i] = states.colorBlend->attachments[i];
    }
    return command;
}

V_VSG_NS_END
