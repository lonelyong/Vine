#include <vine/vsg/VsgPipelineFactory.hpp>

// The definitions below are the moved bodies: their documentation and default
// arguments live on the declarations in the header.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/ClearAttachments.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/commands/Draw.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/lighting/Light.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/VertexIndexDraw.h>
#include <vsg/core/Array.h>
#include <vsg/core/Exception.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/ViewDependentState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/utils/Builder.h>
#include <vsg/app/CompileManager.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>
#include <vsg/vk/ResourceRequirements.h>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>

V_VSG_NS_BEGIN

namespace detail
{

::vsg::ref_ptr<::vsg::ShaderSet> buildShaderSet(vine::graphics::ShaderPreset preset, const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count)
{
    // Pbr / ShadowedPhong are reserved presets without a backend mapping yet
    // (Pbr needs its own PbrMaterialValue; shadow comes last in the roadmap),
    // so they fall back to the Phong shader set for now.
    ::vsg::ref_ptr<::vsg::ShaderSet> shaderSet =
        (preset == vine::graphics::ShaderPreset::FlatShaded) ? ::vsg::createFlatShadedShaderSet() : ::vsg::createPhongShaderSet();
    auto raster_state      = ::vsg::RasterizationState::create();
    raster_state->cullMode = VK_CULL_MODE_NONE; // tolerate either winding order
    auto depth_state       = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = depth_test ? VK_TRUE : VK_FALSE;
    depth_state->depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    // One colour-blend attachment per COLOUR ATTACHMENT this pass has, blending
    // off and writing every channel. The count must match the render pass'
    // colourAttachmentCount or pipeline creation fails
    // (VUID-VkGraphicsPipelineCreateInfo-renderPass-06055), so a DEPTH-ONLY pass
    // (color_count == 0) declares NONE — declaring vsg's default single
    // attachment would make the pipeline unbuildable against a depth-only
    // render pass. One code path covers 0 / 1 / N.
    ::vsg::ColorBlendState::ColorBlendAttachments blend_attachments;
    blend_attachments.reserve(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        VkPipelineColorBlendAttachmentState attachment = {};
        attachment.blendEnable         = VK_FALSE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend_attachments.push_back(attachment);
    }
    auto blend_state = ::vsg::ColorBlendState::create(blend_attachments);
    shaderSet->defaultGraphicsPipelineStates = ::vsg::GraphicsPipelineStates{
        depth_state,
        raster_state,
        blend_state,
        ::vsg::InputAssemblyState::create(),
        ::vsg::MultisampleState::create(),
        ::vsg::ViewportState::create(extent),
    };
    return shaderSet;
}

VkFormat toColorFormat(vine::graphics::RenderTarget::ColorFormat f)
{
    switch (f) {
    case vine::graphics::RenderTarget::ColorFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case vine::graphics::RenderTarget::ColorFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case vine::graphics::RenderTarget::ColorFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkFormat toDepthFormat(vine::graphics::RenderTarget::DepthFormat f)
{
    switch (f) {
    case vine::graphics::RenderTarget::DepthFormat::D16: return VK_FORMAT_D16_UNORM;
    case vine::graphics::RenderTarget::DepthFormat::D24: return VK_FORMAT_D24_UNORM_S8_UINT;
    case vine::graphics::RenderTarget::DepthFormat::D32:
    case vine::graphics::RenderTarget::DepthFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_D32_SFLOAT;
}

/**
 * @brief The two subpass dependencies every COLOUR+DEPTH variant shares.
 *
 * They are built once and reused by every variant of a pass because the Vulkan
 * spec's Render Pass Compatibility rules exempt initial/final layouts and
 * load/store ops, but NOT subpass dependencies: two render passes are only
 * compatible (and a pass may therefore swap between them at run time, keeping its
 * pipelines) when their dependencies match field for field. Reusing this helper
 * is what makes that structural — the masks are deliberately independent of the
 * load ops, since the pass lifecycle picks them later.
 *
 * @return The subpass-external dependencies (writes in, reads out).
 */
::vsg::RenderPass::Dependencies makeColorDepthDependencies()
{
    constexpr VkPipelineStageFlags k_attachments =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags k_attachment_writes =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    ::vsg::RenderPass::Dependencies dependencies;

    // External -> subpass: the previous frames' attachment writes are made
    // available before this subpass CLEARs or LOADs the attachments.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = k_attachments;
    ext_to_sub.dstStageMask             = k_attachments;
    ext_to_sub.srcAccessMask            = k_attachment_writes;
    ext_to_sub.dstAccessMask            = k_attachment_writes;
    dependencies.push_back(ext_to_sub);

    // Subpass -> external: the colour (and depth) writes become visible to a
    // later pass that samples them.
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = k_attachments;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = k_attachment_writes;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return dependencies;
}

::vsg::ref_ptr<::vsg::RenderPass> makeColorDepthRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    VkImageLayout                     depth_initial,
    bool                              promote_depth,
    bool                              color_clear)
{
    const bool has_depth = depth_format != VK_FORMAT_UNDEFINED;

    ::vsg::RenderPass::Attachments attachments;
    attachments.reserve(color_formats.size() + (has_depth ? 1u : 0u));

    ::vsg::SubpassDescription subpass = {};
    subpass.pipelineBindPoint         = VK_PIPELINE_BIND_POINT_GRAPHICS;

    uint32_t attachment_index = 0;
    for (const VkFormat color_format : color_formats) {
        ::vsg::AttachmentDescription color = {};
        color.format                       = color_format;
        color.samples                      = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp                       = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // A CLEAR may start from an undefined image; a LOAD reads what the
        // previous pass left, which this backend always ends in
        // SHADER_READ_ONLY (see the subpass-external dependency below).
        color.initialLayout = color_clear ? VK_IMAGE_LAYOUT_UNDEFINED
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        color.finalLayout                  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(color);

        ::vsg::AttachmentReference color_ref = {};
        color_ref.attachment                 = attachment_index++;
        color_ref.layout                     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        subpass.colorAttachments.emplace_back(color_ref);
    }

    if (has_depth) {
        ::vsg::AttachmentDescription depth = {};
        depth.format                       = depth_format;
        depth.samples                      = VK_SAMPLE_COUNT_1_BIT;
        // An UNDEFINED image may only be CLEARed, so the load-op follows from the
        // layout the caller names; a LOAD of an image in an unexpected layout is
        // the mismatch VUID-vkCmdDraw-None-09600 reports.
        depth.loadOp = depth_initial == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                 : VK_ATTACHMENT_LOAD_OP_LOAD;
        // Stored: when @p promote_depth the depth is left sampleable (for
        // reconstruction / SSAO); otherwise it stays a plain depth attachment so
        // ANOTHER target can borrow it for a later depth test in the same frame
        // (see RenderTarget::shareDepth).
        depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout                = depth_initial;
        depth.finalLayout = promote_depth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);

        ::vsg::AttachmentReference depth_ref = {};
        depth_ref.attachment                 = attachment_index;
        depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.emplace_back(depth_ref);
    }

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass },
                                     makeColorDepthDependencies());
}

