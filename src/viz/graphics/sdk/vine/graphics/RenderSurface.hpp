#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
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
 * A surface is the thing a picture ends up in (the host's native window, a swapchain). Which pass draws into it is a
 * fact about the SURFACE, not about any single pass - and it has exactly one owner, this object: a host declares its
 * presentation here, and this is the answer to "who paints this window".
 *
 * That matters because "who presents" used to be inferred: a view asked the engine whether ANY registered pass
 * carried its camera with a null render target (RenderEngine::hasWindowPass) and stood down when one did - a
 * global, order-sensitive guess with no owner, which silently dropped the SECOND view sharing a camera and could
 * not tell "this pass draws HERE" from "this pass exists" (a HUD pass draws into the window too). A host that
 * builds its own main pass now says so:
 *
 * @code
 * control->surface()->present(pipeline->windowPass());   // this pass presents this surface
 * @endcode
 *
 * and a view's default window pass is built when nothing said so:
 *
 * @code
 * if (!control->surface()->hasPresentation()) {
 *     control->surface()->present(*control->view());     // the fallback: my view presents this surface
 * }
 * @endcode
 *
 * ONE AREA, ONE PICTURE (which is what makes the two calls above alternatives rather than additions): a second
 * presentation is REFUSED and reported once, because two passes writing one window cannot both be the picture, and
 * the clear rule is the same fact seen from the other side - the swapchain render pass expresses ONE clear, so the
 * presentation that draws the area is the one whose clear policy the frame starts with.
 *
 * ONE AREA, ONE ENGINE: the engine this area is built on drives it, and only it (see RenderEngine::adoptArea) - a
 * second area on the same engine is refused, so two windows never end up sharing one frame, and the size this area
 * announces is always the size that frame's passes lay themselves out on.
 *
 * THE WINDOW IS THE AREA'S TOO. What this area is presented through - the native window the host chose (the
 * TARGET, in this design's vocabulary) - is a fact about the AREA, exactly like the size: bindTarget() holds the
 * object the DEVICE made for that window (a device that presents nothing refuses there, once, instead of
 * pretending to attach - see RenderDevice::createSurfaceTarget), resize() is the ONE size announcement (the view,
 * the frame the passes lay out on, and the target's own window half), and a recreated window is announced on that
 * target (which MOVES an established session instead of rebuilding it: the device and every compiled pipeline stay
 * - see WindowPresentTarget::setWindowHandle). The engine drives the frame and owns no handle of its own (see
 * RenderEngine).
 *
 * WHAT THIS OBJECT DOES NOT DO. It is not a framebuffer and holds no pixel: the window's images belong to the
 * target, an off-screen picture to a RenderTarget. It does not divide itself into panes, does not route input and
 * does not hold a focus - one area draws one view (multi-pane was landed and then dropped: layout, hit testing and
 * focus belong to the host, and a host that needs them keeps its own table of where its views are).
 */
class VN_GRAPHICS_API RenderSurface : public RefCounted<RenderSurface> {
  public:
    /** @brief Creates the presentation of one surface rendered through @p engine.
     *
     * The registration happens here: an area is the engine's from the moment it exists (see
     * RenderEngine::adoptArea - the engine drives ONE area, and a second one that tries to register is refused and
     * reported, so an area can never be half-attached).
     *
     * @param engine Engine the presentation is registered on and that drives it (borrowed; must outlive this
     *               surface). Required: an area without an engine could not present anything, has no way to find one
     *               and says nothing about it, which is a state worth not having.
     */
    explicit RenderSurface(RenderEngine& engine);

    /** @brief Destroys the area.
     *
     * The presentation is a declaration, not an owner: the pass stays registered on the engine and the view keeps
     * its pipeline. What does happen is that the view stops naming this surface - it is going away, and a view that
     * outlives it must not reach back into it (see SceneView::~SceneView).
     */
    ~RenderSurface();

  public:
    /** @brief Declares that @p view draws this surface.
     *
     * The declaration a host makes when it has no main pass of its own: this surface is presented by the pass of the
     * pipeline the VIEW adopted (SceneView::setPipeline). The area never builds one - it resolves the pass back
     * through the view - so a view with no pipeline is refused and reported: a declaration without one would leave
     * the host believing a window is being drawn into while nothing draws.
     *
     * Refused and reported, each for its own reason: a view that draws through ANOTHER engine (its passes are
     * registered on that engine's frame, so it could never present this surface), an area that is not the one its
     * engine drives (the second area on one engine - see the class note), and a second presentation of any kind (one
     * area draws one picture).
     *
     * "A view that already presents another surface" needs no rule of its own: an engine drives one area, so a view
     * can only ever present the area of the engine it is bound to (see the class note and RenderEngine::adoptArea).
     *
     * @param view View to present; must have adopted a pipeline, be bound to this surface's engine, present no
     *             other surface, and be the area's only presentation.
     * @return true when this declared the area's presentation; false when @p view already presented this surface,
     *         or could not.
     */
    bool present(SceneView& view);

    /** @brief Declares that @p pass draws this surface.
     *
     * The declaration a host makes when it built its own main pass (an application pipeline presenting this
     * surface): the view's default window pass is not built while this declaration stands, so the host keeps full
     * control. A second presentation is refused (see present(SceneView&)).
     *
     * The pass is NOT adopted: the pipeline that built it owns it (and the engine holds it while registered). This
     * area keeps its own reference so what it reports cannot outlive what it describes.
     *
     * Refused and reported when it draws nothing here: the pass has to be one of THIS engine's registered passes
     * (RenderEngine::addPass - a pipeline registers its own when it is built). A pass that is not in the frame is
     * the same silence present(SceneView&) refuses, and a host that declared one would be told the window is being
     * drawn into while nothing runs.
     *
     * @param pass Pass that presents this surface; null is a no-op, and a pass that is not registered on this
     *             surface's engine is refused.
     * @return true when this declared the area's presentation; false when @p pass already presented this surface,
     *         was not the engine's, or a presentation was already there.
     */
    bool present(raw_ptr<RenderPass> pass);

    /** @brief Drops the presentation @p view declared.
     *
     * The view keeps its pipeline (that is the view's), and the pass stays registered: what leaves is the statement
     * that this surface is theirs.
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
     * The question the default-path host asks before falling back to its own view (see the class docs). A declaration
     * whose pass has left the frame (RenderEngine::removePass, a cleared pass list) presents nothing, and it answers
     * false for it: the host then builds its own picture instead of leaving the window to a pass that does not run.
     *
     * @return true when a presentation is in the frame.
     */
    [[nodiscard]] bool hasPresentation() const noexcept;

    /** @brief Gets the pass that draws this surface now, or null when nothing presents it.
     *
     * Resolved on every read, never remembered: a view's window pass is a NEW pass each time a recipe change
     * rebuilds its pipeline, and this is what a host asks to name the pass the engine runs (see the class note). A
     * declared pass that is no longer registered on the engine answers null as well - "declared" is not "drawn", and
     * this is the one answer the area must never invent.
     *
     * @return The presenting pass, or null (nothing presents this surface, the view has no window pass yet, or the
     *         declared pass left the frame).
     */
    [[nodiscard]] raw_ptr<RenderPass> presentingPass() const;

    /** @brief Gets the view this surface is presented by, or null.
     *
     * Null means the area's presentation is a pass a host declared itself (see present(RenderPass*)), which is also
     * what a viewer reads to answer "has the host taken this area over".
     *
     * @return The presented view, or null.
     */
    [[nodiscard]] raw_ptr<SceneView> presentedView() const noexcept;

    /** @brief Records the size of this surface, keeping the pixel ratio it last saw.
     *
     * @param width  Surface width in logical pixels (the unit the host reports; <= 0 is ignored).
     * @param height Surface height in logical pixels (<= 0 is ignored).
     */
    void resize(int width, int height);

    /** @brief Records the size of this surface and gives it to the view that presents it.
     *
     * The size of the thing a picture ends up in is a fact about the SURFACE: a host reports it ONCE (its window
     * knows it - see SurfaceWindow), and the view reads it from here rather than holding a second copy
     * (SceneView::resize is the one step). A view presented LATER receives the size remembered here at
     * once, so a view added to a running surface is sized without the host feeding it.
     *
     * The same ONE announcement serves the frame: the engine records it in the frame context every pass lays itself
     * out on, and the window half of it - the swapchain, what hangs off the extent - is the TARGET's own call, made
     * here (see PresentTarget::resize - the engine owns no size of its own, the area owns the target).
     *
     * A pass a host declared itself is not fed - it has no view, and such a pass states its own rectangle in device
     * pixels (see RenderPass::setViewport).
     *
     * @param width        Surface width in logical pixels (<= 0 is ignored, with the ratio).
     * @param height       Surface height in logical pixels (<= 0 is ignored, with the ratio).
     * @param pixel_ratio  Device pixels per logical pixel; <= 0 keeps the ratio this surface last saw.
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Gets the device pixel ratio this surface last saw (resize).
     *
     * The ratio a view laid its geometry out with, and the one a host needs to turn a mouse position (device pixels)
     * into the logical surface it reported.
     *
     * @return Device pixels per logical pixel; 1.0 until a host reports one.
     */
    [[nodiscard]] double pixelRatio() const noexcept;

    /** @brief Binds the target this area is presented through (its window), or unbinds it with nullptr.
     *
     * THE WINDOW BELONGS TO THE AREA, and the TARGET is what names it: the area holds the object its DEVICE made
     * for that window (RenderDevice::createSurfaceTarget, which is where a device that presents nothing refuses
     * and says why), and everything the window half is told - a recreated handle, the size, the frame's present -
     * goes to that object (see WindowPresentTarget). The engine, which drives the frame, reads the target from
     * here (it owns no handle of its own, see RenderEngine).
     *
     * RE-ANNOUNCING MOVES A WINDOW, IT DOES NOT REBIND IT. The platform recreating the window (a reparent, a
     * screen change) is the same area on a new handle: a host announces it ON THE TARGET
     * (WindowPresentTarget::setWindowHandle), which is what moves an established session onto the new window while
     * the device, its caches and every compiled pipeline stay. Binding a DIFFERENT target instead is a different
     * window: the previous one is left behind (its device is what owns it - see RenderDevice::shutdown).
     *
     * nullptr unbinds: the area presents nothing from here on, while the session stays up. A boundary case a host
     * sets up before its device exists is simply kept: the target is an object, so nothing has to take it. The
     * ENGINE unbinds on shutdown(), before it takes the device down (see RenderEngine::shutdown): what the device
     * handed out lives until the DEVICE is destroyed or the area is unbound, so a window closing is the device's
     * business and never leaves this pointer dangling.
     *
     * @param target The target this area is presented through (from its device), or nullptr to unbind. BORROWED:
     *               the device that made it owns it, and it lives until that device is destroyed or this area is
     *               unbound (the engine unbinds it on shutdown - see RenderEngine::shutdown).
     */
    void bindTarget(raw_ptr<WindowPresentTarget> target);

    /** @brief Gets the target this area is presented through, or nullptr when it is bound to none.
     *
     * The question a host asks to decide whether to re-announce: the native handle the session was moved onto is
     * the TARGET's own answer (WindowPresentTarget::nativeHandle), so comparing it with what the windowing system
     * reports now is how "the platform recreated my window" is noticed.
     *
     * @return The bound target, or nullptr.
     */
    [[nodiscard]] WindowPresentTarget* target() const noexcept;

    /** @brief Whether this area is bound to a window (see bindTarget).
     *
     * @return true when a target is bound to this area.
     */
    [[nodiscard]] bool hasTarget() const noexcept;

  private:
    friend class SceneView;  // reads the size it lays its geometry out with (see sceneWidth/sceneHeight)

    /** @brief The pass a presented view draws this surface through: the window pass of its live pipeline.
     *
     * @param view View to resolve.
     * @return The pass, or nullptr when there is none to resolve (no pipeline, no window pass).
     */
    raw_ptr<RenderPass> passOf(raw_ptr<SceneView> view) const;

    /** @brief Reports the one refusal an area can make, once per episode. */
    void reportSecondPresentation();

    raw_ptr<RenderEngine>          engine_;
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
