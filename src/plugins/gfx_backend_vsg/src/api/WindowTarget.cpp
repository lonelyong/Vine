#include <vine/vsg/api/WindowTarget.hpp>

#include <vector>

#include <vsg/app/Camera.h>
#include <vsg/app/ProjectionMatrix.h>
#include <vsg/app/ViewMatrix.h>
#include <vsg/app/Window.h>
#include <vsg/maths/vec3.h>

V_VSG_NS_BEGIN

namespace
{

/// @brief The engine's colour entry for a surface format, by bit depth and kind (see WindowTarget::shape).
vine::graphics::RenderTarget::ColorFormat toColorFormat(VkFormat format) noexcept
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return vine::graphics::RenderTarget::ColorFormat::RGBA8;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return vine::graphics::RenderTarget::ColorFormat::RGBA16F;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return vine::graphics::RenderTarget::ColorFormat::RGBA32F;
    default:
        // An unknown surface format falls back to the engine's 8-bit entry rather than failing the session:
        // this mapping only decides which VARIANTS the window's passes share with off-screen passes (the
        // pipelines themselves compile against the window's own render pass, under the window's own view),
        // so a fallback costs at most a variant that is not shared - while refusing here would make a session
        // unable to start on a platform whose surface list this table has not seen.
        return vine::graphics::RenderTarget::ColorFormat::RGBA8;
    }
}

/// @brief The engine's depth entry for the window's depth format, or nothing when the window has no depth.
std::optional<vine::graphics::RenderTarget::DepthFormat> toDepthFormat(VkFormat format) noexcept
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM: return vine::graphics::RenderTarget::DepthFormat::D16;
    case VK_FORMAT_D24_UNORM_S8_UINT: return vine::graphics::RenderTarget::DepthFormat::D24;
    case VK_FORMAT_D32_SFLOAT: return vine::graphics::RenderTarget::DepthFormat::D32F;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return vine::graphics::RenderTarget::DepthFormat::D32;
    default: return std::nullopt;
    }
}

}  // namespace

struct WindowTarget::Data
{
    ::vsg::ref_ptr<::vsg::Window>      window;
    ::vsg::ref_ptr<::vsg::RenderGraph> graph;
    ::vsg::ref_ptr<::vsg::View>        content_view;  ///< The window's own view (see the file note).
    core::TargetShape                  shape;
};

WindowTarget::WindowTarget() : d(std::make_unique<Data>())
{
}

std::unique_ptr<WindowTarget> WindowTarget::create(::vsg::ref_ptr<::vsg::Window> window)
{
    if (window == nullptr)
    {
        return nullptr;
    }

    auto target   = std::unique_ptr<WindowTarget>(new WindowTarget());
    target->d->window = window;

    // The shape is sampled once, and it is the window's own answer: the surface format it presents and the
    // depth format its traits asked for.
    target->d->shape.color_formats.push_back(toColorFormat(window->surfaceFormat().format));
    target->d->shape.depth_format = toDepthFormat(window->depthFormat());

    auto graph      = ::vsg::RenderGraph::create(window);
    graph->contents = VK_SUBPASS_CONTENTS_INLINE;
    // vsg's own resize handling is OFF (see prepare): this backend writes the render area from the live
    // extent, and two writers for one rectangle is what produced stretched rectangles on a maximize.
    graph->windowResizeHandler = {};
    target->d->graph           = graph;

    // The window's content view: one stable view, so vsg compiles the window's pipelines against the
    // window's own render pass (see the file note). The rectangles arrive as commands, which is what keeps
    // an extent out of the pipeline's identity.
    //
    // The view still needs a camera: vsg's compile traversal merges "view.camera->viewportState" into the
    // pipeline states without testing the camera first, so a camera-less view walks into a null pointer.
    // Nothing reads this camera's matrices - the content reads the blocks, and the viewport is a command -
    // so the values below are a neutral placeholder (a 1:1 camera looking down the canonical axis).
    auto placeholder_camera = ::vsg::Camera::create(
        ::vsg::Perspective::create(45.0, 1.0, 0.1, 100.0),
        ::vsg::LookAt::create(::vsg::dvec3(0.0, 0.0, 0.0), ::vsg::dvec3(0.0, 0.0, -1.0), ::vsg::dvec3(0.0, 1.0, 0.0)));
    target->d->content_view = ::vsg::View::create(placeholder_camera, ::vsg::ref_ptr<::vsg::Node>{});
    if (target->d->content_view == nullptr)
    {
        return nullptr;
    }
    graph->addChild(target->d->content_view);

    const VkExtent2D extent = window->extent2D();
    graph->renderArea       = VkRect2D{ { 0, 0 }, extent };
    return target;
}

WindowTarget::~WindowTarget() = default;

void WindowTarget::prepare(const core::ClearPolicy& policy) noexcept
{
    const VkExtent2D extent = d->window->extent2D();
    d->graph->renderArea    = VkRect2D{ { 0, 0 }, extent };

    // The window's load operations are the platform's (the swapchain's render pass clears), so the plan's
    // "bootstrap" rule is the honest reading of it: attachment 0 takes the policy's colour, the depth the
    // policy's value (the reverse-Z far plane by default).
    const core::PassClearPlan plan = core::planClearValues(d->shape, policy, /*bootstrap*/ true,
                                                          /*depth_preserved*/ false);
    VkClearColorValue         color = {};
    if (!plan.colors.empty())
    {
        for (std::size_t component = 0; component < 4U; ++component)
        {
            color.float32[component] = plan.colors.front().clear[component];
        }
    }
    if (plan.has_depth)
    {
        d->graph->setClearValues(color, VkClearDepthStencilValue{ plan.depth.clear, 0U });
    }
    else
    {
        d->graph->setClearValues(color);
    }
}

void WindowTarget::addContent(::vsg::ref_ptr<::vsg::Node> content) noexcept
{
    if (content != nullptr)
    {
        d->content_view->addChild(content);
    }
}

::vsg::ref_ptr<::vsg::RenderGraph> WindowTarget::graph() const noexcept
{
    return d->graph;
}

core::TargetShape WindowTarget::shape() const noexcept
{
    return d->shape;
}

core::TargetFacts WindowTarget::facts() const noexcept
{
    const VkExtent2D extent = d->window->extent2D();

    core::TargetFacts facts;
    facts.target         = nullptr;  // the default framebuffer's identity
    facts.wanted.width   = static_cast<int>(extent.width);
    facts.wanted.height  = static_cast<int>(extent.height);
    facts.wanted.shape   = d->shape;
    facts.current.desc   = facts.wanted;
    facts.current.built  = true;  // the window is whatever it is: nothing of the target layer's work applies
    facts.depth.has_depth = d->shape.depth_format.has_value();
    facts.depth.promotion = false;
    facts.depth.borrowed  = false;
    return facts;
}

std::uint32_t WindowTarget::colorAttachmentCount() const noexcept
{
    return static_cast<std::uint32_t>(d->shape.color_formats.size());
}

bool WindowTarget::depthSampleable() const noexcept
{
    return false;  // the window's depth is an attachment; nothing promotes it to a texture
}

std::uint32_t WindowTarget::width() const noexcept
{
    return d->window->extent2D().width;
}

std::uint32_t WindowTarget::height() const noexcept
{
    return d->window->extent2D().height;
}

V_VSG_NS_END