::vsg::ref_ptr<::vsg::RenderPass> makeDepthOnlyRenderPass(::vsg::Device* device, VkFormat depth_format,
                                                          VkImageLayout depth_initial)
{
    ::vsg::AttachmentDescription depth = {};
    depth.format                       = depth_format;
    depth.samples                      = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp                       = depth_initial == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                                      : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE; // sampled later as a shadow map
    depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // A depth-only target's image always ends sampleable (that is what the pass
    // exists for), so a preserving pass names that layout and hands the image
    // back in it; a clearing pass may start from UNDEFINED.
    depth.initialLayout                = depth_initial;
    depth.finalLayout                  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ::vsg::RenderPass::Attachments attachments{ depth };

    ::vsg::AttachmentReference depth_ref = {};
    depth_ref.attachment                 = 0;
    depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    ::vsg::SubpassDescription subpass = {};
    subpass.pipelineBindPoint         = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.depthStencilAttachments.emplace_back(depth_ref);

    ::vsg::RenderPass::Dependencies dependencies;
    // UNDEFINED -> DEPTH_STENCIL_ATTACHMENT before the subpass.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstAccessMask            = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies.push_back(ext_to_sub);

    // After the subpass, transition to SHADER_READ_ONLY and make the depth
    // writes visible to a later pass that samples the shadow map.
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass }, dependencies);
}

