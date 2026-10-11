#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <optional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "RenderDiagnostic.hpp"
#include "RenderSurfaceTarget.hpp"
#include "ReportOnce.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
class SceneView;

/**
 * @brief The presentation table of ONE render surface: which passes draw into it, and in which order.
 *
 * A surface is the thing a picture ends up in (the host's native window, a swapchain). Which passes draw into it
 * is a fact about the SURFACE, not about any single pass - and it has exactly one owner, this object: a host
 * declares its presentations here, and this is the answer to "who draws on this surface, first to last".
 *
 * That matters because "who presents" used to be inferred: a view asked the engine whether ANY registered pass
 * carried its camera with a null render target (RenderEngine::hasWindowPass) and stood down when one did - a
 * global, order-sensitive guess with no owner, which silently dropped the SECOND view sharing a camera and could
 * not tell "this pass draws HERE" from "this pass exists". A host that builds its own main pass now says so:
 *
 * @code
 * control->surface()->present(pipeline->windowPass());   // this pass presents this surface
 * @endcode
 *
 * and the view's default window pass is only built when nobody declared anything (see present(SceneView&)):
 *
 * @code
 * if (!control->surface()->hasPresentation()) {
 *     control->surface()->present(*control->view());     // the fallback: my view presents this surface
 * }
 * @endcode
 *
 * The table is ordered by the ENGINE's schedule (presentations()): the first entry is the pass that draws first
 * on this surface. Ties - two presentations at the same pass order - keep declaration order.
 *
 * The clear belongs here too, and for the same reason: the backend applies the FIRST clear announcement per
 * attachment and silently drops the rest, so "two views on one surface both clear" used to mean "the second pass'
 * clearColor() is not what the picture shows" - decided by pass registration order, told to nobody. This table
 * gives that decision an owner: the FIRST presentation announces the clear, and every other one is told to stop
 * claiming to clear (RenderPass::setClearEnabled(false)). What the surface owns is the DECISION (who clears); the
 * colour and the depth stay on the pass that asked for them, so there is still one copy of them (the same rule the
 * HUD switches follow - see appfw-render-view.md §7 invariant 15). Each presentation's own ask is remembered, so a
 * presentation that becomes the first one later (the one ahead of it was dismissed) clears the way it asked to,
 * and a recipe whose window pass does not clear at all (the Deferred present pass draws over the whole surface) is
 * not forced to start clearing by being first.
 *
 * THE WINDOW IS THE AREA'S TOO. What this area is presented through - the native window the host chose (the
 * TARGET, in this design's vocabulary) - is a fact about the AREA, exactly like the size: bindTarget() holds the
 * object the DEVICE made for that window (a device that presents nothing refuses there, once, instead of
 * pretending to attach - see RenderDevice::createSurfaceTarget), resize() is the ONE size announcement (the views,
 * the frame the passes lay out on, and the target's own window half), and a recreated window is announced on that
 * target (which MOVES an established session instead of rebuilding it: the device and every compiled pipeline stay
 * - see RenderSurfaceTarget::setWindowHandle). The engine drives the frame and owns no handle of its own (see
 * RenderEngine).
 *
 * @note Splitting a surface between several presentations (each with its own rectangle) is the same table read
 * again, and the rectangle per presentation belongs here too: what a surface is divided into is a fact about the
 * SURFACE (a host states the split once, in normalized terms, and the surface answers "who owns this point" -
 * see viewAt). The view that presents through an entry keeps drawing into the rectangle the entry states: it reads
 * it here instead of holding a second copy (see SceneView::viewport).
 */
