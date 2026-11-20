#include <vine/backend_vsg/internal/WindowTarget.hpp>

#include <algorithm>
#include <utility>
#include <vector>

#include <vsg/app/Camera.h>
#include <vsg/app/ProjectionMatrix.h>
#include <vsg/app/ViewMatrix.h>
#include <vsg/app/Window.h>
#include <vsg/commands/Command.h>
#include <vsg/maths/vec3.h>
#include <vsg/nodes/Group.h>
#include <vsg/vk/CommandBuffer.h>

#include <vine/backend_vsg/internal/TargetObjects.hpp>

VN_BACKEND_VSG_NS_BEGIN

namespace
{

/// @brief The engine's colour entry for a surface format, by bit depth and kind (see WindowTarget::shape).
vn::graphics::RenderTarget::ColorFormat toColorFormat(VkFormat format) noexcept
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return vn::graphics::RenderTarget::ColorFormat::RGBA8;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return vn::graphics::RenderTarget::ColorFormat::RGBA16F;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return vn::graphics::RenderTarget::ColorFormat::RGBA32F;
    default:
        // An unknown surface format falls back to the engine's 8-bit entry rather than failing the session:
        // the engine's spelling decides which variants the window's passes may SHARE with off-screen passes,
        // and the DEVICE format (reported next to it, see the create function) is the one that decides which
        // render passes are compatible at all - so a fallback here costs at most a variant that is not
        // shared, while refusing would make a session unable to start on a platform whose surface list this
        // table has not seen.
        return vn::graphics::RenderTarget::ColorFormat::RGBA8;
    }
}

/// @brief The engine's depth entry for the window's depth format, or nothing when the window has no depth.
std::optional<vn::graphics::RenderTarget::DepthFormat> toDepthFormat(VkFormat format) noexcept
{
    switch (format) {
    case VK_FORMAT_D16_UNORM: return vn::graphics::RenderTarget::DepthFormat::D16;
    case VK_FORMAT_D24_UNORM_S8_UINT: return vn::graphics::RenderTarget::DepthFormat::D24;
    case VK_FORMAT_D32_SFLOAT: return vn::graphics::RenderTarget::DepthFormat::D32F;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return vn::graphics::RenderTarget::DepthFormat::D32;
    default: return std::nullopt;
    }
}

/**
 * @brief A DEPTH-ONLY clear of one rectangle, recorded where it sits inside the window's render pass.
 *
 * @details
 * - The window's frame is ONE render pass instance (see the file note), so a later pass cannot begin one to reset the
 *   depth it is about to draw against. `vkCmdClearAttachments` clears a sub-rectangle of the attachments of the
 *   instance that is ALREADY open, so the reset happens exactly where the pass asked for it: one command, no second
 *   render pass, no attachment layout work - and the COLOUR attachments are not named, so the picture underneath
 *   survives, which is the whole difference between a HUD owning its depth and a HUD clearing the screen.
 * - The value is `kReverseZFarDepth`, the backend's far plane: every fragment the pass records is nearer than it (see
 *   ClearPlan for why the value is the far plane and not 1.0).
 * - Core Vulkan 1.0, so it is called by name like every other core command here (the entry points that need loading
 *   are the extension commands, see VsgVulkanEntryPoints).
 */
class ClearDepthRect : public ::vsg::Inherit<::vsg::Command, ClearDepthRect> {
  public:
    explicit ClearDepthRect(const VkRect2D& in_rect)
      : rect(in_rect)
    {}

    VkRect2D rect{};

    void record(::vsg::CommandBuffer& commandBuffer) const override
    {
        VkClearAttachment attachment{};
        attachment.aspectMask              = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachment.colorAttachment         = 0U; // read only for a colour aspect, which this one is not
        attachment.clearValue.depthStencil = VkClearDepthStencilValue{ core::kReverseZFarDepth, 0U };

        VkClearRect clear{};
        clear.rect           = rect;
        clear.baseArrayLayer = 0U;
        clear.layerCount     = 1U;

        ::vkCmdClearAttachments(commandBuffer, 1U, &attachment, 1U, &clear);
    }
};

} // namespace

struct WindowTarget::Data {
    ::vsg::ref_ptr<::vsg::Window>      window;
    ::vsg::ref_ptr<::vsg::RenderGraph> graph;
    ::vsg::ref_ptr<::vsg::View>        content_view;  // The window's own view (see the file note).
    ::vsg::ref_ptr<::vsg::Group>       frame_content; // THIS frame's recorded content (see beginFrame).
    core::TargetShape                  shape;
    /// The depth-clear commands THIS frame uses, taken from @ref depth_clears in order (see addDepthClear).
    std::vector<::vsg::ref_ptr<ClearDepthRect>> depth_clears;
    std::size_t                                 depth_clear_count{ 0 };
};

WindowTarget::WindowTarget()
  : d(std::make_unique<Data>())
{}