// The body is the moved definition: its documentation lives on the declaration.

PassRenderPassPlan planPassRenderPass(bool color_clear,
                                      bool want_depth_clear,
                                      bool has_depth,
                                      bool promote_requested,
                                      bool any_load_pass,
                                      bool depth_seeded,
                                      bool borrowed_depth)
{
    PassRenderPassPlan plan;
    plan.color_load = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!has_depth) {
        return plan; // colour-only target: the depth fields stay inert
    }
    if (borrowed_depth) {
        // The source's pass defined this depth earlier in the frame and owns
        // its layout; this pass only tests against it.
        plan.depth_load    = VK_ATTACHMENT_LOAD_OP_LOAD;
        plan.depth_initial = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        plan.promote_depth = false;
        plan.seed_required = false;
        return plan;
    }
    if (want_depth_clear) {
        plan.depth_load    = VK_ATTACHMENT_LOAD_OP_CLEAR;
        plan.depth_initial = VK_IMAGE_LAYOUT_UNDEFINED;
        // Promotion is only legal while no pass of the target LOADs depth: a
        // LOAD pass must find the image in the attachment layout.
        plan.promote_depth = promote_requested && !any_load_pass;
        plan.seed_required = false;
        return plan;
    }
    // Preserving pass: LOAD the depth the previous frame / pass left. An
    // UNDEFINED image cannot be loaded, so an unseeded target needs the CLEAR
    // (seed) variant recorded once first (see PassObjects::render_pass_transient).
    plan.depth_load    = VK_ATTACHMENT_LOAD_OP_LOAD;
    plan.depth_initial = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    plan.promote_depth = false; // LOAD and promotion are mutually exclusive
    plan.seed_required = !depth_seeded;
    return plan;
}

PassVariant planPassVariant(bool has_color, bool has_depth, bool borrowed, bool promote_requested, bool any_load_pass,
                            bool depth_seeded, bool color_seeded, bool want_color_clear, bool want_depth_clear,
                            bool depth_left_promoted)
{
    // The colour bootstrap: a target whose colour image is still UNDEFINED may not
    // LOAD it, so the pass that first renders it CLEARs — for ONE frame only (a
    // transient variant), or it would wipe what its siblings drew every frame.
    const bool bootstrap_color_clear = has_color && !color_seeded;
    const bool color_clear           = has_color ? (want_color_clear || bootstrap_color_clear) : true;

    const PassRenderPassPlan plan =
        planPassRenderPass(color_clear, want_depth_clear, has_depth, promote_requested, any_load_pass, depth_seeded,
                           borrowed);

    PassVariant variant;
    variant.color_load    = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    variant.depth_load    = plan.depth_load;
    variant.promote_depth = plan.promote_depth;
    // The colour and depth regimes of a target differ: its colour attachments
    // always end sampleable, while its depth ends sampleable only for a
    // depth-only target (that is what such a target is for) or after a pass that
    // promoted it.
    variant.steady_depth_initial =
        has_color ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    variant.steady_color_load = want_color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!has_depth) {
        variant.transient = bootstrap_color_clear;
        return variant;
    }

    const bool depth_load = plan.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;
    // A preserving pass may find the depth in SHADER_READ_ONLY: an earlier frame's
    // promoting pass left it there and nothing has replaced that layout yet. It
    // then names THAT layout for one frame and hands the image back in the layout
    // its consumers expect. A depth-only target needs no such frame — its steady
    // layout IS SHADER_READ_ONLY.
    const bool needs_revoke = has_color && depth_load && depth_left_promoted;
    // The layout the RECORDED pass declares: a CLEAR starts from an undefined
    // image whatever it held before, so only a LOAD has to name the real one.
    variant.depth_initial = !depth_load         ? VK_IMAGE_LAYOUT_UNDEFINED
                            : needs_revoke      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : plan.seed_required ? VK_IMAGE_LAYOUT_UNDEFINED
                                                 : variant.steady_depth_initial;
    variant.transient     = plan.seed_required || needs_revoke || bootstrap_color_clear;
    return variant;
}

