#include "VsgPipelineFactory.hpp"

// The definitions below are the moved bodies: their documentation and default
// arguments live on the declarations in the header.

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
    ::vsg::ref_ptr<::vsg::ColorBlendState> blend_state;
    if (color_count > 1) {
        // MRT: one color-blend attachment per colour attachment, blending off
        // and writing every channel (mirrors the single-attachment default).
        ::vsg::ColorBlendState::ColorBlendAttachments attachments;
        attachments.reserve(static_cast<std::size_t>(color_count));
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
            attachments.push_back(attachment);
        }
        blend_state = ::vsg::ColorBlendState::create(attachments);
    }
    else {
        blend_state = ::vsg::ColorBlendState::create();
    }
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

::vsg::ref_ptr<::vsg::RenderPass> makeSampleableRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    bool                              promote_depth)
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
        color.loadOp                       = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout                = VK_IMAGE_LAYOUT_UNDEFINED;
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
        depth.loadOp                       = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Stored: when @p promote_depth the depth is left sampleable (for
        // reconstruction / SSAO); otherwise it stays a plain depth attachment so
        // ANOTHER target can borrow it for a later depth test in the same frame
        // (see RenderTarget::shareDepth).
        depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout                = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = promote_depth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);

        ::vsg::AttachmentReference depth_ref = {};
        depth_ref.attachment                 = attachment_index;
        depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.emplace_back(depth_ref);
    }

    ::vsg::RenderPass::Dependencies dependencies;

    // Initial (UNDEFINED) -> attachment-optimal before the first subpass.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstAccessMask            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies.push_back(ext_to_sub);

    // After the subpass, transition colour + depth to SHADER_READ_ONLY and
    // make their writes visible to a later pass that samples either.
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass }, dependencies);
}

::vsg::ref_ptr<::vsg::RenderPass> makeDepthOnlyRenderPass(::vsg::Device* device, VkFormat depth_format)
{
    ::vsg::AttachmentDescription depth = {};
    depth.format                       = depth_format;
    depth.samples                      = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp                       = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE; // sampled later as a shadow map
    depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout                = VK_IMAGE_LAYOUT_UNDEFINED;
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

::vsg::ref_ptr<::vsg::RenderPass> makeDepthLoadRenderPass(
    ::vsg::Device*               device,
    const std::vector<VkFormat>& color_formats,
    VkFormat                     depth_format,
    bool                         initial_clear)
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
        color.loadOp                       = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout                = VK_IMAGE_LAYOUT_UNDEFINED;
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
        depth.loadOp = initial_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // The depth attachment stays in the depth-attachment layout so the
        // next pass can LOAD it; the colour attachments still end up sampled.
        // On the initial-clear pass the depth starts UNDEFINED (fresh image)
        // and is cleared into DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
        depth.initialLayout = initial_clear ? VK_IMAGE_LAYOUT_UNDEFINED
                                            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.finalLayout                  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);

        ::vsg::AttachmentReference depth_ref = {};
        depth_ref.attachment                 = attachment_index;
        depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.emplace_back(depth_ref);
    }

    ::vsg::RenderPass::Dependencies dependencies;

    // Initial layout -> attachment layouts before the first subpass. Colour is
    // cleared (UNDEFINED -> colour); depth is LOADED, so prior depth writes
    // (the previous frame's pass, ordered by queue submission) must be visible
    // before this pass reads them.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.srcAccessMask            = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    ext_to_sub.dstAccessMask            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies.push_back(ext_to_sub);

    // After the subpass, transition the colour attachments to SHADER_READ_ONLY
    // so a later pass can sample them (the preserved depth is not sampled).
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass }, dependencies);
}

// The body is the moved definition: its documentation lives on the declaration.

