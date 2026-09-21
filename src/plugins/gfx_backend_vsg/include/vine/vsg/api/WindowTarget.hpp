#pragma once

#include <cstdint>
#include <memory>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>

#include <vine/vsg/core/ClearPlan.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vsg
{
class Window;
}

/**
 * @brief The session's window as the frame's DEFAULT FRAMEBUFFER target: the one target the platform owns.
 *
 * WHY IT IS A TARGET AT ALL. The plan names the default framebuffer with a null target identity, and a pass
 * that draws into it is an ordinary pass in every other respect: it has a scope, a clear policy, content and
 * a place in the execution order. What it does NOT have is a render pass of this backend's making - the
 * swapchain's render pass belongs to the window (vsg creates it with the surface format and the depth format
 * the traits asked for), and the framebuffer is the one of the image the presentation engine has handed out
 * for the frame. So this type answers the three questions the executor and the content layer ask of any
 * target - what shape are you, what facts do you give the compiler, and where does a pass record into - and
 * it answers them from the window.
 *
 * ONE GRAPH FOR THE WHOLE WINDOW, and that is the platform's decision, not a simplification. vsg's render
 * graph over a window resolves the image's framebuffer WHILE RECORDING (the image index is only known once
 * the frame's image is acquired), and the window's render pass has fixed load operations - so every window
 * pass shares one begin/end render pass, and with it ONE clear. That clear is the first window pass' policy
 * (in execution order): the pass that draws underneath owns what the picture starts from, and a later pass
 * (a HUD layer, say) stacks on top of what is already there. An off-screen pass could clear per pass because
 * a target may build variants of its render pass; the swapchain's pass cannot, and pretending otherwise
 * would erase the earlier passes.
 *
 * ONE VIEW FOR THE WHOLE WINDOW, for the other platform fact: vsg compiles a pipeline per VIEW ID, and a
 * graphics pipeline is only usable in a compatible render pass. Off-screen passes all share view 0, where
 * every render pass of a given shape comes from the same recipe (so they are compatible with one another);
 * the window's render pass comes from vsg, with the surface's format and its own dependencies, so it is NOT
 * compatible with an off-screen pass of the same engine shape - and a variant shared between the two would
 * be bound in a render pass it was not compiled against. Recording the window's content under its own,
 * stable view gives the window its own compiled pipelines (one per variant object, vsg's normal per-view
 * behaviour) while the shared object stays shared, exactly as the previous implementation's slots did with
 * their own views.
 *
 * The view carries a PLACEHOLDER camera: vsg's compile traversal merges `view.camera->viewportState` into
 * the pipeline states without testing the camera first, so a camera-less view walks into a null pointer -
 * and nothing reads the matrices (the content reads the view block, and the rectangles arrive as viewport
 * commands), which is why a neutral camera is enough.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

/** @brief The window as the default-framebuffer target (see the file note). */
class V_VSG_API WindowTarget
{
  public:
    /** @brief Wraps @p window as this session's default-framebuffer target.
     *
     * The graph, the view and the shape are built here - and the shape is sampled ONCE, because a session
     * whose host window changes its swapchain format cannot move to it (see core::planSessionMove): the
     * surface format belongs to the render pass every pipeline of this window is compiled against.
     *
     * @param window The window a session renders into (borrowed: the session owns it and outlives this).
     * @return The target, or null when there is no window.
     */
    static std::unique_ptr<WindowTarget> create(::vsg::ref_ptr<::vsg::Window> window);

    ~WindowTarget();

    WindowTarget(const WindowTarget&)            = delete;
    WindowTarget& operator=(const WindowTarget&) = delete;

  public:
    /** @brief Brings the graph up to date for a frame: render area from the window, clear from @p policy.
     *
     * Called once per frame, by the FIRST window pass the executor records (its clear policy is the one the
     * window's single render pass begins from). The render area follows the window's CURRENT extent, because
     * the graph's own resize handling is off (installed, vsg scales the sub-viewport rectangles of the views
     * under the graph, and this backend derives every rectangle it owns from the target's live size - two
     * writers for one rectangle is what produced stretched rectangles on a maximize).
     *
     * @param policy What that pass asked the target to clear.
     */
    void prepare(const core::ClearPolicy& policy) noexcept;

    /** @brief Adds one pass' recorded content to the window's picture, in the order it is called. */
    void addContent(::vsg::ref_ptr<::vsg::Node> content) noexcept;

    /** @brief Gets the graph a window pass records into (the session's command graph holds it). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> graph() const noexcept;

  public:
    /** @brief Gets the window's shape, in the engine's vocabulary: one colour attachment and its depth.
     *
     * The surface format is mapped by BIT DEPTH AND KIND (an 8-bit surface becomes the engine's 8-bit colour
     * entry, a 16-bit float one its 16-bit entry, and so on): the engine's colour vocabulary has no
     * byte-order variants, and it does not need them here - the render pass the pipelines really compile
     * against is the window's own, and the window's content is compiled under its own view (see the file
     * note), so the engine format only decides WHICH VARIANTS this pass shares with off-screen passes.
     */
    [[nodiscard]] core::TargetShape shape() const noexcept;

    /** @brief Gets the facts the compiler resolves the default framebuffer from.
     *
     * The window is always "built" and always what the frame asks for: the swapchain's images are replaced
     * by vsg behind the graph (which is why the graph names the window and not an image), so nothing of the
     * target layer's repair work applies to it. Its depth is never sampleable: the window's depth is an
     * attachment, not a texture a later pass may read.
     */
    [[nodiscard]] core::TargetFacts facts() const noexcept;

    /** @brief Gets how many colour attachments a window pass draws into (1: the surface's image). */
    [[nodiscard]] std::uint32_t colorAttachmentCount() const noexcept;

    /** @brief Gets whether the window's depth may be sampled (never - see @ref facts). */
    [[nodiscard]] bool depthSampleable() const noexcept;

    /** @brief Gets the window's current width in device pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept;

    /** @brief Gets the window's current height in device pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept;

  private:
    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    WindowTarget();
};

V_VSG_NS_END