bool passVariantIsStale(bool recorded_want_color_clear, bool recorded_want_depth_clear, bool want_color_clear,
                        bool want_depth_clear)
{
    return recorded_want_color_clear != want_color_clear || recorded_want_depth_clear != want_depth_clear;
}

const std::string& fullscreenVertexSource()
{
    static const std::string source = R"(#version 450
layout(location = 0) out vec2 v_uv;
void main()
{
    v_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)";
    return source;
}

::vsg::GraphicsPipelineStates makeOverlayPipelineStates(const VkExtent2D& extent)
{
    auto raster      = ::vsg::RasterizationState::create();
    raster->cullMode = VK_CULL_MODE_NONE;
    auto depth_state = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = VK_FALSE;
    depth_state->depthWriteEnable = VK_FALSE;
    return ::vsg::GraphicsPipelineStates{
        depth_state,
        raster,
        ::vsg::ColorBlendState::create(),
        ::vsg::InputAssemblyState::create(),
        ::vsg::MultisampleState::create(),
        ::vsg::ViewportState::create(extent),
    };
}

/**
 * @brief Compiles an overlay drawable's stages into a ShaderSet ready to configure.
 *
 * Both overlay drawables (the PiP screen triangle and the user fullscreen program)
 * are the same thing to the device: a fullscreen triangle drawn with depth test and
 * write off, blending off and the overlay's own viewport, its samples bound to set 0
 * and their shader modules compiled at run time. This is the half that does not
 * depend on WHAT is sampled; the caller adds the descriptor bindings and textures.
 *
 * @param vertex_source   Vertex stage source (see fullscreenVertexSource).
 * @param fragment_source Fragment stage source.
 * @param fragment_entry  Fragment entry point name ("main" for the built-in one).
 * @param extent          Surface extent for the baked static viewport.
 * @param failure         Receives why it failed (NoCompiler / CompileFailed).
 * @return The shader set, or null when the compiler is unavailable or a stage fails.
 */
::vsg::ref_ptr<::vsg::ShaderSet> makeOverlayShaderSet(const std::string& vertex_source,
                                                     const std::string& fragment_source,
                                                     const std::string& fragment_entry, const VkExtent2D& extent,
                                                     ProgramNodeFailure* failure)
{
    auto vs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex_source);
    auto fs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, fragment_entry, fragment_source);

    auto compiler = ::vsg::ShaderCompiler::create();
    if (compiler == nullptr || !compiler->supported()) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoCompiler;
        }
        return {};
    }
    if (!compiler->compile(vs) || !compiler->compile(fs)) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::CompileFailed;
        }
        return {};
    }
    auto shaderSet    = ::vsg::ShaderSet::create();
    shaderSet->stages = ::vsg::ShaderStages{ vs, fs };
    shaderSet->defaultGraphicsPipelineStates = makeOverlayPipelineStates(extent);
    return shaderSet;
}

/**
 * @brief Wraps a configured overlay pipeline into the drawable an overlay View records.
 *
 * @param config    Configured pipeline (its textures assigned; init() runs here).
 * @param push_data Per-frame push-constant bytes, or null for a drawable that reads
 *                  none (the PiP screen triangle).
 * @return The state group holding the pipeline and the fullscreen triangle draw.
 */
