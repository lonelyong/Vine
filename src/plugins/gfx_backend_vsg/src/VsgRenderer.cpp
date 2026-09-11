#include <vine/vsg/VsgRenderer.hpp>

#include "VsgUtils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <typeinfo>

#if defined(__GNUG__)
#    include <cxxabi.h>
#endif

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
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

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Returns a diagnostic description of the exception currently being
 * handled (must be called from inside a catch handler).
 *
 * std::exception-derived exceptions report their what(); any other C++ type is
 * reported by its runtime type name (demangled on GCC/Clang), so an unexpected
 * failure never collapses to a bare "unknown exception" with no way to tell an
 * environment problem (no Vulkan ICD / display / device) from a code defect.
 *
 * @return Human-readable description of the active exception.
 */
std::string describeCurrentException()
{
    try {
        throw; // re-dispatch the exception being handled
    }
    catch (const ::vsg::Exception& e) {
        // vsg::Exception is a plain struct (message + VkResult), NOT derived
        // from std::exception — without this branch it would fall through to
        // the generic catch below and its message would be lost (the historical
        // "unknown exception").
        std::string text = "vsg::Exception: " + e.message;
        if (e.result != 0) {
            text += " (VkResult " + std::to_string(e.result) + ")";
        }
        return text;
    }
    catch (const std::exception& e) {
        return std::string("std::exception: ") + e.what();
    }
    catch (...) {
#if defined(__GNUG__)
        if (const std::type_info* type = abi::__cxa_current_exception_type()) {
            int          status    = 0;
            char*        demangled = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
            std::string  name      = (demangled != nullptr) ? demangled : type->name();
            std::free(demangled);
            return "non-std exception of type '" + name + "'";
        }
#endif
        return "non-std exception (type name unavailable)";
    }
}

/**
 * @brief Builds the shader set for the given shading preset with complete
 * pipeline states.
 *
 * vsg's built-in shader sets (createPhongShaderSet / createFlatShadedShaderSet)
 * arrive without default pipeline states, so pipelines built by
 * GraphicsPipelineConfigurator would lack a ViewportState and nothing would
 * rasterize. Declare the canonical states here so every SceneBridge-built
 * geometry pipeline is complete. The baked viewport matches the window size at
 * attach; when the window drives a dynamic viewport it is overridden at record
 * time anyway. Both Phong and flat presets bind a "material" descriptor of
 * type vsg::PhongMaterialValue, so the shared Vine material path (SceneBridge
 * assigns that value) works unchanged for either.
 *
 * @param preset      Shading-model preset to build for.
 * @param extent      Window extent for the baked static viewport.
 * @param depth_test  When false, depth test/write are disabled so the geometry
 *                    always draws on top of previously rendered content (used
 *                    for HUD overlays such as the axis gizmo).
 * @param color_count Colour attachment count of the target this set renders
 *                    into. Vulkan requires the pipeline's color-blend
 *                    attachment count to equal the subpass's colour count, so
 *                    MRT targets (color_count > 1) get a matching default
 *                    ColorBlendState; single-colour targets keep one (the
 *                    default).
 * @return Configured shader set.
 */
::vsg::ref_ptr<::vsg::ShaderSet> buildShaderSet(vine::graphics::ShaderPreset preset, const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count = 1)
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

/**
 * @brief Temporary test escape hatch: when VINE_VSG_OWN_WINDOW is set, the
 * backend creates its own independent vsg window instead of binding to the
 * Qt-hosted surface.
 *
 * Used to verify rendering end-to-end independent of the Qt child-window
 * compositing path (see design notes). Remove once the on-screen path is
 * decided.
 */
bool forceOwnWindow()
{
    const char* value = std::getenv("VINE_VSG_OWN_WINDOW");
    return value != nullptr && value[0] != '\0';
}

/**
 * @brief vsg::Viewer whose pollEvents() does not pump the native message queue.
 *
 * vsg's Win32_Window::pollEvents() drains and dispatches the thread's Windows
 * message queue (PeekMessage/DispatchMessage). That is correct for a
 * standalone vsg application, but when vsg is embedded in a GUI toolkit such
 * as Qt — which owns the message loop — dispatching from inside a frame call
 * re-enters the toolkit: the dispatched message triggers a Qt event, which can
 * request another frame, which pumps again, recursing until the stack
 * overflows. Input is delivered by the host instead, so window polling is
 * disabled; only the buffered vsg events are dropped.
 */
class EmbeddedViewer : public ::vsg::Inherit<::vsg::Viewer, EmbeddedViewer> {
  public:
    /** @brief Discards stale events without polling any attached window. */
    bool pollEvents(bool discardPreviousEvents) override
    {
        if (discardPreviousEvents) {
            this->getEvents().clear();
        }
        return false;
    }
};

} // namespace

namespace
{

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
 * @brief Builds an off-screen render pass whose colour attachments end in
 * SHADER_READ_ONLY_OPTIMAL so a later pass can sample them as textures.
 *
 * vsg::createRenderPass() leaves the colour attachment in PRESENT_SRC_KHR
 * (correct for swapchain output, wrong for a texture sampled by a later
 * pass). The dependency on subpass-external fragment-shader reads makes the
 * colour writes visible to the sampling pass without an extra barrier. A
 * target with several colour attachments (MRT / G-buffer) gets one attachment
 * per entry, all sampleable on their own; fragment output @p i writes
 * attachment @p i.
 *
 * @param device       Device the render pass is created on.
 * @param color_formats Colour attachment formats, one per attachment (in
 *                      attachment order).
 * @param depth_format Depth attachment format, or VK_FORMAT_UNDEFINED for a
 *                     colour-only pass.
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeSampleableRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    bool                              promote_depth = true)
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

/**
 * @brief Builds a depth-only off-screen render pass (shadow maps).
 *
 * The depth attachment is stored and left in SHADER_READ_ONLY_OPTIMAL so a
 * later pass can sample it as a shadow map. The subpass-external fragment-read
 * dependency makes the depth writes visible to the sampling pass.
 *
 * @param device      Device the render pass is created on.
 * @param depth_format Depth attachment format.
 * @return The configured render pass.
 */
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

/**
 * @brief Builds a colour + depth render pass that PRESERVES depth (LOAD).
 *
 * Same attachment set / colour handling as makeSampleableRenderPass, but the
 * depth attachment keeps its previous contents across frames instead of being
 * cleared at pass start (loadOp = LOAD, staying in the depth-attachment
 * layout). Used when an engine clear(color, clearDepth=false) targets an
 * off-screen RenderTarget, so a pass keeps drawing against depth a previous
 * pass wrote. Because depth is never promoted to a sampled texture here, such
 * targets must not be sampled for depth (the deferred G-buffer always clears
 * depth, so it never selects this pass).
 *
 * @param device        Device the render pass is created on.
 * @param color_formats Colour attachment formats.
 * @param depth_format  Depth format (VK_FORMAT_UNDEFINED when no depth).
 * @param initial_clear When true, the depth attachment is CLEARED on this
 *                      (first) pass — transitioning a freshly created image
 *                      from UNDEFINED into DEPTH_STENCIL_ATTACHMENT_OPTIMAL —
 *                      instead of being LOADed. Use it for exactly the first
 *                      frame after (re)building a depth-LOAD target.
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeDepthLoadRenderPass(
    ::vsg::Device*               device,
    const std::vector<VkFormat>& color_formats,
    VkFormat                     depth_format,
    bool                         initial_clear = false)
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

/**
 * @brief Vertex shader source shared by the full-screen overlay passes.
 *
 * Generates the full-screen triangle from gl_VertexIndex alone, so the draws
 * need no vertex buffers or camera matrices. Both overlay builders (PiP
 * screen sampling and fullscreen user-program lighting) use this identical
 * vertex stage.
 *
 * @return The GLSL vertex source.
 */
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

