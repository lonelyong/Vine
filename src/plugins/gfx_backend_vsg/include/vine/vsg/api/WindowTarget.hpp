#pragma once

#include <cstdint>
#include <memory>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
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
 * ONE VIEW FOR THE WHOLE WINDOW - and, measured in M4c, NOT the thing that keeps the window's pipelines apart
 * from an off-screen pass' ones. vsg compiles a pipeline per VIEW ID, and a graphics pipeline is only usable
 * in a compatible render pass; the window's render pass comes from vsg, with the surface's format and its own
 * dependencies, so it is NOT compatible with an off-screen pass of the same engine shape. The window's
 * content is recorded under its own stable view, and what that buys is a view id of its own; what it does NOT
 * buy is a separately compiled VkPipeline - vsg reuses an implementation whose pipeline STATES compare equal
 * whatever render pass it was built for, and a view with no state overrides adds no difference. The separation
 * that holds is the pipeline KEY: the DEVICE formats in its compatibility half (the swapchain's sRGB format
 * versus an off-screen target's linear one), which gives each family its own variant object. See
 * RenderPassCompatibility for the measurement.
 *
 * The view carries a PLACEHOLDER camera: vsg's compile traversal merges `view.camera->viewportState` into
 * the pipeline states without testing the camera first, so a camera-less view walks into a null pointer -
 * and nothing reads the matrices (the content reads the view block, and the rectangles arrive as viewport
 * commands), which is why a neutral camera is enough.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief The window as the default-framebuffer target (see the file note). */
class VN_VSG_API WindowTarget
{
  public:
    /** @brief Wraps @p window as this session's default-framebuffer target.
     *
     * The graph, the view and the shape are built here, and the shape is sampled from the swapchain NOW: a
     * session whose host window changes its swapchain format cannot move to it (see core::planSessionMove),
     * so this sample is what every pipeline of this window is compiled against until @ref refresh re-samples
     * it.
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

    /** @brief Brings the graph up to date for a frame that has NO window pass (see the file note).
     *
     * A frame that drew only off-screen content still PRESENTS: an image was acquired, and the only place it
     * gets a defined layout before the present is a render pass over it - the window's own graph. This is the
     * preparation for exactly that: the render area follows the window's live extent, and the clear values
     * stay what the last window pass left (a frame that draws only off-screen does not re-clear the window,
     * it gives the swapchain image the layout the present requires - see VsgExecutor::record).
     */
    void prepareWithoutClear() noexcept;

    /** @brief Opens the window's content for one frame: the PREVIOUS frame's recorded content is dropped.
     *
     * The content a frame's passes record is THE FRAME'S, and this is where that becomes true: the retained
     * view is what keeps the pipelines compiled and the view id stable (see the file note), so it must
     * outlive the frame - but the nodes of a recorded picture must not. Without this the frames would stack:
     * every recording would add to what the last one left, the window would keep drawing pictures nobody
     * asks for any more, and the group would grow without bound.
     *
     * The session's own content root is NOT affected (it is attached with addContent when the session comes
     * up and lives for the session), and a frame with no window pass at all does not call this - the last
     * recorded picture stays on screen, which is what an empty frame presents (see api/Session).
     */
    void beginFrame() noexcept;

    /** @brief Adds one pass' recorded content to THIS frame's picture, in the order it is called. */
    void addFrameContent(::vsg::ref_ptr<::vsg::Node> content) noexcept;

    /** @brief Adds content that stays for the session's life (the session's own root, see the file note). */
    void addContent(::vsg::ref_ptr<::vsg::Node> content) noexcept;

    /** @brief Gets the graph a window pass records into (the session's command graph holds it). */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> graph() const noexcept;

  public:
    /** @brief Gets the window's shape as this target's records were built against it: one colour attachment
     *         and its depth.
     *
     * The surface format is mapped by BIT DEPTH AND KIND (an 8-bit surface becomes the engine's 8-bit colour
     * entry, a 16-bit float one its 16-bit entry, and so on): the engine's colour vocabulary has no
     * byte-order variants, and it does not need them here - the render pass the pipelines really compile
     * against is the window's own, and the window's content is compiled under its own view (see the file
     * note), so the engine format only decides WHICH VARIANTS this pass shares with off-screen passes.
     *
     * This is the shape the pipelines of this window were KEYS on, so it is the one the plan compares the
     * facts with; what the swapchain serves right now is a separate read (see @ref facts and @ref refresh).
     */
    [[nodiscard]] core::TargetShape shape() const noexcept;

    /** @brief Re-samples the swapchain's shape; true when it changed under this target.
     *
     * The ONE event this answers is the platform having rebuilt the surface with another format (see
     * VsgHostWindow::moveToHostSurface, which refuses a different presentation format for a live session,
     * and Session::initialize, which rebuilds the session instead): the render pass this window's pipelines
     * are compiled against is the swapchain's, so such a change has to be RE-READ rather than assumed - a
     * target still serving a stale shape would hand those pipelines a render pass they were not compiled
     * for. Nothing is adopted from a caller's claim: the only writer of this target's shape is the platform.
     *
     * @return true when the shape the swapchain serves differs from the one this target reports; false when
     *         nothing changed (the common case: no host path can change it today).
     */
    [[nodiscard]] bool refresh();

    /** @brief Gets the facts the compiler resolves the default framebuffer from.
     *
     * WHAT THE FRAME WANTS AND WHAT THE TARGET HAS, kept apart like everywhere else: the WANTED shape is what
     * the swapchain serves NOW (a live read - see @ref refresh), while the CURRENT one is the shape this
     * target's records were built against (@ref shape). The pair is what makes "the platform rebuilt the
     * surface with another format" a REBUILD answer for the plan instead of a silent draw through pipelines
     * that were keyed on a render pass that no longer exists.
     *
     * The window is always "built" (vsg replaces the swapchain's images behind the graph - which is why the
     * graph names the window and not an image), so nothing of the target layer's repair work applies to it,
     * and its depth is never sampleable: the window's depth is an attachment, not a texture a later pass may
     * read.
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
    /** @brief What the swapchain serves NOW: the surface format and the traits' depth format, both spellings. */
    [[nodiscard]] core::TargetShape sampledShape() const;

    struct Data;
    // Lexically after Data so the out-of-line destructor is the only place that needs the complete type.
    std::unique_ptr<Data> d;

    WindowTarget();
};

VN_VSG_NS_END