std::unique_ptr<WindowTarget> WindowTarget::create(::vsg::ref_ptr<::vsg::Window> window)
{
    if (window == nullptr) {
        return nullptr;
    }

    auto target       = std::unique_ptr<WindowTarget>(new WindowTarget());
    target->d->window = window;

    // The shape is what the swapchain serves at this moment (see sampledShape): the surface format the window
    // presents and the depth format its traits asked for, plus the DEVICE formats those map from.
    target->d->shape = target->sampledShape();

    auto graph                 = ::vsg::RenderGraph::create(window);
    graph->contents            = VK_SUBPASS_CONTENTS_INLINE;
    // vsg's own resize handling is OFF (see prepare): this backend writes the render area from the live
    // extent, and two writers for one rectangle is what produced stretched rectangles on a maximize.
    graph->windowResizeHandler = {};
    target->d->graph           = graph;

    // The window's content view: one stable view, so the window's content has its own view id. The rectangles
    // arrive as commands, which is what keeps an extent out of the pipeline's identity.
    //
    // NOTE (measured, M4c): this view does NOT keep the window's pipelines apart from an off-screen pass'
    // pipelines. vsg reuses an existing implementation whose pipeline STATES compare equal whatever render
    // pass it was built for, and since a stable view's states are only the pipeline's own, two views of one
    // pipeline object reuse each other's VkPipeline. What keeps the families apart is the pipeline KEY (the
    // device formats in its compatibility half - see RenderPassCompatibility); a view is what a per-view
    // state OVERRIDE would need, and nothing here overrides state.
    //
    // The view still needs a camera: vsg's compile traversal merges "view.camera->viewportState" into the
    // pipeline states without testing the camera first, so a camera-less view walks into a null pointer.
    // Nothing reads this camera's matrices - the content reads the blocks, and the viewport is a command -
    // so the values below are a neutral placeholder (a 1:1 camera looking down the canonical axis).
    auto placeholder_camera =
        ::vsg::Camera::create(::vsg::Perspective::create(45.0, 1.0, 0.1, 100.0),
                              ::vsg::LookAt::create(::vsg::dvec3(0.0, 0.0, 0.0), ::vsg::dvec3(0.0, 0.0, -1.0), ::vsg::dvec3(0.0, 1.0, 0.0)));
    target->d->content_view = ::vsg::View::create(placeholder_camera, ::vsg::ref_ptr<::vsg::Node>{});
    if (target->d->content_view == nullptr) {
        return nullptr;
    }
    // The frame's content goes into a group of its own, so a frame can REPLACE what the last one recorded
    // without touching the session's root (see beginFrame / addContent).
    target->d->frame_content = ::vsg::Group::create();
    if (target->d->frame_content == nullptr) {
        return nullptr;
    }
    target->d->content_view->addChild(target->d->frame_content);
    graph->addChild(target->d->content_view);

    const VkExtent2D extent = window->extent2D();
    graph->renderArea       = VkRect2D{
        { 0, 0 },
        extent
    };
    return target;
}

WindowTarget::~WindowTarget() = default;

void WindowTarget::prepare(const core::ClearValues& policy) noexcept
{
    const VkExtent2D extent = d->window->extent2D();
    d->graph->renderArea    = VkRect2D{
        { 0, 0 },
        extent
    };

    // The window's load operations are the platform's (the swapchain's render pass clears), so the plan's
    // "bootstrap" rule is the honest reading of it: attachment 0 takes the policy's colour, the depth the
    // policy's value (the reverse-Z far plane by default).
    const core::PassClearPlan plan  = core::planClearValues(d->shape,
                                                            policy,
                                                            /*bootstrap*/ true,
                                                            /*depth_borrowed*/ false);
    VkClearColorValue         color = {};
    if (!plan.colors.empty()) {
        for (std::size_t component = 0; component < 4U; ++component) {
            color.float32[component] = plan.colors.front().clear[component];
        }
    }
    if (plan.has_depth) {
        d->graph->setClearValues(color, VkClearDepthStencilValue{ plan.depth.clear, 0U });
    }
    else {
        d->graph->setClearValues(color);
    }
}

void WindowTarget::beginFrame() noexcept
{
    if (d->frame_content != nullptr) {
        d->frame_content->children.clear();
    }
    // The clear commands are REUSED frame to frame (see addDepthClear): a steady HUD frame must not allocate, and a
    // node per clear per frame would be exactly that. The pool keeps them alive; the frame takes them from the start.
    d->depth_clear_count = 0;
}

void WindowTarget::addFrameContent(::vsg::ref_ptr<::vsg::Node> content) noexcept
{
    if (content != nullptr && d->frame_content != nullptr) {
        d->frame_content->addChild(content);
    }
}