::vsg::ref_ptr<::vsg::StateGroup> makeOverlayStateGroup(const ::vsg::ref_ptr<::vsg::GraphicsPipelineConfigurator>& config,
                                                       const ::vsg::ref_ptr<::vsg::Data>& push_data)
{
    config->init();
    auto state_group = ::vsg::StateGroup::create();
    config->copyTo(state_group, ::vsg::ref_ptr<::vsg::SharedObjects>());
    auto draw_commands = ::vsg::Commands::create();
    if (push_data != nullptr) {
        // The per-frame block is recorded from push_data's CURRENT bytes, so the
        // caller mutates and re-records it every frame.
        draw_commands->addChild(::vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_data.get()));
    }
    draw_commands->addChild(::vsg::Draw::create(3, 1, 0, 0));
    state_group->addChild(draw_commands);
    return state_group;
}

::vsg::ref_ptr<::vsg::Node> makeScreenTextureNode(::vsg::ref_ptr<::vsg::ImageView> image_view, const VkExtent2D& extent,
                                                  ProgramNodeFailure* failure)
{
    // The caller owns the reporting (it knows which pass asked and which sink to
    // use); this helper only says why it could not build the node.
    if (failure != nullptr) {
        *failure = ProgramNodeFailure::None;
    }
    const std::string vertex_source   = fullscreenVertexSource();
    const std::string fragment_source = R"(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D screen_tex;
void main()
{
    // vsg projects world-up to the top image row (reverse-Y perspective), and
    // the fullscreen triangle's v_uv.y == 0 sits at the top of the screen, so
    // sampling v_uv directly keeps the source upright (no Y flip).
    out_color = texture(screen_tex, v_uv);
}
)";

    auto shader_set = makeOverlayShaderSet(vertex_source, fragment_source, "main", extent, failure);
    if (shader_set == nullptr) {
        return {};
    }
    shader_set->addDescriptorBinding("screen_tex", "", 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, {});

    auto config     = ::vsg::GraphicsPipelineConfigurator::create(shader_set);
    auto sampler    = ::vsg::Sampler::create();
    auto image_info = ::vsg::ImageInfo::create(sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    config->assignTexture("screen_tex", ::vsg::ImageInfoList{ image_info });
    return makeOverlayStateGroup(config, ::vsg::ref_ptr<::vsg::Data>());
}

/**
 * @brief Collects the descriptor bindings a GLSL source DECLARES.
 *
 * The full-screen program ABI (see makeFullscreenProgramNode) gives every source
 * texture a binding index, so what a program NEEDS is knowable before anything is
 * created — but vsg 1.1.16 exposes no shader reflection, and this backend already
 * compiles the user's GLSL itself. Read the bindings from the source: every
 * `layout(...)` qualifier that mentions `binding`, with `set` defaulting to 0 (the
 * only set this pass uses). It is a scan, not a parser — the ABI's qualifier
 * grammar is small and fixed — and it fails SAFE: what it cannot classify is
 * reported to the caller as unsupported rather than silently ignored.
 *
 * @param source GLSL source of one stage.
 * @return The declared (set, binding) pairs, in source order.
 */
