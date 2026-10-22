#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/Lease.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>

#include "RenderDiagnostic.hpp"
#include "WindowPresentTarget.hpp"
#include "ReportOnce.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
class SceneView;

/**
 * @brief One render area: the ONE pass that draws this host window, how big the window is, and which window it is.
 *
 * @details
 * - The presentation is a DECLARATION with exactly one owner (this object): a host names its own main pass
 *   (`surface->present(pipeline->presentingPass())`), and with nothing declared a view's default window pass presents
 *   the area (`surface->present(*view)`).
 * - ONE AREA, ONE PICTURE: a second presentation of any kind is refused and reported once - two passes cannot both
 *   be the picture, and the swapchain's ONE clear means the presenting pass is whose clear policy the frame starts
 *   with.
 * - ONE AREA, ONE ENGINE, AND THE ENGINE OWNS IT: the only way to get an area is RenderEngine::createSurface,
 *   which keeps it - so the relation has an owner rather than a convention, a second area cannot be constructed at
 *   all, and two windows never share a frame. This area's size is always the size that frame's passes lay
 *   themselves out on.
 * - THE WINDOW IS THE AREA'S TOO: bindTarget() holds the target the DEVICE made for the host's window
 *   (RenderDevice::createSurfaceTarget), resize() is the ONE size announcement (the view, the frame, the target's
 *   window half), and a recreated window is re-announced ON the target, which moves the established session
 *   (device and compiled pipelines stay - see WindowPresentTarget::setWindowHandle).
 *
 * @note
 * - Presentation used to be INFERRED (RenderEngine::hasWindowPass): a global, order-sensitive guess that silently
 *   dropped the second view sharing a camera and could not tell "draws HERE" from "exists". Declaring replaced it.
 * - It is not a framebuffer and holds no pixel (the target owns the window's images, a RenderTarget an off-screen
 *   picture), does not divide into panes, route input or hold a focus - one area draws one view, and layout, hit
 *   testing and focus belong to the host (multi-pane was landed, then deliberately dropped).
 */
class VN_GRAPHICS_API RenderSurface : public RefCounted<RenderSurface> {
  public:
    friend class RenderEngine;  // the ONE creator (createSurface), which owns the result

    /** @brief Destroys the area: the view stops naming it; the pass stays registered and the view keeps its
     * pipeline (a declaration owns neither).
     *
     * @details
     * - Normally reached by ~RenderEngine, which owns this area: the engine releases its handle while it is still an
     * object, so nothing here can call into a destroyed engine. A host that kept its own handle after dropping the
     * control keeps a surface whose lease is empty (see engine()), which reports no presentation instead of failing.
     */
    ~RenderSurface();

  public:
    /** @brief Declares that @p view presents this surface: the pass of the pipeline the view adopted.
     *
     * @details
     * - The declaration for a host with no main pass of its own; the area never builds the pass - it resolves it
     *   back through the view (SceneView::setPipeline), so a view with NO pipeline is refused and reported: the
     *   host would otherwise believe a window is drawn into while nothing draws.
     * - Refused and reported, each for its own reason: a view that draws through ANOTHER engine (its passes live
     *   in that engine's frame), an area that is not the one its engine drives, and a second presentation of any
     *   kind (one area draws one picture).
     * - A view presenting another surface needs no rule of its own: a view can only present the area of its own
     *   engine (an engine drives one area - RenderEngine::createSurface).
     *
     * @param view View to present; must have adopted a pipeline, be bound to this surface's engine, present no
     *             other surface, and be the area's only presentation.
     * @return true when this declared the area's presentation; false when @p view already presented this surface,
     *         or could not.
     */
    bool present(SceneView& view);