::vsg::ref_ptr<::vsg::Node> makeDepthClearCommand(const VkExtent2D& extent, float depth_clear_value)
{
    VkClearAttachment attachment       = {};
    attachment.aspectMask              = VK_IMAGE_ASPECT_DEPTH_BIT;
    attachment.colorAttachment         = 0; // ignored for a depth aspect
    attachment.clearValue.depthStencil = VkClearDepthStencilValue{ depth_clear_value, 0 };

    VkClearRect rect    = {};
    rect.rect.offset    = { 0, 0 };
    rect.rect.extent    = extent;
    rect.baseArrayLayer = 0;
    rect.layerCount     = 1;

    return ::vsg::ClearAttachments::create(::vsg::ClearAttachments::Attachments{ attachment },
                                           ::vsg::ClearAttachments::Rects{ rect });
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

    auto vs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex_source);
    auto fs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", fragment_source);

    auto compiler = ::vsg::ShaderCompiler::create();
    if (compiler == nullptr || !compiler->supported()) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoCompiler;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }
    if (!compiler->compile(vs) || !compiler->compile(fs)) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::CompileFailed;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    auto shaderSet    = ::vsg::ShaderSet::create();
    shaderSet->stages = ::vsg::ShaderStages{ vs, fs };
    shaderSet->addDescriptorBinding("screen_tex", "", 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, {});
    shaderSet->defaultGraphicsPipelineStates = makeOverlayPipelineStates(extent);

    auto config     = ::vsg::GraphicsPipelineConfigurator::create(shaderSet);
    auto sampler    = ::vsg::Sampler::create();
    auto image_info = ::vsg::ImageInfo::create(sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    config->assignTexture("screen_tex", ::vsg::ImageInfoList{ image_info });
    config->init();

    auto stateGroup = ::vsg::StateGroup::create();
    config->copyTo(stateGroup, ::vsg::ref_ptr<::vsg::SharedObjects>());
    auto draw = ::vsg::Commands::create();
    draw->addChild(::vsg::Draw::create(3, 1, 0, 0));
    stateGroup->addChild(draw);
    return stateGroup;
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

    auto vs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex_source);
    auto fs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, fs_spec->entryPoint.stdstr(), fs_spec->source.stdstr());

    auto compiler = ::vsg::ShaderCompiler::create();
    if (compiler == nullptr || !compiler->supported()) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoCompiler;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }
    if (!compiler->compile(vs) || !compiler->compile(fs)) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::CompileFailed;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    auto shaderSet    = ::vsg::ShaderSet::create();
    shaderSet->stages = ::vsg::ShaderStages{ vs, fs };
    for (std::size_t i = 0; i < image_views.size(); ++i) {
        shaderSet->addDescriptorBinding("gbuffer" + std::to_string(i), "", 0, static_cast<uint32_t>(i),
                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                        ::vsg::ref_ptr<::vsg::Data>());
    }
    if (depth_view != nullptr) {
        shaderSet->addDescriptorBinding("gbuffer_depth", "", 0, static_cast<uint32_t>(image_views.size()),
                                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                        ::vsg::ref_ptr<::vsg::Data>());
    }
    // Per-frame light/view parameters (LightPushBlock, the full 128-byte
    // range; its size is asserted against sizeof(LightPushBlock)).
    shaderSet->addPushConstantRange("pc_light", "", VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    static_cast<uint32_t>(sizeof(LightPushBlock)));
    shaderSet->defaultGraphicsPipelineStates = makeOverlayPipelineStates(extent);

    auto config  = ::vsg::GraphicsPipelineConfigurator::create(shaderSet);
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
    config->init();

    auto stateGroup = ::vsg::StateGroup::create();
    config->copyTo(stateGroup, ::vsg::ref_ptr<::vsg::SharedObjects>());
    auto drawCommands = ::vsg::Commands::create();
    // Push the per-frame block first (recorded from push_data's current bytes),
    // then draw the full-screen triangle within the same pipeline layout.
    drawCommands->addChild(::vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_data.get()));
    drawCommands->addChild(::vsg::Draw::create(3, 1, 0, 0));
    stateGroup->addChild(drawCommands);
    return stateGroup;
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

void setGroupLights(::vsg::Group* group, const std::vector<const vine::graphics::Light*>& lights)
{
    if (group == nullptr || lights.empty()) {
        return;
    }
    group->children.clear();
    for (const auto* light : lights) {
        auto node = buildLightNode(light);
        if (node != nullptr) {
            group->addChild(node);
        }
    }
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