/**
 * @brief Builds the default pipeline states for the full-screen overlay
 * passes.
 *
 * Both overlay draws (PiP screen sampling, fullscreen program lighting)
 * composite over already-rendered content: depth test/write stay off and the
 * rasterizer culls nothing; blending stays at the opaque default because each
 * pass replaces the sub-viewport it owns.
 *
 * @param extent Surface extent for the baked static viewport.
 * @return The default GraphicsPipelineStates.
 */
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
 * @brief Builds a state-group that draws a full-screen textured triangle
 * sampling @p image_view into the current target.
 *
 * The vertex shader generates the full-screen triangle from gl_VertexIndex
 * (no vertex buffers / camera matrices involved); the fragment shader samples
 * the passed image. Depth test/write are disabled so the textured triangle
 * composites over previously rendered content (used by the PiP screen pass).
 *
 * @param image_view Image to sample (the off-screen target's colour view).
 * @param extent     Surface extent for the baked static viewport.
 * @return The drawable state-group, or null when shader compilation failed.
 */
/**
 * @brief Why an overlay/program node could not be built.
 *
 * Returned to the caller instead of reported here: only the calling member
 * knows which pass asked, and it owns the sink and the diagnostic counters.
 */
enum class ProgramNodeFailure
{
    None,           ///< Built successfully.
    NoCompiler,     ///< The runtime GLSL compiler is unavailable (no shaderc).
    NoFragmentStage,///< The user program carries no fragment stage.
    CompileFailed,  ///< GLSL compilation failed.
};

::vsg::ref_ptr<::vsg::Node> makeScreenTextureNode(::vsg::ref_ptr<::vsg::ImageView> image_view, const VkExtent2D& extent,
                                                  ProgramNodeFailure* failure = nullptr)
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

/**
 * @brief CPU mirror of the deferred-lighting push-constant block.
 *
 * All members are vec4-sized (std140/std430 offsets coincide). 8 x 16 = 128
 * bytes == the declared push range: ambient + projection params + up to three
 * directional lights (dir/colour pairs). Layout matches the fragment shader
 * ABI the fullscreen program pass expects (see makeFullscreenProgramNode).
 */
struct alignas(16) LightPushBlock
{
    std::array<float, 4> ambient{};   // ambient rgb + intensity
    // Perspective projection params for view-position reconstruction from the
    // G-buffer depth: x = near, y = far, z = proj[0][0], w = proj[1][1].
    std::array<float, 4> projparms{};
    std::array<std::array<float, 4>, 3> dirs{};   // directional lights: view-space directions
    std::array<std::array<float, 4>, 3> cols{};   // directional lights: rgb + intensity
};

// The block is copied verbatim into the shader's push-constant range, so its
// size IS the shader ABI: a field added here without updating the range must
// fail the build, not silently under-push (or over-read) at run time.
static_assert(sizeof(LightPushBlock) == 128, "LightPushBlock must match the 128-byte push-constant range");
static_assert(alignof(LightPushBlock) == 16, "LightPushBlock must stay std140-aligned");

/**
 * @brief Builds a full-screen textured node running a user fragment program.
 *
 * The vertex shader generates the full-screen triangle from gl_VertexIndex
 * (no vertex buffers / camera matrices); the fragment shader is @p program's
 * fragment stage, sampling @p image_views (one per colour attachment of the
 * source MRT target, descriptor binding i = attachment i) plus, when @p
 * depth_view is set, the source's depth attachment (next binding), and reads a
 * per-frame @p push_data block (see LightPushBlock). Depth test/write are
 * disabled and blending is off: the draw overwrites the sub-viewport it owns.
 * The retained node is drawn as its own View (viewport = the PiP rectangle),
 * so it composites over previously rendered content.
 *
 * Fragment-shader ABI the user program must follow:
 *   layout(location = 0) in vec2 v_uv;
 *   layout(binding = i) uniform sampler2D <any>;   // i-th source colour attachment
 *   layout(binding = N) uniform sampler2D depth;    // source depth (N = colour count)
 *   layout(push_constant) uniform PushConstants { vec4 ... } pc;  // LightPushBlock
 *   layout(location = 0) out vec4 out_color;
 *
 * @param program    User program supplying the fragment stage (any vertex
 *                   stage is ignored; the backend provides the fullscreen VS).
 * @param image_views Source MRT colour-attachment views to sample.
 * @param depth_view  Source depth-attachment view to sample (may be null).
 * @param extent     Surface extent for the baked static viewport.
 * @param push_data  Per-frame push-constant bytes (mutated before each record).
 * @return The drawable state-group, or null when shader compilation failed.
 */
::vsg::ref_ptr<::vsg::Node> makeFullscreenProgramNode(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const ::vsg::ImageViews&                          image_views,
    ::vsg::ref_ptr<::vsg::ImageView>                  depth_view,
    const VkExtent2D&                                 extent,
    ::vsg::ref_ptr<::vsg::Data>                       push_data,
    ProgramNodeFailure*                               failure = nullptr)
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

/**
 * @brief Replaces a view's light-group children with the given Vine lights.
 *
 * Light nodes carry no GPU resources (they are collected into the per-view
 * lightData uniform at record time), so replacing them each frame is cheap
 * and needs no recompile. An empty list leaves the group as-is so the view
 * keeps its default light(s).
 *
 * @param group  The view's light group.
 * @param lights Vine lights to attach.
 */
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

/**
 * @brief Detaches a child node from a vsg group (command graph / render
 * graph).
 *
 * The renderer retires whole views (PiP / fullscreen-program slots), rebuilt
 * off-screen graphs and released targets by detaching them from the owning
 * graph. vsg groups store plain child lists, so removal is a remove-and-erase
 * sweep; a null graph or a null node is a safe no-op.
 *
 * @param graph Graph whose children are swept (may be null).
 * @param node  Child to detach (may be null).
 */
void removeGraphChild(::vsg::Group* graph, const ::vsg::ref_ptr<::vsg::Node>& node)
{
    if (graph == nullptr) {
        return;
    }
    auto& children = graph->children;
    children.erase(
        std::remove_if(children.begin(),
                       children.end(),
                       [&node](const ::vsg::ref_ptr<::vsg::Node>& child) { return child.get() == node.get(); }),
        children.end());
}

/**
 * @brief Builds the flat white ambient light used to seed non-scene content
 * slots (HUD overlays and off-screen main slots).
 *
 * Ambient-only lighting makes Phong's colour independent of surface
 * orientation, which is what keeps HUD/axis content readable from any angle.
 *
 * @param name Node name (distinguishes the HUD seed from the off-screen one).
 * @return The ambient light node.
 */
::vsg::ref_ptr<::vsg::Node> makeAmbientLight(const char* name)
{
    auto ambient  = ::vsg::AmbientLight::create();
    ambient->name = name;
    ambient->color.set(1.0f, 1.0f, 1.0f);
    ambient->intensity = 1.0f;
    return ambient;
}

/**
 * @brief Waits for all in-flight GPU work on the viewer's device.
 *
 * Every teardown path (releasing a slot / target / rebuilding an off-screen
 * graph) must wait before dropping Vulkan objects that a still-in-flight
 * command buffer may reference. A null viewer is a safe no-op.
 *
 * @param viewer Viewer whose device to wait on (may be null).
 */
void waitForIdle(::vsg::Viewer* viewer)
{
    if (viewer != nullptr) {
        viewer->deviceWaitIdle();
    }
}

/**
 * @brief Builds and compiles an overlay View for a full-screen node.
 *
 * Wraps @p content in its own View (a dedicated camera carrying the sub-rect
 * viewport and a group holding the content) and attaches the View to @p graph:
 * appended as the last child by default (a PiP screen view drawn above the
 * content), or inserted FIRST when @p front is true (the deferred-lighting
 * main view, which later HUD content must stack above). The new View is
 * compiled before its first record — its pipeline is built against the owning
 * window render pass — and on failure the half-compiled View is detached
 * again and null is returned so the caller can drop its slot.
 *
 * @param viewer   Viewer that compiles the new View.
 * @param graph    Render graph the View is attached to (may be null).
 * @param content  Full-screen drawable to wrap.
 * @param x        Viewport origin x in device pixels.
 * @param y        Viewport origin y in device pixels.
 * @param w        Viewport width in device pixels.
 * @param h        Viewport height in device pixels.
 * @param front    When true, insert the View as the graph's first child.
 * @param what     Label for the compile-failure diagnostic.
 * @return The compiled View, or null when compilation failed.
 */
::vsg::ref_ptr<::vsg::View> makeCompiledOverlayView(
    ::vsg::Viewer& viewer,
    ::vsg::Group* graph,
    ::vsg::ref_ptr<::vsg::Node> content,
    int x,
    int y,
    int w,
    int h,
    bool front,
    const char* what,
    bool*       compile_failed = nullptr)
{
    auto camera           = ::vsg::Camera::create();
    camera->viewportState = ::vsg::ViewportState::create(x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    auto view             = ::vsg::View::create(camera);
    auto group            = ::vsg::Group::create();
    group->addChild(content);
    view->addChild(group);
    if (graph != nullptr) {
        if (front) {
            graph->children.insert(graph->children.begin(), view);
        }
        else {
            graph->addChild(view);
        }
    }
    // Compile the new View (its pipeline is built against the window render
    // pass) before it is first recorded.
    const auto compileResult = viewer.compile();
    if (!compileResult) {
        // The caller reports this (it knows the pass and the host sink): all
        // this helper does is drop the half-compiled View so it is never
        // recorded, and say that the compile was the reason.
        if (compile_failed != nullptr) {
            *compile_failed = true;
        }
        removeGraphChild(graph, view);
        return ::vsg::ref_ptr<::vsg::View>();
    }
    return view;
}

} // namespace

/** @brief Renderer state that outlives any window session.
 *
 * These objects are created once with the renderer and stay valid for its
 * lifetime (MaterialManager contract). Each window session — everything that
 * references a vsg::Window / vsg::Device — lives in @ref Impl and is replaced
 * wholesale on shutdown()/initialize(), so a session-scoped resource can never
 * be forgotten in a manual teardown list.
 */
struct VsgRenderer::Persistent {
    CameraBridge                        cameraBridge;
    VsgMaterialManager                  materialManager;
    vine::graphics::ShaderPreset        shader_preset{ vine::graphics::ShaderPreset::StandardPhong };
    void*                               bound_handle = nullptr;
};

struct VsgRenderer::Impl {
    ::vsg::ref_ptr<::vsg::Window>       window;
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;
    ::vsg::ref_ptr<::vsg::CommandGraph> command_graph;
    // Window-target shader sets shared by its content slots' bridges: one per
    // DepthMode (TestAndWrite / TestOnly / Disabled) so each slot bakes the
    // right depth test/write state. Per-geometry pipelines are compiled per
    // view (vsg compiles per viewID), so every content slot carries its own
    // SceneBridge; off-screen targets bake their own per-size sets (see Target).
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_on_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_testonly_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_off_shader_set;
    // Set by clear() (a pass clears, clearEnabled) and consumed by the next
    // render(): marks the full-target "presenting" pass (content that fills
    // the target and seeds the default headlight on the window). It does NOT
    // decide the depth style — that comes from pending_depth_mode.
    bool                                pending_presenting = false;
    // Depth handling queued by setDepthMode(), consumed by the next render().
    // Explicit per pass; opaque (TestAndWrite) by default.
    vine::graphics::DepthMode           pending_depth_mode = vine::graphics::DepthMode::TestAndWrite;
    bool                                initialized = false;

    /** @brief Identifies one content slot.
     *
     * Slots are keyed by SlotKey: the pass announced in beginPass() owns its
     * slot, so the retained state follows the pass (its camera / render target
     * may change without orphaning it) and two passes never alias. A direct
     * driver that skips the pass protocol falls back to the historical
     * (camera, explicit pass order) identity.
     */    struct ContentSlot {
        int                           order  = 0;   // explicit pipeline order (stacking)
        vine::graphics::DepthMode     depth_mode = vine::graphics::DepthMode::TestAndWrite;
        bool                          presenting = false; // this slot cleared the target (full-target main pass)
        bool                          headlight_seed = false; // its default light is the headlight (presenting window slot)
        ::vsg::ref_ptr<::vsg::Camera> vsg_camera;
        ::vsg::ref_ptr<::vsg::Group>  root;        // retained content root
        ::vsg::ref_ptr<::vsg::Group>  light_group; // lights under this slot's view
        ::vsg::ref_ptr<::vsg::View>   view;
        SceneBridge                   bridge;      // per-view pipelines (vsg compiles per viewID)
        // D22: true once this slot's (window/framebuffer render pass + view)
        // context has been registered into the viewer's CompileManager pool
        // (incrementalCompileViews()). Each slot is registered once, so the
        // pool gains exactly one context per slot View.
        bool                          compile_context_registered = false;
        // True while this slot's view is DETACHED from its target's graph
        // because the pass did not execute in the last submitted frame (see
        // retireInactivePassSlots): the retained data / pipelines are kept, so
        // re-enabling the pass simply re-attaches the view instead of
        // re-uploading the mesh and recompiling.
        bool                          detached = false;
        bool                          ready = false;
    };

    // ---- Pass scope (RenderBackend::beginPass / endPass) ----
    // The pass the engine announced for the call sequence in progress: the
    // identity of every slot this backend retains for it.
    const vine::graphics::RenderPass* current_pass = nullptr;
    // Passes announced since the last submitted frame (see
    // retireInactivePassSlots): a pass that did not execute this frame is
    // retired (its view detached) rather than left drawing stale content.
    std::set<const vine::graphics::RenderPass*> passes_active_this_frame;
    // STICKY: set once any pass is announced, i.e. this backend is being driven
    // through the engine's pass protocol. It is never cleared, so that a frame
    // in which EVERY pass is disabled (nothing announced) still retires the
    // retained views instead of leaving them on screen. A direct driver that
    // never calls beginPass keeps the legacy keying and is never retired.
    bool pass_protocol_used = false;

    // Sub-viewport queued by setViewport(), consumed by the next draw call.
    std::optional<vine::graphics::Viewport> pending_viewport;
    // Lights queued by setLights(), consumed by the next render().
    std::vector<const vine::graphics::Light*> pending_lights;
    // Explicit pipeline order queued by setPassOrder(), consumed by the next
    // draw call: the stacking position of that pass' slot (setupContentSlot).
    int pending_pass_order = 0;
    // Successful off-screen target builds (diagnostic; see
    // VsgRenderer::offscreenBuildCount()).
    std::size_t offscreen_build_count = 0;

    // Content-slot VIEWs that gained new/rebuild subtrees this frame. D22
    // incremental compile: submitFrame() recompiles ONLY these views (not the
    // whole scene). The view (not a detached subtree) is the compile unit
    // because vsg assigns the per-View viewID only while traversing the View
    // node — compiling a detached subtree always uses viewID 0 and crashes at
    // record for any other slot's viewID.
    std::vector<::vsg::ref_ptr<::vsg::View>> pending_compile_views;

    // ---- Output targets: the window (nullptr key) + off-screen (RT* key) ----

    /// Target queued by setRenderTarget(), consumed by the next render().
    vine::graphics::RenderTarget* active_target = nullptr;


    /** @brief One picture-in-picture view sampling another target's colour
     * attachment.
     *
     * Owned by the pass that draws it (see SlotKey); the sampled source and
     * attachment are slot ATTRIBUTES compared each frame, so a pass that
     * switches its input or destination is rebuilt instead of silently
     * sampling the old texture. */
    struct ScreenSlot {
        int                              order      = std::numeric_limits<int>::max(); // stacking order (engine pass order); PiP last by default
        const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
        int                              attachment = 0;             // sampled colour attachment
        ::vsg::ref_ptr<::vsg::Camera>    camera;      // carries the sub-rect viewport
        ::vsg::ref_ptr<::vsg::View>      view;        // extra View of this target's render graph
        ::vsg::ref_ptr<::vsg::ImageView> source_view; // keeps the sampled attachment alive
        int                              source_w = 0;
        int                              source_h = 0;
        int                              dest_w   = 0; // destination surface the node was built for
        int                              dest_h   = 0;
        // See ContentSlot::detached: a retired slot keeps its node / pipeline
        // so re-enabling the pass re-attaches instead of rebuilding.
        bool                             detached = false;
        bool                             ready    = false;
    };

    /** @brief One retained fullscreen-program view sampling another target's
     * colour attachments through a user fragment program (deferred lighting).
     *
     * Owned by the pass that draws it (see SlotKey); rebuilt when the sampled
     * source, its size, the destination size or the program changes. The
     * per-frame push block (view-space lights, see LightPushBlock) is written
     * into @p push_data before each record.
     */
    struct ProgramSlot {
        int                              order      = std::numeric_limits<int>::min(); // stacking order (engine pass order); fullscreen first by default
        const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
        ::vsg::ref_ptr<::vsg::Camera>    camera;     // carries the sub-rect viewport
        ::vsg::ref_ptr<::vsg::View>      view;       // extra View of this target's render graph
        ::vsg::ref_ptr<::vsg::Node>      node;       // the fullscreen program drawable
        ::vsg::ref_ptr<::vsg::Data>      push_data;  // per-frame push-constant bytes
        vine::graphics::ShaderProgram*   program = nullptr; // program the node was built with
        int                              source_w = 0;
        int                              source_h = 0;
        int                              dest_w   = 0; // destination surface the node was built for
        int                              dest_h   = 0;
        // See ContentSlot::detached.
        bool                             detached = false;
        bool                             ready    = false;
    };

    /** @brief One output target (window = nullptr key, off-screen = RT* key).
     *
     * Unified (C6.4 / C6.5): window and off-screen targets are the SAME
     * shape — a RenderGraph whose children are content-slot Views (per
     * (camera, pass order)) plus optional PiP views (screen_slots). The
     * window target's graph is the shared swapchain graph created in
     * initialize(); each off-screen target owns its
     * own graph + attachments (images / views / render pass / framebuffer)
     * and lazily builds per-size shader sets, so one RT can bake several
     * content slots (different program / content / depth policy) the same
     * way the window does.
     */
    struct Target {
        // ---- off-screen GPU attachments (window target: unused) ----
        // One image + view per colour attachment (MRT / G-buffer targets carry
        // several sampleable textures; single-colour targets keep one entry).
        std::vector<::vsg::ref_ptr<::vsg::Image>>     color_images;
        std::vector<::vsg::ref_ptr<::vsg::ImageView>> color_views;
        ::vsg::ref_ptr<::vsg::Image>       depth_image;
        ::vsg::ref_ptr<::vsg::ImageView>   depth_view;
        ::vsg::ref_ptr<::vsg::RenderPass>  render_pass;
        ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;
        ::vsg::ref_ptr<::vsg::RenderGraph> graph; // off-screen: owned here; window: the shared swapchain graph
        // Depth sharing (see RenderTarget::shareDepth): the target whose depth
        // this framebuffer borrows (null = owns its depth) plus the command
        // barrier that makes that depth visible between the two render graphs.
        vine::graphics::RenderTarget* depth_source = nullptr;
        ::vsg::ref_ptr<::vsg::Node>   depth_share_barrier;
        // Set when the borrowed source above was RELEASED while still borrowed:
        // its VkImage is gone, so the borrow cannot be honoured and this target
        // builds with its own depth instead (a later shareDepth() with a live
        // source clears the condition by being a different pointer).
        const vine::graphics::RenderTarget* unusable_depth_source = nullptr;
        // Per-size shader sets for off-screen slots (window slots share
        // impl->depth_on / depth_testonly / depth_off shader sets). Built lazily.
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_on_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_testonly_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_off_shader_set;
        int width  = 0; // off-screen logical size
        int height = 0;
        // Engine clear() request for this target, persisted so an off-screen
        // graph (re)built later reapplies the last requested clear values
        // instead of a hard-coded default. clear_depth also selects the depth
        // policy of the off-screen pass (CLEAR vs depth-LOAD) at (re)build.
        bool        clear_seen  = false;
        ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
        bool        clear_depth = true;
        bool        depth_load  = false; // off-screen pass LOADs (preserves) depth
        // Depth-LOAD targets keep two compatible render passes over the same
        // attachments: the first-frame pass CLEARs the freshly created
        // (UNDEFINED) depth image so its layout becomes
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL, and the steady pass LOADs it every
        // later frame. depth_ready tracks that one-time initialisation.
        ::vsg::ref_ptr<::vsg::RenderPass> render_pass_load;
        bool        depth_ready = false;
        // ---- content slots (retained Views under graph), keyed by owning pass ----
        std::map<SlotKey, ContentSlot> content_slots;
        // ---- PiP views sampling other targets (drawn under this graph) ----
        std::map<SlotKey, ScreenSlot> screen_slots;
        // ---- fullscreen-program views (deferred lighting), keyed by owning pass ----
        std::map<SlotKey, ProgramSlot> program_slots;
    };
    std::map<vine::graphics::RenderTarget*, Target> targets; // nullptr key == window
};

VsgRenderer::VsgRenderer()
  : impl(new Impl()),
    persistent(new Persistent())
{
}

VsgRenderer::~VsgRenderer()
{
    shutdown();
}

bool VsgRenderer::initialize()
{
    // The unified output-target table keys the window (backbuffer) by a null
    // RenderTarget* — the same identity the graphics engine uses for the
    // on-screen target. The window entry is created below when its shared
    // swapchain render graph is assigned.
    // Defensive: tear down any still-live previous session (a caller that
    // skipped shutdown()) so this re-init starts from a clean session.
    // shutdown() nulls bound_handle, so restore the just-bound handle.
    if (impl->window != nullptr) {
        void* bound = persistent->bound_handle;
        shutdown();
        persistent->bound_handle = bound;
    }
    // The stage label is printed if any step below throws, so a failing init
    // reports exactly where it died (window/device/swapchain creation, shader
    // sets, viewer compile) instead of a bare "unknown exception".
    const char* init_stage = "creating Vulkan window (instance/device/swapchain)";
    try {
    // Window. When a host native window is bound, attach to its surface (e.g.
    // a Qt QWindow) instead of creating a separate window.
    auto traits         = ::vsg::WindowTraits::create();
    traits->windowTitle = "Vine";
    traits->width       = 1280;
    traits->height      = 720;
    // Debug switch VINE_VSG_DEBUG_LAYER turns on the Vulkan validation layer so
    // silent pipeline/render-pass failures (no validation in normal runs)
    // surface as messages.
    traits->debugLayer = std::getenv("VINE_VSG_DEBUG_LAYER") != nullptr;
    // The SDK render-state model maps to pipeline features that are optional in
    // Vulkan: PolygonMode::Line needs fillModeNonSolid, and MRT pipelines with
    // differing attachments need independentBlend. Both are near-universal core
    // features; request them so pipelines honour the mapped state instead of
    // tripping validation / pipeline creation on capable devices.
    traits->deviceFeatures = ::vsg::DeviceFeatures::create();
    traits->deviceFeatures->get().fillModeNonSolid = VK_TRUE;
    traits->deviceFeatures->get().independentBlend = VK_TRUE;

    void* host_handle = persistent->bound_handle;
    if (forceOwnWindow()) {
        // Temporary test path: create vsg's own window, ignoring the Qt-hosted
        // surface handle, to verify rendering independent of Qt compositing.
        host_handle = nullptr;
    }
    if (host_handle != nullptr) {
#ifdef _WIN32
        traits->nativeWindow = reinterpret_cast<HWND>(host_handle);
        RECT client_rect{};
        if (::GetClientRect(reinterpret_cast<HWND>(host_handle), &client_rect) && client_rect.right > client_rect.left && client_rect.bottom > client_rect.top)
        {
            traits->width  = client_rect.right - client_rect.left;
            traits->height = client_rect.bottom - client_rect.top;
        }
#else
        // vsg's Xcb backend reads the native window as an xcb_window_t
        // (uint32_t). The host handle carries QWindow::winId() bits, so
        // narrow it to exactly that type; std::any only matches on the
        // exact type, and storing a void*/64-bit handle makes vsg throw
        // bad_any_cast when it casts back to xcb_window_t.
        traits->nativeWindow = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(host_handle));
#endif
    }
    impl->window = ::vsg::Window::create(traits);
    if (impl->window == nullptr) {
        std::fprintf(stderr,
                     "[VsgRenderer] Window::create FAILED (nativeWindow=%d, %ux%u)\n",
                     traits->nativeWindow.has_value() ? 1 : 0,
                     traits->width,
                     traits->height);
        shutdown();
        return false;
    }

    // Window-target shader sets shared by its content slots (embedded SPIR-V,
    // no runtime glslang): the depth-on set keeps depth test/write on; the
    // depth-off set disables it so the slot's content always draws on top of
    // earlier content (HUD). Off-screen targets bake their own per-size sets
    // lazily.
    init_stage = "building window shader sets";
    impl->depth_on_shader_set        = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), true, true);
    impl->depth_testonly_shader_set  = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), true, false);
    impl->depth_off_shader_set       = buildShaderSet(persistent->shader_preset, impl->window->extent2D(), false, false);

    // The primary window layer is created lazily on the first window render
    // (the first pass that clears and draws the scene into the backbuffer).
    // The engine owns the pipeline and drives content per pass, so the
    // renderer binds neither a Vine scene nor a camera and pre-creates
    // nothing here.

    // Viewer. EmbeddedViewer disables vsg's native message pumping (Qt owns
    // the message loop here). Content slots are appended to the render graph
    // later (see setupContentSlot) as extra Views — the canonical vsg
    // multi-viewport pattern: one render pass, later Views drawn on top.
    init_stage = "creating viewer / command graph";
    impl->viewer = ::vsg::ref_ptr<::vsg::Viewer>(new EmbeddedViewer());
    impl->viewer->addWindow(impl->window);

    // Window render graph (empty until the first content slot is created) +
    // command graph. The window target's graph IS this shared swapchain graph
    // (targets[nullptr].graph); every window content slot / PiP view is a
    // child of it.
    auto renderGraph      = ::vsg::RenderGraph::create(impl->window);
    renderGraph->contents = VK_SUBPASS_CONTENTS_INLINE;
    impl->targets[nullptr].graph = renderGraph;
    auto commandGraph     = ::vsg::CommandGraph::create(impl->window);
    commandGraph->addChild(renderGraph);
    impl->command_graph = commandGraph;
    impl->viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ commandGraph });

    init_stage = "initial viewer compile";
    const auto compileResult = impl->viewer->compile();
    if (!compileResult) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                      formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                       compileResult.message.c_str()));
        shutdown();
        return false;
    }

    impl->initialized = true;
    return true;
    }
    catch (...) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::InitFailed,
                      formatDiagnostic(u8"initialize FAILED at '%s': %s", init_stage,
                                       describeCurrentException().c_str()));
        // Environment hints: an init failure here is usually a missing/invalid
        // Vulkan ICD (VK_ICD_FILENAMES), a device/driver problem or a missing
        // display — print what the backend saw so a local environment issue is
        // not mistaken for a code defect.
        std::fprintf(stderr,
                     "[VsgRenderer]   init env: VK_ICD_FILENAMES=%s  VINE_VSG_DEBUG_LAYER=%s  DISPLAY=%s  WAYLAND_DISPLAY=%s\n",
                     std::getenv("VK_ICD_FILENAMES") ? std::getenv("VK_ICD_FILENAMES") : "(unset)",
                     std::getenv("VINE_VSG_DEBUG_LAYER") ? std::getenv("VINE_VSG_DEBUG_LAYER") : "(unset)",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)",
                     std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "(unset)");
    }
    shutdown();
    return false;
}