void WindowTarget::addDepthClear(const vn::graphics::Viewport& rect) noexcept
{
    if (d->frame_content == nullptr || !rect.isValid()) {
        return;
    }
    // The rectangle must lie inside the render area (the whole window, see prepare): a corner HUD whose layout outgrew
    // the surface is clamped here rather than handed to a command the API rejects.
    const VkExtent2D extent = d->window->extent2D();
    const int        left   = std::max(rect.x, 0);
    const int        top    = std::max(rect.y, 0);
    const int        right  = std::min(rect.x + rect.width, static_cast<int>(extent.width));
    const int        bottom = std::min(rect.y + rect.height, static_cast<int>(extent.height));
    if (right <= left || bottom <= top) {
        return;
    }
    const VkRect2D area{ VkOffset2D{ left, top },
                         VkExtent2D{ static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top) } };
    // ONE NODE PER CLEAR, REUSED: a HUD's clear happens every frame, so allocating its command here would put one
    // allocation in every steady frame (the gate every backend phase is measured against). The pool only grows on a
    // frame that needs more clears than any earlier one - a change of shape, not a steady frame's work.
    if (d->depth_clear_count == d->depth_clears.size()) {
        d->depth_clears.push_back(ClearDepthRect::create(area));
    }
    else {
        d->depth_clears[d->depth_clear_count]->rect = area;
    }
    d->frame_content->addChild(d->depth_clears[d->depth_clear_count]);
    ++d->depth_clear_count;
}

void WindowTarget::addContent(::vsg::ref_ptr<::vsg::Node> content) noexcept
{
    if (content != nullptr) {
        d->content_view->addChild(content);
    }
}

void WindowTarget::prepareWithoutClear() noexcept
{
    // Render area only: the clear values are the ones the graph already carries (the last window pass', or
    // the graph's defaults when no window pass has ever run - either way a defined VkClearValue, which is
    // all the present needs).
    const VkExtent2D extent = d->window->extent2D();
    d->graph->renderArea    = VkRect2D{
        { 0, 0 },
        extent
    };
}

::vsg::ref_ptr<::vsg::RenderGraph> WindowTarget::graph() const noexcept
{
    return d->graph;
}

core::TargetShape WindowTarget::shape() const noexcept
{
    return d->shape;
}

core::TargetShape WindowTarget::sampledShape() const
{
    // The window's own answer, in both spellings: the engine's vocabulary cannot tell the window's sRGB
    // surface from a linear off-screen target, and the two are not render-pass compatible (see
    // RenderPassCompatibility: the crossing is measured), so the device's format travels with it.
    core::TargetShape shape;
    shape.color_formats.push_back(toColorFormat(d->window->surfaceFormat().format));
    shape.device_color_formats.push_back(vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(d->window->surfaceFormat().format) });
    shape.depth_format        = toDepthFormat(d->window->depthFormat());
    shape.device_depth_format = vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(d->window->depthFormat()) };
    shape.render_pass_key     = kSingleSubpassRenderPass;
    return shape;
}

bool WindowTarget::refresh()
{
    core::TargetShape sampled = sampledShape();
    if (sampled == d->shape) {
        return false;
    }
    // The platform's answer REPLACES this target's record of it: the next compile keys the window's
    // pipelines on the new compatibility, and the next pass that draws into it begins from a clear (the plan
    // answers Rebuild for a shape change, and its bootstrap rule is what a render pass nobody has written
    // into needs).
    d->shape = std::move(sampled);
    return true;
}

core::TargetFacts WindowTarget::facts() const noexcept
{
    const VkExtent2D extent = d->window->extent2D();

    core::TargetFacts facts;
    facts.target             = nullptr; // the default framebuffer's identity
    facts.wanted.width       = static_cast<int>(extent.width);
    facts.wanted.height      = static_cast<int>(extent.height);
    // The swapchain serves this shape NOW (a live read), and the shape this target's records were built
    // against is what the plan compares it with: the pair is what turns "the platform rebuilt the surface"
    // into a REBUILD answer instead of a silent draw through the old compatibility's pipelines (see the
    // header).
    facts.wanted.shape       = sampledShape();
    facts.current.desc       = facts.wanted;
    facts.current.desc.shape = d->shape;
    facts.current.built      = true; // the window is whatever it is: nothing of the target layer's work applies
    facts.depth.has_depth    = facts.wanted.shape.depth_format.has_value();
    facts.depth.promotion    = false;
    facts.depth.borrowed     = false;
    return facts;
}

std::uint32_t WindowTarget::colorAttachmentCount() const noexcept
{
    return static_cast<std::uint32_t>(d->shape.color_formats.size());
}

bool WindowTarget::depthSampleable() const noexcept
{
    return false; // the window's depth is an attachment; nothing promotes it to a texture
}

std::uint32_t WindowTarget::width() const noexcept
{
    return d->window->extent2D().width;
}

std::uint32_t WindowTarget::height() const noexcept
{
    return d->window->extent2D().height;
}

VN_BACKEND_VSG_NS_END