    /** @brief Declares that @p pass presents this surface (the declaration for a host-built main pass).
     *
     * @details
     * - While it stands, the view's default window pass is NOT built, so the host keeps full control; a second
     *   presentation is refused (see present(SceneView&)).
     * - The pass is NOT adopted: the pipeline that built it owns it (the engine holds it while registered), and
     *   this area keeps its own reference so what it reports cannot outlive what it describes.
     * - Refused and reported when it draws nothing here: it must be one of THIS engine's registered passes
     *   (RenderEngine::addPass - a pipeline registers its own when built); a pass outside the frame is the same
     *   silence present(SceneView&) refuses.
     *
     * @param pass Pass that presents this surface; null is a no-op, and a pass that is not registered on this
     *             surface's engine is refused.
     * @return true when this declared the area's presentation; false when @p pass already presented this surface,
     *         was not the engine's, or a presentation was already there.
     */
    bool present(raw_ptr<RenderPass> pass);

    /** @brief Drops the presentation @p view declared: the view keeps its pipeline (that is the view's) and the
     * pass stays registered - what leaves is the statement that this surface is theirs.
     *
     * @param view View whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<SceneView> view);

    /** @brief Drops the presentation @p pass declared (see dismiss(SceneView*)).
     *
     * @param pass Pass whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<RenderPass> pass);

    /** @brief Whether anything PRESENTS this surface (not merely "something was declared").
     *
     * @details
     * - A declaration whose pass has left the frame (RenderEngine::removePass, a cleared pass list) presents
     *   nothing and answers false: the default-path host then builds its own picture instead of leaving the window
     *   to a pass that does not run.
     *
     * @return true when a presentation is in the frame.
     */
    [[nodiscard]] bool hasPresentation() const noexcept;

    /** @brief Gets the pass that draws this surface now, or null when nothing presents it.
     *
     * @details
     * - Resolved on every read, never remembered: a rebuilt pipeline (a recipe change) is a NEW window pass, and
     *   this is the answer a host names the engine's pass with. A declared pass no longer registered answers null
     *   as well - "declared" is not "drawn", and this is one answer the area must never invent.
     *
     * @return The presenting pass, or null (nothing presents this surface, the view has no window pass yet, or the
     *         declared pass left the frame).
     */
    [[nodiscard]] raw_ptr<RenderPass> presentingPass() const;

    /** @brief Gets the view this surface is presented by, or null.
     *
     * @details
     * - Null means the area's presentation is a pass a host declared itself (see present(RenderPass*)), and it is
     *   what a viewer reads to answer "has the host taken this area over".
     *
     * @return The presented view, or null.
     */
    [[nodiscard]] raw_ptr<SceneView> presentedView() const noexcept;

    /** @brief Records the size of this surface and gives it to the view that presents it.
     *
     * @details
     * - The size of the thing a picture ends up in is a fact about the SURFACE: a host reports it ONCE here (its
     *   window knows it - see SurfaceWindow), the view reads it back (SceneView::resize) rather than holding a
     *   second copy, and a view presented LATER is sized at once from what is remembered.
     * - The same ONE announcement serves the frame: the engine records it in the frame context every pass lays
     *   itself out on, and the window half (the swapchain, what hangs off the extent) is the TARGET's own call,
     *   made here - the engine owns no size of its own, the area owns the target (see PresentTarget::resize).
     * - A pass a host declared itself is not fed: it has no view, and such a pass states its own rectangle in
     *   device pixels (see RenderPass::setViewport).
     *
     * @param width        Surface width in logical pixels (<= 0 is ignored, with the ratio).
     * @param height       Surface height in logical pixels (<= 0 is ignored, with the ratio).
     * @param pixel_ratio  Device pixels per logical pixel; <= 0 keeps the ratio this surface last saw.
     */
    void resize(int width, int height, double pixel_ratio = 0.0);

    /** @brief Gets the device pixel ratio this surface last saw (resize).
     *
     * @details
     * - The ratio a view laid its geometry out with, and the one a host needs to turn a mouse position (device
     *   pixels) into the logical surface it reported.
     *
     * @return Device pixels per logical pixel; 1.0 until a host reports one.
     */
    [[nodiscard]] double pixelRatio() const noexcept;