class VN_GRAPHICS_API RenderSurface : public RefCounted<RenderSurface> {
  public:
    /** @brief One presentation: the pass that draws this surface, and the view it was declared for.
     *
     * `pass` is the pass the engine will run - the window pass of the entry's view (resolved when the table is
     * read, so a rebuilt pipeline is followed), or the pass a host declared itself. `view` is null for a pass a
     * host declared (nothing about that presentation belongs to a view).
     */
    struct Presentation {
        /// The pass that draws this surface.
        raw_ptr<RenderPass> pass = nullptr;
        /// The view this presentation came from, or null when a host declared the pass.
        raw_ptr<SceneView> view = nullptr;
        /// The area this presentation draws into (normalized), for a presentation that HAS a view.
        ///
        /// Empty for a pass a host declared: a rectangle in normalized terms is a statement relative to the
        /// surface's size, and a pass is not fed that size (the views are, see resize) - such a pass states its
        /// own rectangle in device pixels instead (RenderPass::setViewport), which is the host's to own because
        /// the host is the one that knows the pixels.
        std::optional<NormalizedViewport> rect{};
    };

  public:
    /** @brief Creates the presentation table of one surface rendered through @p engine.
     *
     * @param engine Engine the presentations are registered on, and the one their order is read from (borrowed;
     *               must outlive this surface). Null is accepted and presents nothing: find the engine first.
     */
    explicit RenderSurface(raw_ptr<RenderEngine> engine);

    /** @brief Destroys the table.
     *
     * The presentations are declarations, not owners: the passes stay registered on the engine and the views keep
     * their pipelines. What does happen is that the views stop naming this surface - it is going away, and a view
     * that outlives it must not reach back into it (see SceneView::~SceneView).
     */
    ~RenderSurface();

  public:
    /** @brief Declares that @p view draws this surface, into the rectangle @p view already draws into.
     *
     * The declaration a host makes when it has no main pass of its own: this surface is presented by the pass of
     * the pipeline the VIEW adopted (SceneView::setPipeline). The table never builds one - it resolves the pass
     * back through the view - so a view with no pipeline is refused and reported: a declaration without one would
     * leave the host believing a window is being drawn into while nothing draws. Declaring the same view twice is
     * one presentation. A view that draws through ANOTHER engine (or none) is refused and reported: its passes are
     * registered on that engine's frame, so it could never present this surface. A view that already presents
     * ANOTHER surface is refused for the same reason (one view draws one picture).
     *
     * The view's current rectangle (SceneView::viewport) becomes this presentation's rectangle: a view that was
     * already drawing into part of the surface keeps that part once it is presented. Use the overload below to
     * state a different one.
     *
     * @param view View to present; must have adopted a pipeline, be bound to this surface's engine, and present no
     *             other surface.
     * @return true when this added a presentation; false when @p view already presented this surface, or could
     *         not.
     */
    bool present(SceneView& view);

    /** @brief Declares that @p view draws @p rect of this surface.
     *
     * The whole point of a split: a host says once, in normalized terms, where each presentation draws ("the left
     * half", "the corner preview"), and the rectangle lives HERE - with the clear and the order, in the one place
     * that describes what this surface is made of. The view derives everything it needs from it (its window pass'
     * device rectangle, its off-screen targets, its HUD boxes, the shape its picture is projected with), so
     * there is no second statement to keep in step.
     *
     * @param view View to present; same requirements as the overload above.
     * @param rect The area it draws into (normalized); an invalid rectangle means the whole surface.
     * @return true when this added a presentation; false when @p view already presented this surface, or could
     *         not.
     */
    bool present(SceneView& view, const NormalizedViewport& rect);

    /** @brief Declares that @p pass draws this surface.
     *
     * The declaration a host makes when it built its own main pass (an application pipeline presenting this
     * surface): the view's default window pass is not built while this declaration stands, so the host keeps full
     * control. Declaring the same pass twice is one presentation.
     *
     * The pass is NOT adopted: the pipeline that built it owns it (and the engine holds it while registered).
     * This table keeps its own reference so what it reports cannot outlive what it describes.
     *
     * @param pass Pass that presents this surface; null is a no-op.
     * @return true when this added a presentation; false when @p pass already presented this surface.
     */
    bool present(raw_ptr<RenderPass> pass);