void VsgRenderer::shutdown()
{
    if (impl->viewer != nullptr) {
        impl->viewer->deviceWaitIdle();
        // Detach the window from the viewer so its command graphs are dropped
        // before the viewer is released.
        if (impl->window != nullptr) {
            impl->viewer->removeWindow(impl->window);
        }
        impl->viewer->close();
    }
    if (impl->window != nullptr) {
        // Release the native handle the platform window wraps. When the
        // reference is dropped, the Win32_Window destructor would call
        // ::DestroyWindow() (and ::UnregisterClass()) on the HOST's window —
        // here a Qt-owned HWND that Qt is itself tearing down. releaseWindow()
        // nulls the internal HWND so the destructor leaves Qt's window alone.
        impl->window->releaseWindow();
    }
    // Whole-session teardown: replacing the (session) Impl drops the window,
    // viewer, command graph, per-target render graphs, content slots and every
    // compiled pipeline that references the old vsg::Device — in one step, so
    // a newly retained vsg member cannot be forgotten here. The next
    // initialize() starts from a fresh Impl and allocates device ID 0, so a
    // surface-recreate re-init never trips vsg's VSG_MAX_DEVICES limit.
    impl = std::make_unique<Impl>();
    // The material manager outlives sessions (MaterialManager contract), so it
    // is cleared explicitly to drop references its cache holds to the dead
    // device; bound_handle points at a surface that is going away.
    persistent->materialManager.clear();
    persistent->bound_handle = nullptr;
}

void VsgRenderer::beginFrame()
{
    // A new frame: the passes active this frame are re-announced by beginPass()
    // as the engine runs them, so the activity set starts empty. The
    // protocol-used marker is deliberately STICKY (not cleared here): once the
    // backend has been driven through pass scopes, a frame in which every pass
    // is disabled announces nothing and must still retire the retained views.
    impl->passes_active_this_frame.clear();
    if (impl->viewer == nullptr) {
        return;
    }
    impl->viewer->advanceToNextFrame();
    impl->viewer->handleEvents();
}

void VsgRenderer::endFrame()
{
    if (impl->viewer == nullptr) {
        return;
    }
    impl->viewer->update();
}

void VsgRenderer::setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target)
{
    // Queue the target for the next render() call (mirrors setViewport()).
    impl->active_target = target;
}

void VsgRenderer::resetPerPassState()
{
    impl->active_target      = nullptr;
    impl->pending_viewport.reset();
    impl->pending_lights.clear();
    impl->pending_depth_mode = vine::graphics::DepthMode::TestAndWrite;
    impl->pending_pass_order = 0;
    impl->pending_presenting = false;
}

void VsgRenderer::beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    // Open a clean pass scope: a scope must never inherit the previous pass'
    // pending state (the direct-driver path, which queues state without opening
    // a scope, is unaffected because beginPass is never called there).
    impl->current_pass = pass;
    resetPerPassState();
    if (pass != nullptr) {
        // The pass owns its retained slot and counts as active this frame: a
        // pass that is not announced again next frame is retired (see
        // retireInactivePassSlots), which is what makes disabling it take effect.
        impl->passes_active_this_frame.insert(pass);
        impl->pass_protocol_used = true;
    }
}

void VsgRenderer::endPass()
{
    // Close the scope: state the pass queued but no draw call consumed (a
    // target / viewport / lights / depth mode set by a pass that then drew
    // nothing) is discarded here so it can never apply to the next pass.
    impl->current_pass = nullptr;
    resetPerPassState();
}

void VsgRenderer::erasePassSlotsFromTarget(vine::graphics::RenderTarget* target,
                                           const vine::graphics::RenderPass* pass)
{
    if (pass == nullptr) {
        return;
    }
    const auto target_entry = impl->targets.find(target);
    if (target_entry == impl->targets.end()) {
        return;
    }
    auto&      t   = target_entry->second;
    const SlotKey key = SlotKey::ownerPass(pass);
    // Nothing to do for a pass this target holds no slot for: avoid a device
    // wait on the common path (a pass that moved targets usually owns a slot
    // in only one of them).
    if (t.content_slots.find(key) == t.content_slots.end() &&
        t.screen_slots.find(key) == t.screen_slots.end() &&
        t.program_slots.find(key) == t.program_slots.end()) {
        return;
    }
    // Wait BEFORE detaching / dropping anything: the views, pipelines and
    // samplers about to be destroyed may still be referenced by a submitted
    // command buffer (destroying them first trips VUID-vkDestroyPipeline /
    // vkDestroySampler).
    waitForIdle(impl->viewer.get());
    // A dropped view must not stay queued for the frame's incremental compile.
    const auto forget_view = [this](const ::vsg::ref_ptr<::vsg::View>& view) {
        auto& queue = impl->pending_compile_views;
        queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
    };

    if (const auto it = t.content_slots.find(key); it != t.content_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        forget_view(it->second.view);
        it->second.bridge.clearCache();
        t.content_slots.erase(it);
    }
    if (const auto it = t.screen_slots.find(key); it != t.screen_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        t.screen_slots.erase(it);
    }
    if (const auto it = t.program_slots.find(key); it != t.program_slots.end()) {
        removeGraphChild(t.graph.get(), it->second.view);
        t.program_slots.erase(it);
    }
}

void VsgRenderer::retargetPass(const vine::graphics::RenderPass* pass,
                               vine::graphics::RenderTarget*     target)
{
    if (pass == nullptr) {
        return;
    }
    // A pass keeps exactly one retained slot per target. When it draws into a
    // different target than before, the slot it left behind would otherwise
    // keep drawing its content there forever.
    for (auto& entry : impl->targets) {
        if (entry.first == target) {
            continue;
        }
        erasePassSlotsFromTarget(entry.first, pass);
    }
}