std::vector<std::pair<std::uint32_t, std::uint32_t>> declaredBindings(const std::string& source)
{
    // Reads "<name> = <uint>" out of a qualifier's text, or @p fallback when the
    // name is absent (i.e. the GLSL default applies).
    const auto assignment = [](const std::string& text, const char* name, std::uint32_t fallback) {
        const std::size_t at = text.find(name);
        const std::size_t eq = at == std::string::npos ? std::string::npos : text.find('=', at);
        if (eq == std::string::npos) {
            return fallback;
        }
        std::size_t digit = eq + 1;
        while (digit < text.size() && !std::isdigit(static_cast<unsigned char>(text[digit]))) {
            ++digit;
        }
        return digit < text.size() ? static_cast<std::uint32_t>(std::strtoul(text.c_str() + digit, nullptr, 10))
                                   : fallback;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> bindings;
    std::size_t                                          pos = 0;
    while ((pos = source.find("layout", pos)) != std::string::npos) {
        const std::size_t open = source.find('(', pos);
        const std::size_t close = open == std::string::npos ? std::string::npos : source.find(')', open);
        if (close == std::string::npos) {
            break;
        }
        const std::string qualifier = source.substr(open + 1, close - open - 1);
        pos                         = close + 1;
        if (qualifier.find("binding") == std::string::npos) {
            continue; // location / push_constant / ... qualifiers name no descriptor
        }
        bindings.emplace_back(assignment(qualifier, "set", 0u), assignment(qualifier, "binding", 0u));
    }
    return bindings;
}

bool programSamplesDepth(vine::raw_ptr<const vine::graphics::ShaderProgram> program, std::size_t color_count)
{
    if (program == nullptr) {
        return false;
    }
    const std::uint32_t depth_binding = static_cast<std::uint32_t>(color_count);
    for (const auto& stage : program->stages()) {
        if (stage.type != vine::graphics::ShaderStageType::Fragment) {
            continue;
        }
        for (const auto& [set, binding] : declaredBindings(stage.source.stdstr())) {
            if (set == 0u && binding == depth_binding) {
                return true;
            }
        }
    }
    return false;
}

::vsg::ref_ptr<::vsg::Node> makeFullscreenProgramNode(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const ::vsg::ImageViews&                          image_views,
    ::vsg::ref_ptr<::vsg::ImageView>                  depth_view,
    const VkExtent2D&                                 extent,
    ::vsg::ref_ptr<::vsg::Data>                       push_data,
    ProgramNodeFailure*                               failure)
{
    // The caller reports (it knows the pass and the sink); this helper only says
    // why it could not build the node.
    if (failure != nullptr) {
        *failure = ProgramNodeFailure::None;
    }
    if (program == nullptr || image_views.empty() || push_data == nullptr) {
        return ::vsg::ref_ptr<::vsg::Node>();
    }
    // Locate the user's fragment stage.
    const vine::graphics::ShaderStage* fs_spec = nullptr;
    for (const auto& stage : program->stages()) {
        if (stage.type == vine::graphics::ShaderStageType::Fragment) {
            fs_spec = &stage;
            break;
        }
    }
    if (fs_spec == nullptr) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoFragmentStage;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    const std::string vertex_source = fullscreenVertexSource();

    auto shader_set = makeOverlayShaderSet(vertex_source, fs_spec->source.stdstr(), fs_spec->entryPoint.stdstr(), extent,
                                           failure);
    if (shader_set == nullptr) {
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    // This pass provides ONE descriptor set: binding i = the source's i-th colour
    // attachment, and binding N (= the colour count) = the source's depth — but
    // only when the caller has a sampleable depth to bind. A fragment stage that
    // declares anything else would build a pipeline whose layout lacks that
    // binding and fail at DRAW time (a validation error per frame, with nothing
    // telling the host why), so refuse it here instead, before anything is built.
    const std::uint32_t provided = static_cast<std::uint32_t>(image_views.size()) + (depth_view != nullptr ? 1u : 0u);
    for (const auto& [set, binding] : declaredBindings(fs_spec->source.stdstr())) {
        if (set != 0u || binding >= provided) {
            if (failure != nullptr) {
                *failure = ProgramNodeFailure::MissingDescriptorBinding;
            }
            return ::vsg::ref_ptr<::vsg::Node>();
        }
    }

    for (std::size_t i = 0; i < image_views.size(); ++i) {
        shader_set->addDescriptorBinding("gbuffer" + std::to_string(i), "", 0, static_cast<uint32_t>(i),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ref_ptr<::vsg::Data>());
    }
    if (depth_view != nullptr) {
        shader_set->addDescriptorBinding("gbuffer_depth", "", 0, static_cast<uint32_t>(image_views.size()),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ref_ptr<::vsg::Data>());
    }
    // Per-frame light/view parameters (LightPushBlock, the full 128-byte
    // range; its size is asserted against sizeof(LightPushBlock)).
    shader_set->addPushConstantRange("pc_light", "", VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                     static_cast<uint32_t>(sizeof(LightPushBlock)));

    auto config = ::vsg::GraphicsPipelineConfigurator::create(shader_set);
    auto sampler = ::vsg::Sampler::create();
    for (std::size_t i = 0; i < image_views.size(); ++i) {
        auto image_info = ::vsg::ImageInfo::create(sampler, image_views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        config->assignTexture("gbuffer" + std::to_string(i), ::vsg::ImageInfoList{ image_info });
    }
    if (depth_view != nullptr) {
        // Depth is sampled raw (no compare): nearest filter keeps it exact.
        auto depth_sampler   = ::vsg::Sampler::create();
        depth_sampler->magFilter  = VK_FILTER_NEAREST;
        depth_sampler->minFilter  = VK_FILTER_NEAREST;
        depth_sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        auto depth_info = ::vsg::ImageInfo::create(depth_sampler, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        config->assignTexture("gbuffer_depth", ::vsg::ImageInfoList{ depth_info });
    }
    return makeOverlayStateGroup(config, push_data);
}

/**
 * @brief Builds a vsg light node from a Vine light.
 *
 * vsg lights are scene nodes collected per view into the phong "lightData"
 * uniform; colour comes in float [0,1] (vine::Colorf) and intensity is a
 * multiplier. Disabled lights produce no node.
 *
 * @param light Vine light to translate.
 * @return The vsg light node, or null for a disabled / unsupported light.
 */
::vsg::ref_ptr<::vsg::Node> buildLightNode(const vine::graphics::Light* light)
{
    if (light == nullptr || !light->isEnabled()) {
        return ::vsg::ref_ptr<::vsg::Node>();
    }
    const auto c = light->color();
    switch (light->type()) {
    case vine::graphics::LightType::Ambient:
    {
        auto ambient = ::vsg::AmbientLight::create();
        ambient->color.set(c.r, c.g, c.b);
        ambient->intensity = light->intensity();
        return ambient;
    }
    case vine::graphics::LightType::Directional:
    {
        auto dir = ::vsg::DirectionalLight::create();
        dir->color.set(c.r, c.g, c.b);
        dir->intensity = light->intensity();
        const auto v   = light->direction();
        dir->direction.set(v.x, v.y, v.z);
        // Shadow mapping is deferred until the custom-shader / multi-pass
        // slice is mature: a directional Vine light maps to a plain vsg
        // directional light for now. Light::castShadow() stays a reserved
        // semantic flag for that future slice and is not consumed here.
        return dir;
    }
    default:
        // Point/Spot are not implemented yet.
        return ::vsg::ref_ptr<::vsg::Node>();
    }
}

std::size_t setGroupLights(::vsg::Group* group, const std::vector<const vine::graphics::Light*>& lights)
{
    if (group == nullptr || lights.empty()) {
        return 0u;
    }
    // Build first, replace second: only a list that yields at least one usable
    // light node may displace the view's seeded default. A list whose every
    // entry is disabled / untranslatable means "no active light", and leaving
    // the view with no light would shade the whole pass to black (see the
    // header contract).
    std::vector<::vsg::ref_ptr<::vsg::Node>> nodes;
    nodes.reserve(lights.size());
    for (const auto* light : lights) {
        if (auto node = buildLightNode(light)) {
            nodes.push_back(std::move(node));
        }
    }
    if (nodes.empty()) {
        return 0u;
    }
    const std::size_t attached = nodes.size();
    group->children.clear();
    for (auto& node : nodes) {
        group->addChild(std::move(node));
    }
    return attached;
}

::vsg::ref_ptr<::vsg::Node> makeAmbientLight(const char* name)
{
    auto ambient  = ::vsg::AmbientLight::create();
    ambient->name = name;
    ambient->color.set(1.0f, 1.0f, 1.0f);
    ambient->intensity = 1.0f;
    return ambient;
}

} // namespace detail

V_VSG_NS_END