    /** @brief States the area of this surface that @p view draws into (normalized).
     *
     * Geometry, not recipe: nothing is rebuilt. The view is told (it re-derives the numbers that follow from its
     * rectangle - its window pass' device viewport, its off-screen targets, its HUD boxes) and so is the shape its
     * picture is projected with, because that follows from the same rectangle where the pass is recorded (the
     * camera the pass draws through carries the shape, see Camera::setAspect).
     *
     * Same call as SceneView::setViewport, which is the spelling a host that thinks in views uses; this one is the
     * spelling for a host that thinks in terms of the surface's layout - it is the same rectangle either way.
     *
     * @param view View whose area to state; must present this surface.
     * @param rect The area it draws into (normalized); an invalid rectangle means the whole surface.
     * @return true when @p view presents this surface (its area was stated); false when it does not.
     */
    bool setViewport(raw_ptr<SceneView> view, const NormalizedViewport& rect);

    /** @brief Drops the presentation @p view declared.
     *
     * The view keeps its pipeline (that is the view's), and the passes stay registered: what leaves is the
     * statement that this surface is theirs. The rectangle goes with the view - it keeps drawing into the same
     * area, which is now its own statement rather than this surface's.
     *
     * @param view View whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<SceneView> view);

    /** @brief Drops the presentation @p pass declared (see dismiss(SceneView&)).
     *
     * @param pass Pass whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<RenderPass> pass);

    /** @brief Whether anything declares this surface.
     *
     * The question the default-path host asks before falling back to its own view (see the class docs).
     *
     * @return true when this surface has at least one presentation.
     */
    [[nodiscard]] bool hasPresentation() const noexcept;

    /** @brief Records the size of this surface, keeping the pixel ratio it last saw.
     *
     * @param width  Surface width in logical pixels (the unit the host reports; <= 0 is ignored).
     * @param height Surface height in logical pixels (<= 0 is ignored).
     */
    void resize(int width, int height);

    /** @brief Records the size of this surface and gives it to every view that presents it.
     *
     * The size of the thing a picture ends up in is a fact about the SURFACE, like the order and the clear: a
     * host reports it ONCE (its window knows it - see SurfaceWindow), and this table fans it out to every
     * presented view, each of which derives its own rectangle from it (SceneView::onSurfaceResized is the one
     * step, and a view never holds a second copy of the surface's size). A view presented LATER receives the
     * size remembered here at once, so a pane added to a running surface is sized without the host feeding it.
     *
     * The same ONE announcement serves the frame: the engine records it in the frame context every pass lays
     * itself out on, and the window half of it - the swapchain, what hangs off the extent - is the TARGET's own
     * call, made here (see RenderSurfaceTarget::resize - the engine owns no size of its own, the area owns the
     * target).
     *
     * Presentation is the unit: a pass a host declared itself is not fed - it has no view, and such a pass
     * states its own rectangle in device pixels (see Presentation::rect).
     *
     * @param width        Surface width in logical pixels (<= 0 is ignored, with the ratio).
     * @param height       Surface height in logical pixels (<= 0 is ignored, with the ratio).
     * @param pixel_ratio  Device pixels per logical pixel; <= 0 keeps the ratio this surface last saw.
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Binds the target this area is presented through (its window), or unbinds it with nullptr.
     *
     * THE WINDOW BELONGS TO THE AREA, and the TARGET is what names it: the area holds the object its DEVICE made
     * for that window (RenderDevice::createSurfaceTarget, which is where a device that presents nothing refuses
     * and says why), and everything the window half is told - a recreated handle, the size, the frame's present -
     * goes to that object (see RenderSurfaceTarget). The engine, which drives the frame, reads the target from
     * here (it owns no handle of its own, see RenderEngine).
     *
     * RE-ANNOUNCING MOVES A WINDOW, IT DOES NOT REBIND IT. The platform recreating the window (a reparent, a
     * screen change) is the same area on a new handle: a host announces it ON THE TARGET
     * (RenderSurfaceTarget::setWindowHandle), which is what moves an established session onto the new window while
     * the device, its caches and every compiled pipeline stay. Binding a DIFFERENT target instead is a different
     * window: the previous one is left behind (its device is what owns it - see RenderDevice::shutdown).
     *
     * nullptr unbinds: the area presents nothing from here on, while the session stays up. A boundary case a host
     * sets up before its device exists is simply kept: the target is an object, so nothing has to take it.
     *
     * @param target The target this area is presented through (from its device), or nullptr to unbind. BORROWED:
     *               the device that made it owns it, and it must outlive the area's use of it.
     */
    void bindTarget(raw_ptr<RenderSurfaceTarget> target);