    /** @brief Binds the target this area is presented through (its window), or unbinds it with nullptr.
     *
     * @details
     * - THE WINDOW BELONGS TO THE AREA, and the TARGET names it: the object its DEVICE made for that window
     *   (RenderDevice::createSurfaceTarget, where a device that presents nothing refuses and says why).
     *   Everything the window half is told - a recreated handle, the size, the frame's present - goes to that
     *   object, and the engine reads it from here (it owns no handle of its own, see RenderEngine).
     * - RE-ANNOUNCING MOVES A WINDOW, IT DOES NOT REBIND IT: a platform-recreated window (a reparent, a screen
     *   change) is the same area on a new handle - announced ON the target (WindowPresentTarget::setWindowHandle),
     *   which moves an established session while the device, its caches and every compiled pipeline stay. Binding
     *   a DIFFERENT target is a different window: the previous one is left behind (its device owns it -
     *   RenderDevice::shutdown).
     * - nullptr unbinds: the area presents nothing while the session stays up, and a target set up before its
     *   device exists is simply kept (it is an object - nothing has to take it). The ENGINE unbinds on shutdown()
     *   before it takes the device down (RenderEngine::shutdown), so a window closing is the device's business and
     *   this pointer never dangles.
     *
     * @param target The target this area is presented through (from its device), or nullptr to unbind. BORROWED:
     *               the device that made it owns it, and it lives until that device is destroyed or this area is
     *               unbound.
     */
    void bindTarget(raw_ptr<WindowPresentTarget> target);

    /** @brief Gets the target this area is presented through, or nullptr when it is bound to none.
     *
     * @details
     * - The question a host asks to decide whether to re-announce: the native handle the session was moved onto
     *   is the TARGET's own answer (WindowPresentTarget::nativeHandle), so comparing it with what the windowing
     *   system reports now is how "the platform recreated my window" is noticed.
     *
     * @return The bound target, or nullptr.
     */
    [[nodiscard]] raw_ptr<WindowPresentTarget> target() const noexcept;

    /** @brief Whether this area is bound to a window (see bindTarget).
     *
     * @return true when a target is bound to this area.
     */
    [[nodiscard]] bool hasTarget() const noexcept;

  private:
    friend class SceneView;  // reads the size it lays its geometry out with (see resize)

    /** @brief Creates the presentation of one surface rendered through @p engine (see RenderEngine::createSurface).
     *
     * @details
     * - PRIVATE, because the relation is 1:1 and has an owner: the engine creates the area it drives and keeps it,
     *   so "one area per engine" is a property of the type rather than a rule a constructor has to refuse.
     * - The engine is a BORROW through a lease: an area kept past its engine reports no presentation and lays nothing
     *   out instead of calling into freed memory.
     *
     * @param engine Engine this area is driven by (must be the engine creating it).
     */
    explicit RenderSurface(RenderEngine& engine);

    /** @brief The pass a presented view draws this surface through: the window pass of its live pipeline.
     *
     * @param view View to resolve.
     * @return The pass, or nullptr when there is none to resolve (no pipeline, no window pass).
     */
    raw_ptr<RenderPass> passOf(raw_ptr<SceneView> view) const;

    /** @brief Reports "this area already draws a picture" once per episode (the second-presentation refusal). */
    void reportSecondPresentation();

    /// The engine this area is registered on, BORROWED THROUGH A LEASE: the area does not own its engine, and an
    /// area that outlives it (a host that kept one) reads an empty lease instead of a pointer into freed memory -
    /// the same rule the view and the pipeline follow (see Lease and SceneView::engine_).
    Lease<RenderEngine>           engine_;
    raw_ptr<SceneView>             view_ = nullptr;
    intrusive_ptr<RenderPass>      pass_;
    raw_ptr<WindowPresentTarget>   target_ = nullptr;
    int                            width_ = 0;
    int                            height_ = 0;
    double                         pixel_ratio_ = 1.0;
    ReportOnce                     refused_{};
    ReportOnce                     unregistered_{};
    ReportOnce                     not_the_engines_{};
};

VN_GRAPHICS_NS_END