void VsgRenderer::retireInactivePassSlots()
{
    if (!impl->pass_protocol_used) {
        return; // direct driver (legacy keys): nothing is pass-owned
    }
    // A slot needs retiring when its pass did not execute this frame and its
    // view is still attached. Already-retired slots are skipped, so a pass that
    // stays disabled costs nothing per frame (no scan hit, no device wait, no
    // repeated diagnostic).
    const auto needs_retire = [this](const SlotKey& key, bool detached) {
        return !detached && key.owner != nullptr &&
               impl->passes_active_this_frame.count(key.owner) == 0;
    };
    bool any = false;    for (const auto& entry : impl->targets) {
        const auto& t = entry.second;
        for (const auto& kv : t.content_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        for (const auto& kv : t.screen_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        for (const auto& kv : t.program_slots) {
            any = any || needs_retire(kv.first, kv.second.detached);
        }
        if (any) {
            break;
        }
    }
    if (!any) {
        return;
    }
    // Wait BEFORE detaching: the pipelines the detached views hold must not be
    // destroyed while a submitted command buffer may still reference them. The
    // slots themselves are KEPT (only the view is detached) so re-enabling a
    // pass re-attaches instead of re-uploading its mesh and recompiling.
    waitForIdle(impl->viewer.get());

    for (auto& entry : impl->targets) {
        auto& t = entry.second;
        for (auto& kv : t.content_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
        for (auto& kv : t.screen_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
        for (auto& kv : t.program_slots) {
            if (!needs_retire(kv.first, kv.second.detached)) {
                continue;
            }
            removeGraphChild(t.graph.get(), kv.second.view);
            kv.second.detached = true;
        }
    }
    // Dropping a view can remove a command-graph dependency edge.
    reconcileOffscreenOrder();
    std::fprintf(stderr, "[VsgRenderer] retired (detached) the retained view of pass(es) not active this frame\n");
}

void VsgRenderer::releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const vine::graphics::RenderPass* removed = pass;
    for (auto& entry : impl->targets) {
        erasePassSlotsFromTarget(entry.first, removed);
    }
    impl->passes_active_this_frame.erase(removed);
    if (impl->current_pass == removed) {
        impl->current_pass = nullptr;
    }
    // Dropping a sampling slot can change the off-screen record order.
    reconcileOffscreenOrder();
}

void VsgRenderer::setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights)
{
    // Queue the lights for the next render() call (mirrors setViewport()): the
    // light nodes are built when the matching view is reconciled in render().
    impl->pending_lights.clear();
    impl->pending_lights.reserve(lights.size());
    for (const auto* light : lights) {
        impl->pending_lights.push_back(light);
    }
}

bool VsgRenderer::supportsRenderTargets()
{
    return true;
}

void VsgRenderer::render(const std::vector<vine::graphics::RenderCommand>& commands, vine::raw_ptr<const vine::graphics::Camera> camera)
{
    if (!impl->initialized || impl->viewer == nullptr) {
        return;
    }

    // Consume the sub-viewport queued by setViewport() just before this pass
    // (overlays); the main pass never sets one and renders the full surface.
    const std::optional<vine::graphics::Viewport> viewport = takePendingViewport();

    // Consume the lights queued by setLights() just before this pass (from the
    // content scene). Empty keeps each view's default light(s).
    std::vector<const vine::graphics::Light*> lights = std::move(impl->pending_lights);
    impl->pending_lights.clear();

    // Consume the explicit pass order queued by setPassOrder() just before
    // this pass (the engine announces each pass's addPass() order). It is the
    // content-slot key under this camera AND the stacking order (ascending) —
    // setupContentSlot keeps each target's slot views sorted by it, so stacking
    // always follows the user-set pipeline order, whatever the creation order.
    const int pass_order     = impl->pending_pass_order;
    impl->pending_pass_order = 0;

    // The content slot's depth handling is EXPLICIT: setDepthMode() (from
    // RenderPass::depthMode) decides Disabled / TestOnly / TestAndWrite. The
    // clear() marker only records that this render is the full-target
    // "presenting" pass (used to seed the default light and to keep main slots
    // filling the whole target); it does not imply a depth mode. Both are
    // consumed here for every render, so one pass's state cannot leak into a
    // later render of another target.
    const vine::graphics::DepthMode depth_mode = impl->pending_depth_mode;
    impl->pending_depth_mode = vine::graphics::DepthMode::TestAndWrite;
    const bool presenting = impl->pending_presenting;
    impl->pending_presenting    = false;

    // The active target (setRenderTarget, nullptr = the window). Every target
    // shares ONE content-slot path (renderContentSlot): only the GPU
    // attachment kind differs, and it is ensured here before the slot draws
    // (window = the shared swapchain graph from initialize(); off-screen =
    // owned attachments + graph, built / rebuilt to the target's size).
    vine::graphics::RenderTarget* target_key = impl->active_target;
    impl->active_target                      = nullptr;

    if (target_key != nullptr && (camera == nullptr || !target_key->valid() || (!target_key->hasColor() && !target_key->hasDepth()))) {
        // Off-screen target unusable (no camera, invalid, or neither colour
        // nor depth attachment): nothing to draw this pass.
        return;
    }

    // A pass owns one slot per target: if this pass rendered into a DIFFERENT
    // target before (its render target changed at run time), drop that stale
    // slot so it stops drawing there (H2).
    retargetPass(impl->current_pass, target_key);

    auto& target = impl->targets[target_key];
    // A depth-LOAD policy (clearDepth=false) needs a render pass whose depth
    // attachment is not cleared; when the target's persisted clear policy
    // differs from its current pass, the graph is rebuilt so depth is either
    // genuinely preserved (LOAD) or cleared (CLEAR) — buildOffscreenTarget
    // bakes the policy and colour into the pass. A target that BORROWS another
    // target's depth never selects the depth-LOAD pass (its depth policy comes
    // from the owner), so the rebuild predicate must not expect one there (H4).
    const bool want_load_depth = target.clear_seen && !target.clear_depth && target.depth_source == nullptr;
    if (target_key != nullptr &&
        (target.graph == nullptr || target.width != target_key->width() ||
         target.height != target_key->height() || target.depth_load != want_load_depth)) {
        // First render into this off-screen target, or it was resized, or its
        // depth-clear policy changed: build (or rebuild) its attachments +
        // render graph. Any content slots compiled against an older graph are
        // dropped by buildOffscreenTarget.
        buildOffscreenTarget(target_key);
        if (target.graph == nullptr) {
            return; // off-screen target could not be built
        }
    }

    // Render into the content slot this pass owns under the active target —
    // window and off-screen share the same slot machinery (C6.4). The slot's
    // depth style and presenting role are carried per call and re-applied when
    // they changed; its stacking position follows the pass' explicit order.
    if (camera != nullptr) {
        ContentSlotRequest request;
        request.target     = target_key;
        request.camera     = camera;
        request.commands   = &commands;
        request.lights     = &lights;
        request.depth_mode = depth_mode;
        request.presenting = presenting;
        request.order      = pass_order;
        request.viewport   = viewport;
        renderContentSlot(request);
    }

    // Submission is deferred to swapBuffers() so one frame (main pass + all
    // overlay passes) is recorded and presented exactly once.
}

void VsgRenderer::buildOffscreenTarget(vine::graphics::RenderTarget* target)
{
    // (Re)build an off-screen target's GPU attachments + render graph, sized
    // to the target. The graph is created EMPTY: content-slot Views are
    // appended by setupContentSlot() as passes render into this target (C6.4:
    // one RT can hold several content slots, like the window target).
    // EXPERIMENTAL: must be validated on a real Vulkan device before
    // production use.
    if (target == nullptr || impl->window == nullptr) {
        return;
    }
    auto& t = impl->targets[target];

    // A rebuild (target resized) must first release the previous graph: it
    // may still be referenced by an in-flight command buffer, and every
    // content slot compiled against it must be dropped with it (per-view
    // pipelines bind the old render pass).
    if (t.graph != nullptr) {
        removeGraphChild(impl->command_graph.get(), t.graph);
        // Wait for any in-flight command buffer that may still reference the
        // old framebuffer/images before their Vk handles are destroyed.
        waitForIdle(impl->viewer.get());
        // Every slot this target held (content views, PiP screen views and
        // fullscreen-program views) was a child of the graph being dropped:
        // they must go with it, or their retained view would never be recorded
        // again while still reporting itself as ready.
        for (auto& slot_entry : t.content_slots) {
            slot_entry.second.bridge.clearCache();
            const auto& view = slot_entry.second.view;
            auto&       queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        }
        t.content_slots.clear();
        t.screen_slots.clear();
        t.program_slots.clear();
        t.color_images.clear();
        t.color_views.clear();
        t.depth_image          = {};
        t.depth_view           = {};
        t.render_pass          = {};
        t.render_pass_load     = {};
        t.depth_ready          = false;
        t.framebuffer          = {};
        t.depth_source         = nullptr;
        t.depth_share_barrier  = {};
        t.graph                = {};
        t.depth_on_shader_set  = {};
        t.depth_testonly_shader_set = {};
        t.depth_off_shader_set = {};
        t.width                = 0;
        t.height               = 0;
    }

    const uint32_t w = static_cast<uint32_t>(target->width());
    const uint32_t h = static_cast<uint32_t>(target->height());
    if (w == 0 || h == 0) {
        return;
    }
    t.width             = static_cast<int>(w);
    t.height            = static_cast<int>(h);
    auto       device     = impl->window->getOrCreateDevice();
    const int  color_count = target->colorCount();
    const bool has_color   = color_count > 0;
    const bool has_depth   = target->hasDepth();
    // Depth may be OWNED (allocated below) or BORROWED from an earlier target
    // in the same frame (RenderTarget::shareDepth): the deferred-lit composite
    // reuses the G-buffer's depth so forward content can test against it.
    // A borrow whose source has since been RELEASED cannot be honoured — the
    // source's VkImage is gone — so such a target builds with its own depth
    // instead of failing to build forever (see releaseRenderTarget).
    vine::graphics::RenderTarget* const depth_src = target->depthSource();
    const bool borrowed = depth_src != nullptr && t.unusable_depth_source != depth_src;

    std::vector<VkFormat> color_formats;
    color_formats.reserve(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        color_formats.push_back(toColorFormat(target->colorFormat(i)));
    }

    ::vsg::ImageViews attachments;
    attachments.reserve(static_cast<std::size_t>(color_count) + (has_depth ? 1u : 0u));
    // One colour image + view per attachment: each is written by fragment
    // output location i, then usable as a sampled texture on its own
    // (VK_IMAGE_USAGE_SAMPLED_BIT) or as a blit source for compositing.
    t.color_images.resize(static_cast<std::size_t>(color_count));
    t.color_views.resize(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        auto color           = ::vsg::Image::create();
        color->imageType     = VK_IMAGE_TYPE_2D;
        color->format        = color_formats[static_cast<std::size_t>(i)];
        color->extent        = VkExtent3D{ w, h, 1 };
        color->mipLevels     = 1;
        color->arrayLayers   = 1;
        color->tiling        = VK_IMAGE_TILING_OPTIMAL;
        color->usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        color->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.color_images[static_cast<std::size_t>(i)] = color;
        // createImageView compiles the Image (creates VkImage + allocates/binds
        // device memory) and creates+compiles the ImageView (VkImageView).
        // WITHOUT this the VkImage/VkImageView stay VK_NULL_HANDLE and the
        // Framebuffer holds a corrupt handle -> vkCmdBeginRenderPass crashes.
        t.color_views[static_cast<std::size_t>(i)] = ::vsg::createImageView(device.get(), color, VK_IMAGE_ASPECT_COLOR_BIT);
        attachments.push_back(t.color_views[static_cast<std::size_t>(i)]);
    }
    if (has_depth && !borrowed) {
        auto depth           = ::vsg::Image::create();
        depth->imageType     = VK_IMAGE_TYPE_2D;
        depth->format        = toDepthFormat(target->depthFormat());
        depth->extent        = VkExtent3D{ w, h, 1 };
        depth->mipLevels     = 1;
        depth->arrayLayers   = 1;
        depth->tiling        = VK_IMAGE_TILING_OPTIMAL;
        depth->usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        depth->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.depth_image        = depth;
        t.depth_view         = ::vsg::createImageView(device.get(), depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        attachments.push_back(t.depth_view);
    }
    else if (borrowed) {
        // Borrow the source's depth image/view (it renders earlier this frame):
        // the framebuffer below attaches the shared depth, loaded not cleared.
        auto src_it = impl->targets.find(depth_src);
        if (src_it == impl->targets.end() || src_it->second.depth_view == nullptr) {
            reportFailure(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                          formatDiagnostic(u8"shared-depth target '%s': source '%s' not built yet; this frame keeps its own depth",
                                           target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(),
                                           depth_src->name().empty() ? "(unnamed)" : depth_src->name().stdstr().c_str()));
            return;
        }
        attachments.push_back(src_it->second.depth_view);
    }

    // The depth policy of this target's pass follows its persisted clearDepth
    // request: CLEAR (the default, depth sampled afterwards) when the engine
    // asked to clear depth; depth-LOAD (depth preserved across frames, not
    // sampled) when it asked not to. A depth-LOAD pass cannot also promote the
    // depth to a sampled texture, so such a target must not be depth-sampled
    // (the deferred G-buffer always clears depth, so it never selects LOAD).
    const bool load_depth = t.clear_seen && !t.clear_depth;
    t.depth_load          = load_depth;
    if (has_color) {
        const VkFormat depth_format =
            has_depth ? (borrowed ? toDepthFormat(depth_src->depthFormat()) : toDepthFormat(target->depthFormat()))
                      : VK_FORMAT_UNDEFINED;
        if (borrowed) {
            // Composite sharing the source's depth: colour is cleared + stored
            // (sampleable for the present pass) while the depth is LOADED from
            // the source graph recorded earlier this frame. Both graphs leave
            // the depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; the depth-share
            // barrier inserted between them orders the write -> load/test.
            t.depth_load  = false;
            t.render_pass = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, /*initial_clear*/ false);
        }
        else if (load_depth) {
            // Two compatible passes over the same attachments: the first-frame
            // pass CLEARs the fresh (UNDEFINED) depth image — transitioning it
            // into DEPTH_STENCIL_ATTACHMENT_OPTIMAL and seeding its content —
            // so the steady depth-LOAD pass that follows is valid (a LOAD pass
            // cannot start from an UNDEFINED image). submitFrame records the
            // first-frame pass once, then the steady one.
            t.render_pass_load = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, false);
            t.render_pass      = makeDepthLoadRenderPass(device.get(), color_formats, depth_format, true);
            t.depth_ready      = false;
        } else {
            t.render_pass = makeSampleableRenderPass(device.get(), color_formats, depth_format, target->depthPromotion());
        }
    } else {
        t.render_pass = makeDepthOnlyRenderPass(device.get(), toDepthFormat(target->depthFormat()));
    }
    t.framebuffer = ::vsg::Framebuffer::create(t.render_pass, attachments, w, h, 1);

    t.graph              = ::vsg::RenderGraph::create();
    t.graph->framebuffer = t.framebuffer;
    t.graph->renderArea  = VkRect2D{
        { 0, 0 },
        { w, h }
    };
    t.graph->contents      = VK_SUBPASS_CONTENTS_INLINE;
    t.graph->viewportState = ::vsg::ViewportState::create(VkExtent2D{ w, h });
    // Clear values match the attachment order (colour attachments in order,
    // then depth). Attachment 0 is cleared to the last engine clear() colour
    // requested for this target (recorded on the target so a rebuild reapplies
    // it); extra MRT attachments clear transparent black (empty regions stay
    // black until a fragment writes them). Depth is cleared to the far plane
    // for depth-only (shadow) targets and to the value that makes the window
    // forward-path geometry test pass for colour+RT targets; a depth-LOAD pass
    // ignores its depth clear value (its depth attachment is not cleared).
    t.graph->clearValues.clear();
    for (int i = 0; i < color_count; ++i) {
        VkClearValue color_clear = {};
        if (i == 0) {
            color_clear.color = VkClearColorValue{
                { t.clear_seen ? t.clear_color.r : 0.2f,
                  t.clear_seen ? t.clear_color.g : 0.2f,
                  t.clear_seen ? t.clear_color.b : 0.2f,
                  t.clear_seen ? t.clear_color.a : 1.0f }
            };
        }
        t.graph->clearValues.push_back(color_clear);
    }
    if (has_depth) {
        // Colour targets clear depth to 0.0, matching what the window main's
        // clear() pushes — the geometry depth test that works for the window
        // forward path must see the same cleared value off-screen or every
        // fragment fails and nothing rasterises. Depth-only (shadow) targets
        // keep clearing to the far plane (1.0).
        VkClearValue depth_clear = {};
        depth_clear.depthStencil = VkClearDepthStencilValue{ has_color ? 0.0f : 1.0f, 0 };
        t.graph->clearValues.push_back(depth_clear);
    }

    // A (re)built target created FRESH colour views: any OTHER target that
    // samples this one (PiP screen slots / fullscreen-program slots) still
    // holds the OLD views and would sample a stale, no-longer-drawn image
    // (its stale check only watches source size, which a same-size rebuild
    // does not change). Drop those slots so the next drawScreenTexture /
    // drawScreenProgram call reattaches against the new attachments.
    for (auto& entry : impl->targets) {
        auto& other = entry.second;
        if (entry.first == target || other.graph == nullptr) {
            continue;
        }
        // A slot's sampled target is a slot ATTRIBUTE, so consumers are found
        // by inspecting it (a slot's key is its owning pass).
        const auto forget_view = [this](const ::vsg::ref_ptr<::vsg::View>& view) {
            auto& queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        };
        for (auto it = other.screen_slots.begin(); it != other.screen_slots.end();) {
            if (it->second.source_target == target) {
                removeGraphChild(other.graph.get(), it->second.view);
                it = other.screen_slots.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = other.program_slots.begin(); it != other.program_slots.end();) {
            if (it->second.source_target == target) {
                removeGraphChild(other.graph.get(), it->second.view);
                forget_view(it->second.view);
                it = other.program_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (borrowed) {
        // Create the depth-share barrier now (the source's graph is already
        // built earlier this frame). reconcileOffscreenOrder() inserts it into
        // the command graph right after the source's render graph so the depth
        // writes are visible before this pass LOADs / tests them.
        auto src_it = impl->targets.find(depth_src);
        if (src_it != impl->targets.end() && src_it->second.depth_image != nullptr) {
            // The depth image may be a COMBINED depth/stencil format (D24 ->
            // VK_FORMAT_D24_UNORM_S8_UINT). With separateDepthStencilLayouts
            // disabled, a barrier's subresource range must cover BOTH aspects
            // of such a format (VUID-VkImageMemoryBarrier-image-03320), so the
            // aspect mask follows the source's format instead of assuming a
            // depth-only image.
            const VkFormat src_depth_format = toDepthFormat(depth_src->depthFormat());
            const bool     has_stencil =
                src_depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
                src_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                src_depth_format == VK_FORMAT_D16_UNORM_S8_UINT;
            const VkImageAspectFlags aspect_flags =
                VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
            auto imb = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                src_it->second.depth_image,
                VkImageSubresourceRange{ aspect_flags, 0, 1, 0, 1 });
            t.depth_share_barrier = ::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                0,
                imb);
        }
        t.depth_source = depth_src;
    }

    if (impl->command_graph != nullptr) {
        // Add the graph as the command graph's last child, then reorder every
        // off-screen graph into a dependency-valid sequence — each consumer is
        // recorded after the targets it samples (see reconcileOffscreenOrder()
        // for why creation order alone is not enough).
        impl->command_graph->children.push_back(t.graph);
        reconcileOffscreenOrder();
    }
    std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL off-screen target '%s' %ux%u attached\n",
                 target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str(), w, h);
    ++impl->offscreen_build_count;
    // NOTE: no compile here — the graph is empty until its first content slot
    // is added; setupContentSlot() compiles the (whole) command graph then.
}

void VsgRenderer::reconcileOffscreenOrder()
{
    // Keeps the command graph's child render graphs in a dependency-valid
    // RECORD order. The engine can build a target's graph out of dependency
    // order — a producer (re)built after its consumers existed (resize or
    // depth-policy change in render()), or a consumer wired to a producer
    // built later — so creation order alone is not enough: a screen pass that
    // samples another target (drawScreenTexture / drawScreenProgram) reads
    // that target's colour texture, and the sample is only CURRENT when the
    // producer's graph is recorded before the consumer's in the same frame.
    // This orders every off-screen graph by its sampling edges (source before
    // the targets that sample it), seeding ties with the current child order
    // so unrelated targets keep a stable sequence; the window swapchain graph
    // stays the last child (it may itself sample off-screen targets).
    if (impl->command_graph == nullptr) {
        return;
    }
    auto& children = impl->command_graph->children;
    const auto win = impl->targets.find(nullptr);
    if (win == impl->targets.end() || win->second.graph == nullptr) {
        return;
    }
    const auto window_graph = win->second.graph;

    // Index every off-screen graph by its target, and collect the ones
    // currently in the command graph, preserving their current relative order
    // as the stable tie-break seed.
    std::map<vine::graphics::RenderTarget*, ::vsg::ref_ptr<::vsg::RenderGraph>> graph_of;
    for (const auto& entry : impl->targets) {
        if (entry.first != nullptr && entry.second.graph != nullptr) {
            graph_of.emplace(entry.first, entry.second.graph);
        }
    }
    std::vector<vine::graphics::RenderTarget*> present;
    present.reserve(graph_of.size());
    for (const auto& child : children) {
        if (child == window_graph) {
            continue;
        }
        for (const auto& entry : graph_of) {
            if (entry.second == child) {
                present.push_back(entry.first);
                break;
            }
        }
    }

    // Sampling edges: a consumer depends on every source it samples (screen
    // slot keys carry the sampled target; program slots are keyed by it).
    // Self-sampling is rejected on attach and mutual same-frame sampling
    // (ping-pong inside one frame) is not a supported pattern, so the edge
    // graph is acyclic in practice; a cycle would only leave targets in their
    // current order below.
    std::map<vine::graphics::RenderTarget*, int>                                              indegree;
    std::map<vine::graphics::RenderTarget*, std::vector<vine::graphics::RenderTarget*>> consumers;
    for (auto* t : present) {
        indegree[t] = 0;
    }
    for (auto* t : present) {
        const auto entry = impl->targets.find(t);
        if (entry == impl->targets.end()) {
            continue;
        }
        const auto& target = entry->second;
        const auto add_source = [&](vine::graphics::RenderTarget* source) {
            if (source == nullptr || source == t || graph_of.find(source) == graph_of.end()) {
                return;
            }
            consumers[source].push_back(t);
            ++indegree[t];
        };
        // A slot's sampled target is a slot attribute (its key is the owning
        // pass), so the dependency edges come from the attribute. A retired
        // (detached) slot is not recorded, so it contributes no edge.
        for (const auto& slot : target.screen_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
        for (const auto& slot : target.program_slots) {
            if (!slot.second.detached) {
                add_source(const_cast<vine::graphics::RenderTarget*>(slot.second.source_target));
            }
        }
    }

    // Stable topological order (Kahn): seed with the zero-indegree targets in
    // current order, emit each and unlock its consumers.
    std::vector<vine::graphics::RenderTarget*> order;
    order.reserve(present.size());
    std::vector<vine::graphics::RenderTarget*> ready;
    ready.reserve(present.size());
    for (auto* t : present) {
        if (indegree[t] == 0) {
            ready.push_back(t);
        }
    }
    for (std::size_t head = 0; head < ready.size(); ++head) {
        auto* t = ready[head];
        order.push_back(t);
        for (auto* c : consumers[t]) {
            if (--indegree[c] == 0) {
                ready.push_back(c);
            }
        }
    }
    for (auto* t : present) {
        if (indegree[t] > 0) {
            order.push_back(t); // cycle remainder (unsupported pattern)
        }
    }

    // Rewrite the command graph's children: off-screen graphs in dependency
    // order, then the window graph last. Reordering render-graph children only
    // changes per-frame record order — each graph is its own render pass, so
    // no recompilation is needed.
    children.clear();
    for (auto* t : order) {
        children.push_back(graph_of[t]);
        // After a graph whose depth another target borrows, insert that
        // borrower's depth-share barrier so its LOAD / depth test sees this
        // graph's writes (both share one depth image in the attachment layout).
        for (const auto& entry : impl->targets) {
            const auto& other = entry.second;
            if (other.depth_source == t && other.graph != nullptr && other.depth_share_barrier != nullptr) {
                children.push_back(other.depth_share_barrier);
            }
        }
    }
    children.push_back(window_graph);
}

void VsgRenderer::drawScreenTexture(vine::graphics::RenderTarget* source, int attachment)
{
    if (!impl->initialized || impl->viewer == nullptr || impl->window == nullptr || source == nullptr) {
        return;
    }

    // Consume the sub-viewport queued by setViewport() (the ScreenPass's PiP
    // rectangle); mirrors how render() consumes one for overlays.
    const std::optional<vine::graphics::Viewport> viewport = takePendingViewport();

    auto src_it = impl->targets.find(source);
    if (src_it == impl->targets.end() || src_it->second.color_views.empty()) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenTexture: source target has no colour attachment: the pass draws nothing");
        return;
    }
    const auto& src = src_it->second;
    if (src.width <= 0 || src.height <= 0) {
        return;
    }
    // Select the colour attachment to sample (MRT / G-buffer targets expose
    // several sampleable textures under one target). Out-of-range indexes
    // clamp to the last attachment so a mis-set consumer still draws.
    std::size_t attachment_index = 0;
    if (attachment > 0) {
        attachment_index = static_cast<std::size_t>(attachment);
        if (attachment_index >= src.color_views.size()) {
            attachment_index = src.color_views.size() - 1;
        }
    }
    const auto source_view = src.color_views[attachment_index];

    // PiP (screen) views are drawn into the CURRENT target (setRenderTarget;
    // nullptr = the window), so their slots live in that target's entry: a
    // pass can composite a sampled source into an off-screen target, enabling
    // post-processing chains (A -> B -> window). The slot is owned by the pass
    // drawing it (SlotKey); the sampled source + attachment are slot
    // ATTRIBUTES re-checked every frame, so a pass that switches its input (or
    // the attachment it reads of an MRT source) is rebuilt instead of silently
    // sampling the previous texture.
    vine::graphics::RenderTarget* dest = impl->active_target;
    impl->active_target               = nullptr;
    // A source == destination feedback loop would sample the very attachments
    // this pass writes. Reject it with a diagnostic (a ping-pong pair of
    // targets is the standard way to build a feedback chain).
    if (dest == source) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenTexture: source == destination (feedback loop): the pass draws nothing");
        return;
    }
    auto& dest_entry = impl->targets[dest];
    if (dest != nullptr) {
        // Writing into an off-screen target: (re)build its graph to its size.
        if (dest->colorCount() <= 0 || dest->width() <= 0 || dest->height() <= 0) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                          u8"drawScreenTexture: destination target has no usable colour attachment: the pass draws nothing");
            return;
        }
        if (dest_entry.graph == nullptr || dest_entry.width != dest->width() || dest_entry.height != dest->height()) {
            buildOffscreenTarget(dest);
            if (dest_entry.graph == nullptr) {
                return;
            }
        }
    }
    else if (dest_entry.graph == nullptr) {
        return; // window graph not created yet
    }
    const int surf_w = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().width) : dest_entry.width;
    const int surf_h = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().height) : dest_entry.height;

    // The pass owns its slot under this destination; if it drew elsewhere
    // before (its render target changed), drop that stale slot so it stops
    // compositing there.
    retargetPass(impl->current_pass, dest);

    // Pass-scoped identity when the engine opened a pass scope (the normal
    // path); the historical (source, attachment) identity otherwise, so a
    // direct driver that draws several PiPs in one frame stays distinct.
    const SlotKey key = (impl->current_pass != nullptr)
                            ? SlotKey::ownerPass(impl->current_pass)
                            : SlotKey::sampledTarget(source, static_cast<int>(attachment_index));

    // Drop a stale slot when the sampled source / attachment changed, or the
    // sampled target OR the destination was resized (the sampled colour view /
    // the baked viewport was rebuilt).
    {
        const auto old = dest_entry.screen_slots.find(key);
        if (old != dest_entry.screen_slots.end() && old->second.ready &&
            (old->second.source_target != source ||
             old->second.attachment != static_cast<int>(attachment_index) ||
             old->second.source_w != src.width || old->second.source_h != src.height ||
             old->second.dest_w != surf_w || old->second.dest_h != surf_h)) {
            removeGraphChild(dest_entry.graph.get(), old->second.view);
            dest_entry.screen_slots.erase(old);
        }
    }

    auto& slot = dest_entry.screen_slots[key];

    // Destination rectangle: the pass' sub-viewport, else the full surface.
    int req_x = 0, req_y = 0, req_w = surf_w, req_h = surf_h;
    if (viewport && viewport->width > 0 && viewport->height > 0) {
        req_x = viewport->x;
        req_y = viewport->y;
        req_w = viewport->width;
        req_h = viewport->height;
    }

    int rect_x = req_x, rect_y = req_y, rect_w = req_w, rect_h = req_h;
    if (rect_x < 0 || rect_y < 0 || rect_w > surf_w || rect_h > surf_h || rect_x + rect_w > surf_w || rect_y + rect_h > surf_h) {
        // The requested rect does not fit the surface (e.g. an anchor computed
        // before the surface size was known): auto-anchor bottom-right inside
        // the surface, keeping the (16:9) size within half of it.
        int w = req_w;
        int h = req_h;
        if (w > surf_w / 2) {
            w = surf_w / 2;
            h = static_cast<int>(w * 9 / 16);
        }
        if (h > surf_h / 2) {
            h = surf_h / 2;
            w = static_cast<int>(h * 16 / 9);
        }
        const int margin = 8;
        rect_x           = surf_w - w - margin;
        rect_y           = surf_h - h - margin;
        rect_w           = w;
        rect_h           = h;
    }

    if (!slot.ready) {
        // Capture the pass's explicit order (announced by the engine before
        // this pass) so the view stacks among the target's slot views at its
        // pipeline position: a full-screen present at a low order draws
        // beneath later HUD slots, while a small PiP at a high order stays on
        // top of them (the INT_MAX default keeps a legacy-created PiP last).
        slot.order = impl->pending_pass_order;
        impl->pending_pass_order = 0;
        slot.source_target = source;
        slot.attachment    = static_cast<int>(attachment_index);
        slot.source_w    = src.width;
        slot.source_h    = src.height;
        slot.dest_w      = surf_w;
        slot.dest_h      = surf_h;
        slot.source_view = source_view;

        // Full-screen textured triangle sampling the off-screen colour
        // attachment, drawn as another View of the DESTINATION target's render
        // graph (like overlays) so the sub-viewport clips the
        // picture-in-picture rectangle.
        const VkExtent2D surface{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) };
        ProgramNodeFailure screen_failure = ProgramNodeFailure::None;
        auto content = makeScreenTextureNode(source_view, surface, &screen_failure);
        if (content == nullptr) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error,
                          vine::graphics::DiagnosticCategory::CompileFailed,
                          screen_failure == ProgramNodeFailure::NoCompiler
                              ? u8"screen pass (PiP) needs the runtime GLSL compiler, which is unavailable: the pass draws nothing"
                              : u8"screen pass (PiP) shader failed to compile: the pass draws nothing");
            dest_entry.screen_slots.erase(key);
            return;
        }
        bool overlay_compile_failed = false;
        auto view = makeCompiledOverlayView(*impl->viewer, dest_entry.graph.get(), content,
                                            rect_x, rect_y, rect_w, rect_h,
                                            /*front*/ false, "screen pass",
                                            &overlay_compile_failed);
        if (view == nullptr) {
            if (overlay_compile_failed) {
                reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                              vine::graphics::DiagnosticCategory::CompileFailed,
                              u8"screen pass (PiP) view failed to compile; retrying with a full compile");
            }
            dest_entry.screen_slots.erase(key);
            return;
        }
        slot.camera        = view->camera;
        slot.view          = view;
        slot.ready         = true;
        // Position the view by its explicit order; the compile above already
        // ran against this target's render pass, so only the record order
        // changes.
        placeViewByOrder(dest, view, slot.order);
        std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL screen PiP %dx%d (att %zu) -> %s %d,%d %dx%d attached\n", src.width, src.height, attachment_index, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h);
        if (dest != nullptr) {
            // A new sampling edge appeared under an off-screen destination:
            // re-order the command graph so this consumer records after every
            // target it samples (source == dest is rejected above, so this
            // cannot feed the producer back on itself).
            reconcileOffscreenOrder();
        }
    }

    if (slot.ready && slot.detached) {
        // Re-attach a slot retired while its pass was inactive (see
        // retireInactivePassSlots): its node and pipeline were kept, only the
        // view was detached from the graph.
        placeViewByOrder(dest, slot.view, slot.order);
        slot.detached = false;
    }

    // Follow the requested sub-viewport each frame (dynamic viewport + scissor).
    slot.camera->viewportState = ::vsg::ViewportState::create(rect_x, rect_y, static_cast<uint32_t>(rect_w), static_cast<uint32_t>(rect_h));
}