    /** @brief Gets the target this area is presented through, or nullptr when it is bound to none.
     *
     * The question a host asks to decide whether to re-announce: the native handle the session was moved onto is
     * the TARGET's own answer (RenderSurfaceTarget::nativeHandle), so comparing it with what the windowing system
     * reports now is how "the platform recreated my window" is noticed.
     *
     * @return The bound target, or nullptr.
     */
    [[nodiscard]] RenderSurfaceTarget* target() const noexcept;

    /** @brief Whether this area is bound to a window (see bindTarget).
     *
     * @return true when a target is bound to this area.
     */
    [[nodiscard]] bool hasTarget() const noexcept;

    /** @brief Gets the device pixel ratio this surface last saw (resize).
     *
     * The ratio is a fact about the surface, like its size: a reader that lays something out in device pixels
     * (an overlay, a preview rectangle) reads it HERE rather than holding a second copy that could drift.
     *
     * @return Device pixels per logical pixel; 1.0 until a host reports one.
     */
    [[nodiscard]] double pixelRatio() const noexcept;

    /** @brief The presentations of this surface, in the order the engine runs them.
     *
     * The answer describes the frame the engine would draw NOW, not what was once declared: a presentation whose
     * pass the engine no longer has (the pipeline that built it was dropped, the host removed the pass) is left
     * out. An entry whose view has no window pass yet is left out for the same reason.
     *
     * @return The presentations, first-drawn first (empty when nothing presents this surface).
     */
    [[nodiscard]] std::vector<Presentation> presentations() const;

    /** @brief Gets the view that draws the point (@p nx, @p ny) of this surface, or null.
     *
     * The default answer to "which view is the user pointing at": a point belongs to the presentation whose
     * rectangle contains it. Coordinates are the surface's own, in the same normalized terms the rectangles are
     * stated in - a host that has device pixels (a mouse event) divides them by the surface size it reported
     * (see resize), which is the size this table fanned out. A host-declared pass is not a view and is never
     * the answer.
     *
     * Where presentations overlap, the one drawn LAST wins: that is the picture the point is actually on.
     *
     * @param nx Point x as a fraction of the surface width.
     * @param ny Point y as a fraction of the surface height.
     * @return The view that owns the point, or null when none does (outside every rectangle, or no views here).
     */
    [[nodiscard]] raw_ptr<SceneView> viewAt(double nx, double ny) const;

    /** @brief Says that input went to @p view: this surface's FOCUS.
     *
     * A press is what decides where the user's attention is, so the path that routed the event says so here - and
     * this table answers the two questions that follow: which view a later event without a position belongs to
     * (a key), and which view a command about "the current view" means. Same reasoning as viewAt: what a surface
     * is divided into, and which piece of it the user is working in, are facts about the SURFACE.
     *
     * Only a view that PRESENTS this surface can hold the focus (a view is focused in the pane it draws), so a
     * view that presents nothing here is ignored rather than remembered. Null clears the explicit focus, which
     * makes focusedView() answer its default again (the first presentation).
     *
     * @param view View the input went to, or null to clear.
     * @return true when @p view presents this surface (the focus is now it, or cleared for null); false when it
     *         does not.
     */
    bool focus(raw_ptr<SceneView> view);

    /** @brief The presentation the input went to, or the first one when nothing has been focused yet.
     *
     * The default is the first presentation - the pass that draws first, which for the whole-surface case is the
     * only one - so a surface that never saw a press still answers "which view is this" (and a split answers with
     * its first pane rather than nothing). A presentation that was dismissed, or whose pass left the frame, is not
     * the answer: what this reports is a presentation of the frame the engine would draw now.
     *
     * @return The focused view, or null when no presentation of this surface has a view.
     */
    [[nodiscard]] raw_ptr<SceneView> focusedView() const;