namespace
{

/**
 * @brief Computes the world -> view rotation basis for a look-at camera. *
 * @param camera Vine camera (eye / target / up).
 * @param r      Receives the view-space X axis in world coords (right).
 * @param u      Receives the view-space Y axis in world coords (up).
 * @param f      Receives the view-space -Z axis in world coords (forward).
 */
void viewRotation(const vine::graphics::Camera* camera, double r[3], double u[3], double f[3])
{
    const auto eye    = camera->eye();
    const auto center = camera->target();
    const auto up_vec = camera->up();
    double fx = center.x - eye.x;
    double fy = center.y - eye.y;
    double fz = center.z - eye.z;
    const double fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-12) {
        fx /= fl;
        fy /= fl;
        fz /= fl;
    }
    else {
        fx = 0.0;
        fy = 0.0;
        fz = -1.0;
    }
    // r = normalize(f x up), u = r x f.
    double rx = fy * up_vec.z - fz * up_vec.y;
    double ry = fz * up_vec.x - fx * up_vec.z;
    double rz = fx * up_vec.y - fy * up_vec.x;
    const double rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rl > 1e-12) {
        rx /= rl;
        ry /= rl;
        rz /= rl;
    }
    else {
        rx = 1.0;
        ry = 0.0;
        rz = 0.0;
    }
    const double ux = ry * fz - rz * fy;
    const double uy = rz * fx - rx * fz;
    const double uz = rx * fy - ry * fx;
    r[0] = rx;
    r[1] = ry;
    r[2] = rz;
    u[0] = ux;
    u[1] = uy;
    u[2] = uz;
    f[0] = fx;
    f[1] = fy;
    f[2] = fz;
}

/**
 * @brief Fills a fullscreen light push block for a deferred-lighting pass.
 *
 * The G-buffer stores view-space normals / positions, so directional lights
 * are pre-transformed from world to view space on the CPU (the fragment
 * shader then never needs a view matrix). Supports the first ambient plus up
 * to three directional lights (the push block is exactly 128 bytes); further
 * lights are ignored (documented S4 limitation). When the pass carries no
 * ambient light a small default ambient is seeded, mirroring how a scene pass
 * with an empty light list keeps its view's default light: without it a
 * fullscreen program pass bound to no lights would shade everything to black
 * (ambient 0 x albedo) — a silent, hard-to-diagnose blank frame.
 *
 * @param camera Camera whose view transforms the lights (may be null).
 * @param lights Scene lights to bake (borrowed).
 * @param block  Receives the packed block (zeroed first).
 */
void fillLightPushBlock(const vine::graphics::Camera*                               camera,
                        const std::vector<const vine::graphics::Light*>&            lights,
                        LightPushBlock&                                             block)
{
    block = LightPushBlock{};
    if (camera == nullptr) {
        return;
    }
    // Perspective projection parameters for view-position reconstruction from
    // the G-buffer depth (near / far / proj00 / proj11).
    if (camera->projectionType() == vine::graphics::Camera::ProjectionType::Perspective) {
        const double fov    = camera->fieldOfView() * 0.5; // degrees
        const double cot    = 1.0 / std::tan(fov * 3.14159265358979323846 / 180.0);
        const double aspect = camera->aspectRatio();
        block.projparms[0]  = static_cast<float>(camera->nearPlane());
        block.projparms[1]  = static_cast<float>(camera->farPlane());
        block.projparms[2]  = static_cast<float>(cot / aspect); // proj[0][0]
        block.projparms[3]  = static_cast<float>(cot);          // proj[1][1]
    }
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(camera, r, u, f);
    int  dirlight    = 0;
    bool has_ambient = false;
    for (const auto* light : lights) {
        if (light == nullptr || !light->isEnabled()) {
            continue;
        }
        const auto c = light->color();
        switch (light->type()) {
        case vine::graphics::LightType::Ambient:
            block.ambient[0] = c.r;
            block.ambient[1] = c.g;
            block.ambient[2] = c.b;
            block.ambient[3] = light->intensity();
            has_ambient      = true;
            break;
        case vine::graphics::LightType::Directional:
            if (dirlight >= 3) {
                break; // the push block holds up to three directional lights
            }
            {
                const auto d = light->direction();
                // world -> view direction (rotation only): rows r, u, -f.
                double vx = r[0] * d.x + r[1] * d.y + r[2] * d.z;
                double vy = u[0] * d.x + u[1] * d.y + u[2] * d.z;
                double vz = -f[0] * d.x - f[1] * d.y - f[2] * d.z;
                const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (vl > 1e-9) {
                    vx /= vl;
                    vy /= vl;
                    vz /= vl;
                }
                float* dd = block.dirs[dirlight].data();
                float* cc = block.cols[dirlight].data();
                dd[0] = static_cast<float>(vx);
                dd[1] = static_cast<float>(vy);
                dd[2] = static_cast<float>(vz);
                dd[3] = 0.0f;
                cc[0] = c.r;
                cc[1] = c.g;
                cc[2] = c.b;
                cc[3] = light->intensity();
                ++dirlight;
            }
            break;
        default:
            break;
        }
    }
    if (!has_ambient) {
        // Keep an unlit fullscreen program visible: without any ambient the
        // fragment shader would multiply the albedo by zero (see the header).
        block.ambient[0] = 0.15f;
        block.ambient[1] = 0.15f;
        block.ambient[2] = 0.15f;
        block.ambient[3] = 1.0f;
    }
}

} // namespace

void VsgRenderer::drawScreenProgram(vine::graphics::RenderTarget*              source,
                                    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                    vine::raw_ptr<const vine::graphics::Camera>        camera)
{
    if (!impl->initialized || impl->viewer == nullptr || impl->window == nullptr || source == nullptr || program == nullptr) {
        return;
    }

    // Consume the sub-viewport queued by setViewport() (the pass's rectangle).
    const std::optional<vine::graphics::Viewport> viewport = takePendingViewport();

    auto src_it = impl->targets.find(source);
    if (src_it == impl->targets.end() || src_it->second.color_views.empty()) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenProgram: source target has no colour attachment: the pass draws nothing");
        return;
    }
    const auto& src = src_it->second;
    if (src.width <= 0 || src.height <= 0) {
        return;
    }

    // Fullscreen-program views are drawn into the CURRENT target
    // (setRenderTarget; nullptr = the window), so their slots live in that
    // target's entry: deferred / post passes can write into an off-screen
    // target as well as the window.
    vine::graphics::RenderTarget* dest = impl->active_target;
    impl->active_target               = nullptr;
    // A source == destination feedback loop would sample the very attachments
    // this pass writes. Reject it (a ping-pong pair of targets is the standard
    // way to build a feedback chain).
    if (dest == source) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"drawScreenProgram: source == destination (feedback loop): the pass draws nothing");
        return;
    }
    auto& dest_entry = impl->targets[dest];
    if (dest != nullptr) {
        if (dest->colorCount() <= 0 || dest->width() <= 0 || dest->height() <= 0) {
            reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
                          u8"drawScreenProgram: destination target has no usable colour attachment: the pass draws nothing");
            return;
        }
        if (dest_entry.graph == nullptr || dest_entry.width != dest->width() || dest_entry.height != dest->height()) {
            buildOffscreenTarget(dest);
            if (dest_entry.graph == nullptr) {
                return;
            }
        }
    }
    else if (dest_entry.graph == nullptr) {
        return; // window graph not created yet
    }
    const int surf_w = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().width) : dest_entry.width;
    const int surf_h = (dest == nullptr) ? static_cast<int>(impl->window->extent2D().height) : dest_entry.height;

    // The pass owns its slot under this destination; a pass that drew
    // elsewhere before (its render target changed) drops that stale slot here.
    retargetPass(impl->current_pass, dest);
    // Pass-scoped identity when a pass scope is open (normal path), else the
    // historical per-source identity used by direct drivers.
    const SlotKey slot_key = (impl->current_pass != nullptr)
                                 ? SlotKey::ownerPass(impl->current_pass)
                                 : SlotKey::sampledTarget(source);
    auto& slot = dest_entry.program_slots[slot_key];

    // Destination rectangle: the pass' sub-viewport, else the full surface
    // (clamped into the surface - the fullscreen draw has no auto-fit).
    int rect_x = 0, rect_y = 0, rect_w = surf_w, rect_h = surf_h;
    if (viewport && viewport->width > 0 && viewport->height > 0) {
        rect_x = viewport->x;
        rect_y = viewport->y;
        rect_w = viewport->width;
        rect_h = viewport->height;
    }
    if (rect_x < 0) {
        rect_w += rect_x;
        rect_x = 0;
    }
    if (rect_y < 0) {
        rect_h += rect_y;
        rect_y = 0;
    }
    if (rect_w <= 0 || rect_h <= 0) {
        return;
    }
    if (rect_x + rect_w > surf_w) {
        rect_w = surf_w - rect_x;
    }
    if (rect_y + rect_h > surf_h) {
        rect_h = surf_h - rect_y;
    }
    if (rect_w <= 0 || rect_h <= 0) {
        return;
    }

    // (Re)build the retained slot when it is missing, the sampled source
    // changed (or was resized: its colour views were rebuilt), the DESTINATION
    // was resized, or the program changed.
    const bool stale = !slot.ready || slot.source_target != source ||
                       slot.source_w != src.width || slot.source_h != src.height ||
                       slot.dest_w != surf_w || slot.dest_h != surf_h || slot.program != program;
    if (stale) {
        removeGraphChild(dest_entry.graph.get(), slot.view);
        slot = Impl::ProgramSlot{};
        // Capture the pass's explicit order (announced by the engine before
        // this pass) so the fullscreen view stacks at its pipeline position
        // among the target's content slots (e.g. between an opaque depth pass
        // and a forward transparent pass) instead of always drawing first.
        slot.order = impl->pending_pass_order;
        impl->pending_pass_order = 0;
        slot.push_data = ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(LightPushBlock)));
        const VkExtent2D surface{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) };
        ProgramNodeFailure program_failure = ProgramNodeFailure::None;
        auto node = makeFullscreenProgramNode(program, src.color_views, source->depthPromotion() ? src.depth_view : ::vsg::ref_ptr<::vsg::ImageView>(), surface, slot.push_data, &program_failure);
        if (node == nullptr) {
            const vine::String why =
                program_failure == ProgramNodeFailure::NoCompiler
                    ? u8"fullscreen program needs the runtime GLSL compiler, which is unavailable"
                    : program_failure == ProgramNodeFailure::NoFragmentStage
                          ? u8"fullscreen program has no fragment stage"
                          : u8"fullscreen program shader failed to compile";
            reportFailure(vine::graphics::DiagnosticSeverity::Error,
                          vine::graphics::DiagnosticCategory::CompileFailed,
                          why + vine::String(u8": the pass draws nothing"));
            dest_entry.program_slots.erase(slot_key);
            return;
        }
        slot.source_target = source;
        slot.source_w = src.width;
        slot.source_h = src.height;
        slot.dest_w   = surf_w;
        slot.dest_h   = surf_h;
        slot.program  = const_cast<vine::graphics::ShaderProgram*>(program);
        slot.node     = node;

        // Create + compile the fullscreen view against this target's render
        // pass (inserted provisionally at the front so the compile sees it),
        // then move it to its explicit-order position below.
        bool overlay_compile_failed = false;
        auto view = makeCompiledOverlayView(*impl->viewer, dest_entry.graph.get(), node,
                                            rect_x, rect_y, rect_w, rect_h,
                                            /*front*/ true, "fullscreen program",
                                            &overlay_compile_failed);
        if (view == nullptr) {
            if (overlay_compile_failed) {
                reportFailure(vine::graphics::DiagnosticSeverity::Warning,
                              vine::graphics::DiagnosticCategory::CompileFailed,
                              u8"fullscreen program view failed to compile; retrying with a full compile");
            }
            dest_entry.program_slots.erase(slot_key);
            return;
        }
        slot.camera        = view->camera;
        slot.view          = view;
        slot.ready         = true;
        placeViewByOrder(dest, view, slot.order);
        std::fprintf(stderr, "[VsgRenderer] EXPERIMENTAL deferred fullscreen program %dx%d -> %s %d,%d %dx%d attached\n", src.width, src.height, dest == nullptr ? "window" : "offscreen", rect_x, rect_y, rect_w, rect_h);
        if (dest != nullptr) {
            // New sampling edges (this program samples every colour attachment
            // of source) appeared under an off-screen destination: re-order so
            // the consumer records after its producer (source == dest is
            // rejected above, so this cannot feed back on itself).
            reconcileOffscreenOrder();
        }
    }

    // Consume the lights queued by setLights() (from the pass's content scene)
    // and push view-space light parameters each frame before record. An empty
    // list seeds a small default ambient (see fillLightPushBlock) so a
    // fullscreen program pass that carries no lights still shades its albedo
    // instead of rendering black.
    std::vector<const vine::graphics::Light*> lights = std::move(impl->pending_lights);
    impl->pending_lights.clear();
    LightPushBlock block{};
    fillLightPushBlock(camera, lights, block);
    if (slot.push_data != nullptr && slot.push_data->dataSize() >= sizeof(block)) {
        std::memcpy(slot.push_data->dataPointer(), &block, sizeof(block));
    }

    if (slot.ready && slot.detached) {
        // Re-attach a slot retired while its pass was inactive (see
        // retireInactivePassSlots): its node and pipeline were kept.
        placeViewByOrder(dest, slot.view, slot.order);
        slot.detached = false;
    }

    // Follow the requested sub-viewport each frame.
    slot.camera->viewportState = ::vsg::ViewportState::create(rect_x, rect_y, static_cast<uint32_t>(rect_w), static_cast<uint32_t>(rect_h));
}

void VsgRenderer::releaseWindowLayer(vine::raw_ptr<const vine::graphics::Camera> camera, int order)
{
    if (camera == nullptr || !impl->initialized) {
        return;
    }
    // Window content slots live in the window target (nullptr key) of the
    // output-target table, keyed by (camera, explicit pass order). Off-screen
    // slots are released together with their whole target (releaseRenderTarget).
    auto& t  = impl->targets[nullptr];
    // Legacy key (camera, order): only state created by a direct driver that
    // never opened a pass scope uses it. Engine-driven slots are released by
    // releasePass() (keyed by the pass itself).
    auto  it = t.content_slots.find(SlotKey::cameraOrder(camera, order));
    if (it == t.content_slots.end()) {
        return;
    }
    // Detach the slot's View from the window render graph so it is no longer
    // recorded each frame, then drop it (releases its compiled pipelines and
    // the per-slot bridge cache). Slot removal is rare, so a device wait
    // before the drop keeps the release safe against an in-flight frame.
    removeGraphChild(t.graph.get(), it->second.view);
    waitForIdle(impl->viewer.get());
    it->second.bridge.clearCache();
    t.content_slots.erase(it);
}