  private:
    friend class SceneView;  // reads the size and the rectangle it draws into, and re-states the surface's clear after a rebuild

    /** @brief One declared presentation: a view (its window pass is resolved when the table is read) or a pass a
     * host declared itself.
     */
    struct Entry {
        raw_ptr<SceneView>        view = nullptr;
        intrusive_ptr<RenderPass> pass;
        /// The area this entry draws into (normalized). A view presentation always has one (its default is the
        /// whole surface); a pass a host declared has none - a normalized rectangle would have to be converted
        /// against a surface size this table does not give a pass (see Presentation::rect).
        NormalizedViewport        rect{};
        /// The pass this entry was last seen with, and what IT asked for (clears? which colour? depth?). A new pass
        /// means a fresh ask (a rebuild rebuilds its defaults) - and the surface needs the ask it remembered,
        /// because the surface is the one that takes clearing away from everything but the first presentation.
        intrusive_ptr<RenderPass> asked_of;
        bool                      clears = false;
    };

    /** @brief One entry that is in the frame right now, with the pass it resolves to and where the engine runs it.
     *
     * An index rather than a pointer: presentations() is const and answers with copies, while the clear rule writes
     * to the entry it is looking at.
     */
    struct Resolved {
        std::size_t         index = 0;
        raw_ptr<RenderPass> pass = nullptr;
        int                 order = 0;
    };

    /** @brief The entries the engine would draw now, in the order it runs them (a stale entry is left out).
     *
     * @return The resolved entries, first-drawn first.
     */
    [[nodiscard]] std::vector<Resolved> resolvedEntries() const;

    /** @brief The pass an entry presents now: its declared pass, or the window pass of its view's live pipeline.
     *
     * @param entry Entry to resolve.
     * @return The pass, or nullptr when there is none to resolve (no pipeline yet, no window pass).
     */
    raw_ptr<RenderPass> passOf(const Entry& entry) const;

    /** @brief The area @p view draws into, as this surface states it.
     *
     * The rectangle a presented view reads while it presents this surface (SceneView::viewport): the surface's
     * table is the ONE place the split is stated, so the view holds no second copy of it. A view that presents
     * nothing here answers with the whole surface.
     *
     * @param view View to look up.
     * @return The normalized rectangle, or the whole surface when @p view is not in the table.
     */
    [[nodiscard]] NormalizedViewport viewportOf(raw_ptr<const SceneView> view) const noexcept;

    /** @brief Puts the ONE clear of this surface on the pass that draws first, and off every other one.
     *
     * Called whenever the table or the order it is read in can have moved: a presentation was added or dismissed,
     * and a presented view adopted another pipeline (its window pass is a new pass, created ENABLED - see
     * SceneView::setPipeline, which calls this through the friendship above).
     */
    void applyClear();

    /// The engine the presentations are registered on and ordered by (borrowed; must outlive this surface).
    raw_ptr<RenderEngine> engine_;
    /// The target this area is presented through (see bindTarget): the area's own fact, made by its device and
    /// handed to it (BORROWED - the device owns it). nullptr = no window bound (a frame run for this area presents
    /// nothing).
    raw_ptr<RenderSurfaceTarget> target_ = nullptr;
    /// The size of this surface as the host reported it (resize), and the ratio that came with it: what every
    /// presented view derives its rectangle from. Zero until a host reports one.
    int    width_ = 0;
    int    height_ = 0;
    double pixel_ratio_ = 1.0;
    /// The table, in declaration order (presentations() answers in engine order).
    std::vector<Entry> entries_;
    /// The view the input went to (borrowed, and only while it presents this surface): null = nothing focused yet,
    /// so focusedView() answers with the first presentation.
    raw_ptr<SceneView> focused_ = nullptr;
    /// "A declaration was refused" is one condition, not a per-call event (see present(SceneView&)).
    ReportOnce refused_;
};

VN_GRAPHICS_NS_END