void VsgRenderer::releaseRenderTarget(vine::graphics::RenderTarget* target)
{
    if (target == nullptr || !impl->initialized) {
        return;
    }
    bool released = false;
    auto ot       = impl->targets.find(target);
    if (ot != impl->targets.end()) {
        // Remove the target's off-screen graph from the command graph before
        // dropping its images / views / render pass / framebuffer / slots.
        auto& t = ot->second;
        removeGraphChild(impl->command_graph.get(), t.graph);
        waitForIdle(impl->viewer.get());
        for (auto& slot_entry : t.content_slots) {
            slot_entry.second.bridge.clearCache();
            // A dropped slot must not stay queued for the frame's incremental
            // compile: its view no longer belongs to any target.
            const auto& view  = slot_entry.second.view;
            auto&       queue = impl->pending_compile_views;
            queue.erase(std::remove(queue.begin(), queue.end(), view), queue.end());
        }
        // Drop the target's whole table entry (its off-screen attachments /
        // render graph / content slots). Any sampling slot that reads it lives
        // in another target's slot tables and is removed right below.
        impl->targets.erase(ot);
        released = true;
    }
    // A target that BORROWED the removed target's depth (RenderTarget::
    // shareDepth) now references a destroyed depth image, and the barrier that
    // ordered the two graphs still points at it. Drop the borrow and force the
    // borrower to rebuild with its own depth: otherwise its framebuffer keeps
    // a dead attachment (and the command graph is ordered around a dead
    // barrier). Clearing the recorded size re-enters the rebuild path on its
    // next draw.
    for (auto& entry : impl->targets) {
        auto& other = entry.second;
        if (other.depth_source != target) {
            continue;
        }
        std::fprintf(stderr,
                     "[VsgRenderer] target '%s' borrowed the released target '%s' depth; "
                     "dropping the borrow (it rebuilds with its own depth)\n",
                     entry.first->name().empty() ? "(unnamed)" : entry.first->name().stdstr().c_str(),
                     target->name().empty() ? "(unnamed)" : target->name().stdstr().c_str());
        // Remember WHICH source became unusable instead of a global tombstone
        // set: a later shareDepth() with a live source clears the condition by
        // being a different pointer, and the memory is bounded by the live
        // targets rather than by every target ever released.
        other.unusable_depth_source = target;
        other.depth_source          = nullptr;
        other.depth_share_barrier   = {};
        other.width                 = 0;
        other.height                = 0;
        released                    = true;
    }
    // PiP (screen) slots live in the target that draws them; drop any that
    // sample this target's colour (any of its colour attachments). The slot's
    // sampled target is an attribute now (the key is its owning pass).
    for (auto& target_entry : impl->targets) {
        auto& slots = target_entry.second.screen_slots;
        for (auto it = slots.begin(); it != slots.end();) {
            if (it->second.source_target != target) {
                ++it;
                continue;
            }
            removeGraphChild(target_entry.second.graph.get(), it->second.view);
            waitForIdle(impl->viewer.get());
            it = slots.erase(it);
            released = true;
        }
    }
    // Fullscreen-program slots (deferred lighting) live under the drawing
    // target; drop any sampling the removed target.
    for (auto& target_entry : impl->targets) {
        auto& slots = target_entry.second.program_slots;
        for (auto it = slots.begin(); it != slots.end();) {
            if (it->second.source_target != target) {
                ++it;
                continue;
            }
            removeGraphChild(target_entry.second.graph.get(), it->second.view);
            waitForIdle(impl->viewer.get());
            it = slots.erase(it);
            released = true;
        }
    }
    if (released) {
        // The remaining command-graph child order may have changed (a sampling
        // edge disappeared, a graph was detached).
        reconcileOffscreenOrder();
        std::fprintf(stderr, "[VsgRenderer] released GPU resources for removed render target\n");
    }
}

void VsgRenderer::placeViewByOrder(vine::graphics::RenderTarget* target,
                                   const ::vsg::ref_ptr<::vsg::View>& view,
                                   int order)
{
    auto& t = impl->targets[target];
    if (t.graph == nullptr || view == nullptr) {
        return;
    }
    auto& children = t.graph->children; // RenderGraph is a Group: children are ref_ptr<Node>
    // Drop any previous position, then insert so the children stay ascending
    // by each slot's explicit order: content slots carry theirs, fullscreen-
    // program and PiP / present screen slots carry theirs, and any child with
    // no known slot sorts last. Reordering render-graph children only changes
    // per-frame record order within the (single) render pass, so no
    // recompilation is needed.
    for (auto it = children.begin(); it != children.end();) {
        if (it->get() == view.get()) {
            it = children.erase(it);
        }
        else {
            ++it;
        }
    }
    const auto child_order = [&](const ::vsg::ref_ptr<::vsg::Node>& child) -> int {
        for (const auto& kv : t.content_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        for (const auto& kv : t.program_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        for (const auto& kv : t.screen_slots) {
            if (kv.second.ready && kv.second.view.get() == child.get()) {
                return kv.second.order;
            }
        }
        return std::numeric_limits<int>::max();
    };
    auto it = children.begin();
    for (; it != children.end(); ++it) {
        if (child_order(*it) > order) {
            break;
        }
    }
    children.insert(it, view); // ref_ptr<View> -> ref_ptr<Node> (View is a Node)
}

void VsgRenderer::setupContentSlot(const SlotKey& key, vine::graphics::RenderTarget* target, vine::raw_ptr<const vine::graphics::Camera> camera, int order, vine::graphics::DepthMode depth_mode, bool presenting)
{
    // Content slots are retained Views under the TARGET's render graph — the
    // window target (target == nullptr) and every off-screen target share
    // this one mechanism. Each pass is its own View + bridge, so several
    // passes sharing one camera and order still stack as separate content.
    auto& t          = impl->targets[target];
    auto& content    = t.content_slots[key];
    if (content.ready) {
        return;
    }
    if (t.graph == nullptr) {
        // No graph yet (e.g. an off-screen target that failed to build): drop
        // the half-made slot. Reported because the pass then draws nothing,
        // and the host may have no other way to learn the target is unusable.
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::TargetBuildFailed,
                      u8"no render graph for the pass' target: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.order      = order;
    content.depth_mode = depth_mode;
    content.presenting = presenting;
    content.vsg_camera = persistent->cameraBridge.create(camera);
    if (content.vsg_camera == nullptr) {
        reportFailure(vine::graphics::DiagnosticSeverity::Error,
                      vine::graphics::DiagnosticCategory::ContentSkipped,
                      u8"camera bridge could not be created: the pass draws nothing");
        t.content_slots.erase(key);
        return;
    }
    content.root = ::vsg::Group::create();

    // Per-slot pipeline bridge. vsg compiles pipelines per viewID, so every
    // content slot keeps its own SceneBridge (sharing already-compiled
    // pipelines across views crashes GraphicsPipeline::vk()); the bridge's
    // shader set bakes the slot's depth policy.
    if (target == nullptr) {
        // Window slots share the renderer's (window-sized) shader sets.
        content.bridge.setShaderSet(depth_mode == vine::graphics::DepthMode::TestAndWrite ? impl->depth_on_shader_set
                                    : depth_mode == vine::graphics::DepthMode::TestOnly ? impl->depth_testonly_shader_set
                                                                                         : impl->depth_off_shader_set);
    }
    else {
        // Off-screen slots get a per-target shader set baked at the target's
        // size (created lazily).
        auto& set_ref = depth_mode == vine::graphics::DepthMode::TestAndWrite ? t.depth_on_shader_set
                        : depth_mode == vine::graphics::DepthMode::TestOnly ? t.depth_testonly_shader_set
                                                                            : t.depth_off_shader_set;
        if (set_ref == nullptr) {
            const bool depth_test  = depth_mode != vine::graphics::DepthMode::Disabled;
            const bool depth_write = depth_mode == vine::graphics::DepthMode::TestAndWrite;
            set_ref = buildShaderSet(persistent->shader_preset,
                                     VkExtent2D{ static_cast<uint32_t>(t.width), static_cast<uint32_t>(t.height) },
                                     depth_test, depth_write,
                                     target->colorCount());
        }
        content.bridge.setShaderSet(set_ref);
    }
    content.bridge.setMaterialManager(&persistent->materialManager);
    // Route this slot's rejections through the renderer's diagnostics (trace,
    // counters, host sink): the slot is what actually discovers them.
    installDiagnosticRoute(content.bridge);
    // The pass' depth policy reaches the pipeline through the bridge (it fills
    // the depth item of content that did not author one), so it must be set
    // before the slot's first sync; a later change invalidates the state.
    content.bridge.setContentDepthMode(depth_mode);
    content.bridge.clearCache();

    // Seed the slot's default light before the first compile. A slot whose
    // scene carries no lights (checked per frame) keeps this seed: the
    // window's presenting (full-target) slot gets vsg's default headlight,
    // everything else an ambient fill — ambient keeps HUD / off-screen content
    // readable from any angle (a directional headlight would shade the axis
    // gizmo dark from diagonal views). When the content scene provides lights
    // they replace this seed each frame, so the light source always reflects
    // the scene, never the slot's depth style.
    content.light_group = ::vsg::Group::create();
    content.headlight_seed = (presenting && target == nullptr);
    if (content.headlight_seed) {
        content.light_group->addChild(::vsg::createHeadlight());
    }
    else {
        content.light_group->addChild(makeAmbientLight(presenting ? "offscreen_ambient" : "content_ambient"));
    }

    content.view = ::vsg::View::create(content.vsg_camera);
    content.view->addChild(content.light_group);
    content.view->addChild(content.root);

    // Position the slot's View in the target's render graph by its explicit
    // order, then compile it before it is first recorded. Content slots are
    // only created from render() calls that follow initialize(), so the
    // target's graph is always present here.
    //
    // Within a target the slot views are stacked in ASCENDING pass order —
    // the order the caller gave addPass() and the engine already runs passes
    // in (placeViewByOrder keeps content, fullscreen-program and PiP / present
    // views all sorted by it). No main/on-top semantic constrains the order —
    // a pass positioned by the user at any order draws exactly there. Ordering
    // by the explicit value also keeps the pre-frame warm-up safe: warm-up may
    // create a higher-order (on-top) slot before a lower-order (main) slot has
    // run, but the main slot is inserted ahead of it by its smaller order when
    // it is finally created.
    placeViewByOrder(target, content.view, content.order);
    content.ready = true;
    if (impl->viewer != nullptr) {
        impl->viewer->compile();
    }
}

void VsgRenderer::renderContentSlot(const ContentSlotRequest& request)
{
    if (request.commands == nullptr || request.lights == nullptr) {
        return; // no command stream / light list to draw from
    }
    // The slot is owned by the pass that draws it (pass scope) or, for a
    // direct driver, by the historical (camera, order) pair.
    const SlotKey key = (impl->current_pass != nullptr)
                            ? SlotKey::ownerPass(impl->current_pass)
                            : SlotKey::cameraOrder(request.camera, request.order);

    auto& t  = impl->targets[request.target];
    auto  it = t.content_slots.find(key);
    if (it == t.content_slots.end() || !it->second.ready) {
        setupContentSlot(key, request.target, request.camera, request.order, request.depth_mode, request.presenting);
        it = t.content_slots.find(key);
    }
    if (it == t.content_slots.end() || !it->second.ready) {
        return; // slot could not be built (e.g. camera bridge failed)
    }
    auto& content = it->second;

    if (content.detached) {
        // The pass executes again after having been retired: re-attach its
        // retained view (its data and pipelines were kept, so no upload /
        // recompile is needed).
        placeViewByOrder(request.target, content.view, content.order);
        content.detached = false;
    }

    // The pass' properties are re-applied every frame, so changing them at run
    // time takes effect instead of leaving the slot with the state it was
    // first built with:
    //  - the depth policy is forwarded to the bridge (which rebuilds only the
    //    state wrappers, not the vertex data) and invalidates them on change;
    //  - the explicit pipeline order moves the view to its new stacking slot;
    //  - the presenting role drives the viewport each frame and re-seeds the
    //    slot's default light when it flips.
    if (content.depth_mode != request.depth_mode) {
        // Wait before the state rebuild: the pipelines / descriptor sets the
        // bridge is about to drop may still be referenced by a submitted
        // command buffer.
        waitForIdle(impl->viewer.get());
        content.depth_mode = request.depth_mode;
        content.bridge.setContentDepthMode(request.depth_mode);
        content.bridge.invalidateState();
    }
    if (content.order != request.order) {
        content.order = request.order;
        placeViewByOrder(request.target, content.view, request.order);
    }
    if (content.presenting != request.presenting) {
        content.presenting       = request.presenting;
        const bool want_headlight = (request.presenting && request.target == nullptr);
        if (content.headlight_seed != want_headlight) {
            content.headlight_seed = want_headlight;
            content.light_group->children.clear();
            if (want_headlight) {
                content.light_group->addChild(::vsg::createHeadlight());
            }
            else {
                content.light_group->addChild(makeAmbientLight(request.presenting ? "offscreen_ambient" : "content_ambient"));
            }
        }
    }

    // Full target extent for this slot's viewport: the live swapchain size for
    // the window target, the off-screen target's logical size otherwise.
    const int surf_w = (request.target == nullptr) ? static_cast<int>(impl->window->extent2D().width) : t.width;
    const int surf_h = (request.target == nullptr) ? static_cast<int>(impl->window->extent2D().height) : t.height;

    // Keep the slot's vsg camera viewport in step with its role each frame:
    // presenting (full-target) content always fills the whole target; other
    // content carries its pass sub-viewport when one was queued (an unset or
    // empty sub-viewport means the full target). The slot is created lazily on
    // its first render, so this also covers the first frame and any resize that
    // happened before the slot existed.
    if (content.presenting) {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(VkExtent2D{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) });
    }
    else if (request.viewport && request.viewport->width > 0 && request.viewport->height > 0) {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(request.viewport->x, request.viewport->y,
                                         static_cast<uint32_t>(request.viewport->width),
                                         static_cast<uint32_t>(request.viewport->height));
    }
    else {
        content.vsg_camera->viewportState =
            ::vsg::ViewportState::create(VkExtent2D{ static_cast<uint32_t>(surf_w), static_cast<uint32_t>(surf_h) });
    }

    persistent->cameraBridge.apply(request.camera, content.vsg_camera);

    // Lights come from the pass' content scene each frame (the scene is the
    // source of truth); an empty list keeps the slot's seeded default light.
    // The light source is never chosen by the slot's depth style.
    setGroupLights(content.light_group.get(), *request.lights);

    // The command stream is the source of truth: reconcile the retained slot
    // root against it (in-place for moves/material edits). The legacy own-
    // window debug path skips syncing the window's presenting slot.
    if (!(request.target == nullptr && forceOwnWindow() && content.presenting)) {
        std::vector<::vsg::ref_ptr<::vsg::Node>> created;
        content.bridge.syncRenderCommands(*request.commands, content.root.get(), &created);
        if (!created.empty()) {
            // Queue this slot's VIEW for an incremental (re)compile in
            // submitFrame(): traversing the View sets the correct viewID, so
            // the new/rebuild subtrees compile for the view they will be
            // recorded under (D22). One entry per view per frame.
            auto& pending = impl->pending_compile_views;
            if (std::find(pending.begin(), pending.end(), content.view) == pending.end()) {
                pending.push_back(content.view);
            }
        }
        // TEMP diagnostics (VINE_VSG_DIAG_MRT): report how many commands were
        // collected for this slot, how many geometry subtrees were built and
        // how many distinct pipeline variants the bridge registered, to tell
        // "no geometry collected" from "geometry not rasterised" and to
        // confirm pipeline sharing (variants << commands when states repeat).
        if (std::getenv("VINE_VSG_DIAG_MRT") != nullptr) {
            std::fprintf(stderr, "[MRT-DIAG] target=%s depth_mode=%d order=%d commands=%zu created=%zu rootChildren=%zu variants=%zu\n",
                         request.target == nullptr ? "window"
                                                   : (request.target->name().empty() ? "offscreen" : request.target->name().stdstr().c_str()),
                         static_cast<int>(request.depth_mode),
                         request.order,
                         request.commands->size(),
                         created.size(),
                         content.root->children.size(),
                         content.bridge.pipelineVariantCount());
        }
    }
}

void VsgRenderer::setViewport(int x, int y, int width, int height)
{
    impl->pending_viewport = vine::graphics::Viewport{ x, y, width, height };
}

std::optional<vine::graphics::Viewport> VsgRenderer::takePendingViewport()
{
    const std::optional<vine::graphics::Viewport> queued = impl->pending_viewport;
    impl->pending_viewport.reset();
    return queued;
}

void VsgRenderer::setPassOrder(int order)
{
    // Queue the pass' explicit pipeline order for the next draw call: the
    // engine announces each pass' addPass() order before it executes, so the
    // pass' slot can be stacked at that position (setupContentSlot /
    // placeViewByOrder).
    impl->pending_pass_order = order;
}

bool VsgRenderer::incrementalCompileViews()
{
    auto compileManager = impl->viewer->compileManager;
    if (compileManager == nullptr) {
        return false;
    }

    for (const auto& view : impl->pending_compile_views) {
        if (view == nullptr) {
            return false;
        }

        // Locate the owning target (window target keyed by nullptr) and the
        // retained content slot the view belongs to, so the compile context
        // can carry that target's render pass (window swapchain vs off-screen
        // framebuffer — a graphics pipeline cannot be created without one).
        Impl::Target*     owner    = nullptr;
        Impl::ContentSlot* slot    = nullptr;
        bool              is_window = false;
        for (auto& [target_key, target] : impl->targets) {
            for (auto& [slot_key, candidate] : target.content_slots) {
                if (candidate.ready && candidate.view == view) {
                    owner     = &target;
                    slot      = &candidate;
                    is_window = (target_key == nullptr);
                    break;
                }
            }
            if (slot != nullptr) {
                break;
            }
        }
        if (slot == nullptr) {
            // A pending view that is not a content slot (e.g. a PiP or
            // program slot compiled by another path): let the caller fall
            // back to the full compile.
            return false;
        }

        // Register the slot's (render pass + view) context once. The pool's
        // pooled traversal was built by CompileManager::create(viewer, hints)
        // when the window graph was still empty, so without this the pool has
        // no context that matches this view and compile() would compile
        // nothing.
        if (!slot->compile_context_registered) {
            ::vsg::CollectResourceRequirements collect;
            view->accept(collect);
            const auto& requirements = collect.requirements;
            try {
                if (is_window) {
                    if (impl->window == nullptr) {
                        return false;
                    }
                    compileManager->add(*impl->window, view, requirements);
                }
                else {
                    if (owner->framebuffer == nullptr || owner->framebuffer->getDevice() == nullptr) {
                        return false;
                    }
                    compileManager->add(*owner->framebuffer, view, requirements);
                }
            }
            catch (...) {
                return false;
            }
            slot->compile_context_registered = true;
        }

        // Compile ONLY this view: restrict the compile to the context whose
        // pre-assigned view matches, so the new/rebuild subtree is compiled
        // for the viewID it will be recorded under and no other slot is
        // touched.
        ::vsg::CompileResult result;
        try {
            ::vsg::View* const target_view = view.get();
            result = compileManager->compile(
                view, [target_view](::vsg::Context& context) { return context.view == target_view; });
        }
        catch (...) {
            return false;
        }
        if (!result) {
            return false;
        }
        // Feed dynamic data / slot / bin updates from the incremental compile
        // into the record tasks (per-frame dynamic buffers such as the DYNAMIC
        // opacity colour arrays rely on this).
        ::vsg::updateViewer(*impl->viewer, result);
    }

    return true;
}

void VsgRenderer::submitFrame()
{
    if (!impl->initialized || impl->viewer == nullptr) {
        return;
    }
    // Retire the retained state of every pass that did not execute this frame
    // (disabled, or no longer registered) BEFORE submitting: such a pass must
    // stop being drawn, and the removal itself needs a presented frame or the
    // stale content would stay on screen.
    retireInactivePassSlots();
    // The frame is submitted even when nothing was drawn. beginFrame() already
    // ACQUIRED a swapchain image for it, and an acquired image is only returned
    // to the presentation engine by presenting it: skipping the submission
    // (nothing to draw / every pass disabled) leaks one image per frame, which
    // the validation layer reports as
    // VUID-vkAcquireNextImageKHR-surface-07783 and which eventually starves the
    // swapchain. Re-recording an unchanged graph is cheap (vsg records the
    // command graph every frame anyway).
    // Compile any geometry synced this frame before the record. If this frame
    // never submits, the queue survives to the next submit (nothing was
    // presented in between).
    if (!impl->pending_compile_views.empty()) {
        // D22 incremental compile: ON by default. vsg's compileManager.compile
        // path is NOT wired for this renderer out of the box — the manager's
        // pooled traversal is built once at Viewer::compile() time, and in
        // Vine that first compile runs on an EMPTY window graph (content-slot
        // views are appended lazily later), so the pool holds no contexts and
        // compileManager->compile(view) silently compiles nothing ("successful"
        // but with unbuilt pipelines), which crashes at record
        // (GraphicsPipeline::vk on an empty _implementation). incrementalCompileViews()
        // registers each queued view's context into the pool itself and
        // compiles only that view, so it is safe to run every frame that some
        // slot gained geometry. Setting VINE_VSG_DISABLE_INCREMENTAL_COMPILE
        // forces the full-graph compile (stable, vsg skips already-compiled
        // objects) as an A/B escape hatch; any incremental failure also falls
        // back to the full compile automatically.
        bool compiled = false;
        if (std::getenv("VINE_VSG_DISABLE_INCREMENTAL_COMPILE") == nullptr &&
            impl->viewer->compileManager != nullptr) {
            compiled = incrementalCompileViews();
        }
        if (!compiled) {
            const auto compileResult = impl->viewer->compile();
            if (!compileResult) {
                reportFailure(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::CompileFailed,
                              formatDiagnostic(u8"frame compile failed (%s): newly added content is not drawn this frame",
                                               compileResult.message.c_str()));
            }
        }
        impl->pending_compile_views.clear();
    }
    // Depth-LOAD (clearDepth=false) off-screen targets alternate between two
    // compatible render passes: the first frame after a (re)build uses the
    // depth-CLEAR pass, which initialises the fresh depth image's layout and
    // content; every later frame uses the depth-LOAD pass, which preserves it.
    // Swapping the graph's render pass is legal because the two passes differ
    // only in the depth load-op (framebuffer / pipeline compatible).
    for (auto& entry : impl->targets) {
        auto& t = entry.second;
        if (entry.first == nullptr || t.graph == nullptr || !t.depth_load ||
            t.render_pass_load == nullptr) {
            continue;
        }
        t.graph->renderPass = t.depth_ready ? t.render_pass_load : t.render_pass;
        t.depth_ready       = true;
    }
    impl->viewer->recordAndSubmit();
    impl->viewer->present();

    // One frame has been submitted: release the retained nodes that were
    // parked kRetireRingDepth frames ago, when every command-buffer slot that
    // could still reference them has been re-recorded (see
    // SceneBridge::retireNode). Done after the submit so the parked objects
    // stay alive for the whole frame that dropped them.
    for (auto& target_entry : impl->targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            slot_entry.second.bridge.advanceRetireRing();
        }
    }
}

void VsgRenderer::clear(const vine::Color& backgroundColor, bool clearDepth)
{
    // A clear marks the next render() as main (depth-on) content; a render
    // without a preceding clear is styled on-top (HUD, depth-off). The style
    // is consumed in render(). This marker is separate from the real clear
    // below (colour + optional depth on the CURRENT target), so the depth-on/
    // off mechanism no longer swallows the actual clear semantics.
    impl->pending_presenting = true;

    // The clear applies to the CURRENT render target (set by setRenderTarget;
    // nullptr = the window): an off-screen pass's clear must reach ITS graph,
    // not the window's. The request is recorded on that target so a later
    // off-screen graph (re)build reapplies the colour and the depth policy
    // (buildOffscreenTarget), then pushed into the graph when one exists.
    vine::graphics::RenderTarget* key = impl->active_target;
    auto& t = impl->targets[key];
    const ::vsg::vec4 color{
        backgroundColor.r / 255.0f,
        backgroundColor.g / 255.0f,
        backgroundColor.b / 255.0f,
        backgroundColor.a / 255.0f
    };
    t.clear_seen  = true;
    t.clear_color = color;
    t.clear_depth = clearDepth;

    if (t.graph == nullptr) {
        // No graph yet (e.g. the first pass into an off-screen target): render()
        // builds one from the recorded request via buildOffscreenTarget.
        return;
    }

    if (key == nullptr) {
        // Window graph: the swapchain render pass (vsg-owned) clears colour AND
        // depth at the start of every frame, so the requested colour is pushed
        // through and the depth-clear value is the main pass's (0.0). The
        // window CANNOT honour clearDepth=false — vsg fixes the pass depth
        // load-op to CLEAR when the window is created — so a false request is
        // treated as true there (documented on RenderBackend::clear()); only
        // off-screen targets honour clearDepth through their depth-LOAD pass.
        const VkClearColorValue clear_value{
            { color.r, color.g, color.b, color.a }
        };
        t.graph->setClearValues(clear_value, VkClearDepthStencilValue{ 0.0f, 0 });
        return;
    }

    // Off-screen graph: update the colour entries in place (the pass was built
    // with the right depth load-op — CLEAR or LOAD — from t.clear_depth).
    // Attachment 0 gets the requested colour; extra MRT attachments stay
    // transparent black so untouched G-buffer regions remain empty.
    const int color_count = key->colorCount();
    const std::size_t n   = std::min<std::size_t>(t.graph->clearValues.size(),
                                                  static_cast<std::size_t>(color_count));
    for (std::size_t i = 0; i < n; ++i) {
        t.graph->clearValues[i].color = VkClearColorValue{
            { i == 0u ? color.r : 0.0f,
              i == 0u ? color.g : 0.0f,
              i == 0u ? color.b : 0.0f,
              i == 0u ? color.a : 0.0f }
        };
    }
}

void VsgRenderer::setDepthMode(vine::graphics::DepthMode mode)
{
    // The content's depth handling is explicit (Disabled / TestOnly /
    // TestAndWrite). Consumed by the next render() to pick the slot's depth
    // shader set. Independent of clear() (a pass can test-only against depth
    // an earlier pass of the same target wrote, without clearing) and of
    // lighting.
    impl->pending_depth_mode = mode;
}

void VsgRenderer::swapBuffers()
{
    // One record+submit+present per frame, after all passes were synced.
    submitFrame();
}

vine::raw_ptr<vine::graphics::MaterialManager> VsgRenderer::materialManager()
{
    return &persistent->materialManager;
}

void VsgRenderer::setShaderPreset(vine::graphics::ShaderPreset preset)
{
    persistent->shader_preset = preset;
}

void VsgRenderer::setWindowHandle(void* native_handle)
{
    persistent->bound_handle = native_handle;
}

void VsgRenderer::resize(int width, int height)
{
    (void)width;
    (void)height;
    if (impl->window != nullptr) {
        impl->window->resize();
    }
    // Every window presenting (full-target) content slot's camera viewport
    // follows the live window size so the render graph's render area tracks a
    // resize (renderContentSlot also re-derives each slot's viewport every
    // frame; refreshing here keeps slots correct even before their next
    // render). Other slots carry their own sub-viewport, re-set per frame by
    // their pass.
    auto& window_target = impl->targets[nullptr];
    if (impl->window == nullptr) {
        return;
    }
    const auto extent = impl->window->extent2D();
    for (auto& kv : window_target.content_slots) {
        auto& slot = kv.second;
        if (slot.ready && slot.vsg_camera != nullptr && slot.presenting) {
            slot.vsg_camera->viewportState = ::vsg::ViewportState::create(extent);
        }
    }
}

void* VsgRenderer::nativeHandle() const
{
    return persistent->bound_handle;
}

void VsgRenderer::installDiagnosticRoute(SceneBridge& bridge)
{
    // The bridge reports a diagnostic; the renderer turns it into the single
    // route (stderr trace + backend counters + host sink). Routing it through
    // the renderer instead of handing the host sink straight to the bridge is
    // what keeps diagnosticCount() honest: a bridge report is a backend report.
    bridge.setDiagnosticSink([this](const vine::graphics::RenderDiagnostic& diagnostic) {
        reportFailure(diagnostic.severity, diagnostic.category, diagnostic.message);
    });
}

void VsgRenderer::setDiagnosticSink(vine::graphics::DiagnosticSink sink)
{
    RenderBackend::setDiagnosticSink(std::move(sink));
    // Re-route every retained slot bridge, and remember it for slots created
    // later (setupContentSlot installs the route). The renderer is the object a
    // host holds, so it must be the single place the sink is set.
    for (auto& target_entry : impl->targets) {
        for (auto& slot_entry : target_entry.second.content_slots) {
            installDiagnosticRoute(slot_entry.second.bridge);
        }
    }
}

void VsgRenderer::reportFailure(vine::graphics::DiagnosticSeverity severity,
                                vine::graphics::DiagnosticCategory category,
                                const vine::String&             message)
{
    const char* level = severity == vine::graphics::DiagnosticSeverity::Error    ? "error"
                        : severity == vine::graphics::DiagnosticSeverity::Warning ? "warning"
                                                                                  : "info";
    std::fprintf(stderr, "[VsgRenderer] %s: %s\n", level, message.stdstr().c_str());
    reportDiagnostic(severity, category, message);
}

void VsgRenderer::frame()
{
    if (!impl->initialized || impl->viewer == nullptr) {
        return;
    }
    // VSG frame order: advance -> handleEvents -> update -> record -> present.
    // No Vine content is bound to the renderer: the engine drives content per
    // pass, so this convenience hook only presents whatever the passes synced
    // (submitFrame() skips when nothing was rendered this frame).
    beginFrame();
    endFrame();
    swapBuffers();
}

::vsg::ref_ptr<::vsg::Viewer> VsgRenderer::viewer() const
{
    return impl->viewer;
}

std::size_t VsgRenderer::offscreenBuildCount() const noexcept
{
    return impl->offscreen_build_count;
}

std::size_t VsgRenderer::detachedSlotCount() const noexcept
{
    std::size_t count = 0;
    for (const auto& entry : impl->targets) {
        for (const auto& kv : entry.second.content_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
        for (const auto& kv : entry.second.screen_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
        for (const auto& kv : entry.second.program_slots) {
            count += kv.second.detached ? 1u : 0u;
        }
    }
    return count;
}

V_VSG_NS_END
