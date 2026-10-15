#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/window/window_global.hpp>

#include "Pipeline.hpp"

VN_WINDOW_NS_BEGIN
struct KeyEvent;
struct MouseEvent;
struct ScrollEvent;
VN_WINDOW_NS_END

VN_GRAPHICS_NS_BEGIN

class Camera;
class CameraManipulator;
class Pipeline;
class RenderEngine;
class RenderPass;
class RenderSurface;
class Scene;

/**
 * @brief Host-agnostic interactive primary view: camera + content + navigation.
 *
 * @details
 * - The view owns WHAT is drawn (a Camera and a content Scene) and WHERE (the whole area it presents - see
 *   RenderSurface); WHICH passes draw it is a Pipeline it ADOPTS (setPipeline), built by RenderPipelineBuilder -
 *   so a shading mode or overlay change is a call on the pipeline (Pipeline::setShadingMode / setGizmoVisible),
 *   never a view rebuild, and a view with no pipeline draws nothing.
 * - It borrows an engine (setEngine) rather than being one: the engine stays content- and camera-agnostic, while
 *   the view owns the primary view state - camera, content, manipulator, and the projection aspect kept in step
 *   with the surface.
 * - Navigation: a CameraManipulator (default: an orbit one bound to this view's camera and scene, created
 *   lazily) receives the window input the host pushes (pushEvent).
 * - The public surface is grouped by role, in this order: CAMERA AND CONTENT, NAVIGATION, GEOMETRY, INPUT, then
 *   PRESENTATION.
 * - Several views may share one engine, but each area draws ONE of them (one area, one picture). A host wanting
 *   several pictures at once owns the layout that produces them - its own passes with their own sub-viewports
 *   (RenderPass::setViewport) and clear policy; the engine never manages layout.
 *
 * @note
 * - Whether a pass presents the surface is the SURFACE's declaration (RenderSurface::present), never a guess: a
 *   view is not suppressed because some pass elsewhere happens to carry its camera.
 * - setSurface and applySurfaceGeometry are NOT host API - a presentation drives them, and a host calling one by
 *   hand would make the surface believe in a view nobody declared.
 */
class VN_GRAPHICS_API SceneView : public RefCounted<SceneView> {
    friend class RenderSurface;  // binds this view to the area it presents, and hands its size back on unbind

  public:
    /** @brief Constructs a view with a default camera and an empty content scene.
     *
     * The view borrows no engine until setEngine(), and draws nothing until a pipeline is adopted (setPipeline).
     */
    SceneView();

    /** @brief Destroys the view: the adopted pipeline is dropped (its destructor unregisters its passes) and the
     * surface stops naming it. The bound engine must outlive the view.
     */
    ~SceneView();

  public:
    // ---- Camera and content ----

    /** @brief Gets the view camera (non-owning). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the content scene drawn by this view.
     *
     * @details
     * - The reference shares ownership with the view, so it can be bound explicitly to engine passes
     *   (addPass(pass, content, order)).
     *
     * @return The content scene (never null; an empty scene draws nothing).
     */
    intrusive_ptr<Scene> scene() const;

    /** @brief Sets the content scene drawn by this view (the view keeps a reference).
     *
     * @param scene Content scene, or null to clear (draws nothing).
     */
    void setScene(intrusive_ptr<Scene> scene);

  public:
    // ---- Navigation ----

    /** @brief Gets the camera manipulator driving this view's camera.
     *
     * @details
     * - A default orbit manipulator is created on first access when none was set (setManipulator); creation is
     *   lazy so it snapshots its home view from the camera's final placement.
     *
     * @return The manipulator (never null).
     */
    raw_ptr<CameraManipulator> manipulator();

    /** @brief Sets the camera manipulator driving this view's camera.
     *
     * @param manipulator Manipulator bound to this view's camera; the view keeps a reference.
     */
    void setManipulator(intrusive_ptr<CameraManipulator> manipulator);

    /** @brief Fits the content scene into the view.
     *
     * @return true when a scene with valid bounds was fitted.
     */
    bool fitToScreen();

    /** @brief Restores the view captured as home by the manipulator. */
    void home();

  public:
    // ---- Geometry ----

    /** @brief Refreshes this view's geometry for a new surface size (keeps the current pixel ratio).
     *
     * @param width  New surface width in logical pixels, as the host reports it (<= 0 is ignored).
     * @param height New surface height in logical pixels (<= 0 is ignored).
     */
    void resize(int width, int height);

    /** @brief Refreshes this view's geometry for a new surface size and device pixel ratio.
     *
     * @details
     * - The size is the whole surface the host renders into: this view's area IS that surface (one area draws one
     *   picture - see RenderSurface), so there is no second rectangle to state.
     * - What the one size becomes, in order: the camera aspect and the manipulator's resize, the adopted
     *   pipeline's own resize (its off-screen targets and HUD boxes - a view drawing through a built pipeline
     *   needs no layout step at all), then every callback registered with addSurfaceLayout().
     * - A view that PRESENTS a surface forwards this to RenderSurface::resize (the size of the thing the picture
     *   ends up in is the SURFACE's fact - recorded there and fanned out), so this entry is the one for a view
     *   that presents nothing; a view that presents later is sized at once from what the surface remembers.
     * - The verb matches RenderSurface::resize on purpose: the SAME announcement, addressed to whichever of the
     *   two a host holds.
     *
     * @param width        New surface width in logical pixels (<= 0 is ignored).
     * @param height       New surface height in logical pixels (<= 0 is ignored).
     * @param pixel_ratio  Device pixels per logical pixel (devicePixelRatio); <= 0 keeps the current one.
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Registers a creator-managed surface-layout step.
     *
     * @details
     * - The callback receives the new surface size on every surface change and updates whatever its creator owns
     *   (an off-screen target's size, a pass's viewport - pass->setViewport(Viewport{...})); this keeps each
     *   pipeline's resize policy with its creator (deferred chains at native / fractional resolution, fixed
     *   shadow maps doing nothing, corner-anchored previews re-anchoring) while the engine stays a pure
     *   scheduler.
     * - The size is in the space the HOST reports (for the Qt host, the logical widget size - see RenderControl),
     *   but a pass viewport wants DEVICE pixels: scale by the ratio in the callback (see the ratio overload and
     *   Pipeline::resize, which takes the ratio for exactly that reason; AxisGizmo / FpsOverlay lay out
     *   logically and apply the ratio themselves).
     *
     * @param layout Callback taking the new surface width and height the host reported.
     */
    void addSurfaceLayout(std::function<void(int width, int height)> layout);

    /** @brief Registers a creator-managed surface-layout step that also wants the device pixel ratio.
     *
     * @details
     * - Same as the two-argument overload plus the ratio: the consumer that needs it lays out in DEVICE pixels
     *   (`pass->setViewport(Viewport{...})`, an off-screen target at native resolution), and multiplying here
     *   is what keeps a layer-1 host from reaching back into appfw for RenderControl::devicePixelRatio().
     * - The size is again the AREA's size (one area draws one picture), in the host's logical space.
     *
     * @param layout Callback taking the view's width, height and the host's pixel ratio.
     */
    void addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout);

    /** @brief Removes all registered surface-layout steps. */
    void clearSurfaceLayouts();

    /** @brief The surface width (logical px) this view derives its picture from.
     *
     * @details
     * - The SURFACE's while this view presents one, this view's own announcement otherwise (see resize): the
     *   size has one owner, and a presenting view asks it rather than holding a second copy that could drift.
     *
     * @return Surface width in logical pixels, 0 when nobody has sized anything yet.
     */
    [[nodiscard]] int surfaceWidth() const noexcept;

    /** @brief The surface height (logical px) this view derives its picture from (see surfaceWidth). */
    [[nodiscard]] int surfaceHeight() const noexcept;

    /** @brief The device pixel ratio this view derives its picture from (see surfaceWidth). */
    [[nodiscard]] double surfacePixelRatio() const noexcept;

  public:
    // ---- Input ----

    /** @brief Pushes a mouse event to the manipulator.
     *
     * @param event Mouse event to forward.
     */
    void pushEvent(const vn::window::MouseEvent& event);

    /** @brief Pushes a scroll (wheel) event to the manipulator.
     *
     * @param event Scroll event to forward.
     */
    void pushEvent(const vn::window::ScrollEvent& event);

    /** @brief Pushes a keyboard event to the manipulator.
     *
     * @param event Key event to forward.
     */
    void pushEvent(const vn::window::KeyEvent& event);

  public:
    // ---- Presentation ----

    /** @brief Binds the engine this view renders through (borrowed; must outlive the view; null detaches).
     *
     * @details
     * - Re-binding replaces any previous engine, and the adopted pipeline is dropped with the old one (its
     *   passes live in the old engine's frame): adopt a pipeline built on the new engine to keep drawing.
     * - The engine stays content-free; this view binds its content to the passes it registers.
     *
     * @param engine Engine to borrow.
     */
    void setEngine(raw_ptr<RenderEngine> engine);

    /** @brief Gets the bound engine, or nullptr when unset. */
    raw_ptr<RenderEngine> engine() const;

    /** @brief Gets the pipeline this view adopted (setPipeline), or nullptr - which means this view draws
     * nothing.
     *
     * @details
     * - The handle for inspection and micro-tuning: `windowPass()` (what presents this view), `gizmoPass()` /
     *   `fpsOverlayPass()` (their state IS the overlay's visibility), `gizmo()` / `fpsOverlay()` (the content
     *   objects), `offscreenTarget()`, `setShadingMode()`.
     * - The view shares ownership of it, so it stays alive while the view draws through it; letting it go is
     *   what removes its passes from the frame.
     *
     * @return The adopted pipeline, or nullptr when this view has none.
     */
    raw_ptr<Pipeline> pipeline() const;

    /** @brief Adopts @p pipeline as what draws this view (build one with RenderPipelineBuilder).
     *
     * @details
     * - THE one step connecting a view to a drawing: the view brings what a pipeline cannot know (its camera and
     *   content are the builder's inputs; the area's size and this view's layout steps are applied here), and
     *   the pipeline brings what a view must not own (which passes exist, the shading mode, the overlays).
     * - Adopting replaces any previous pipeline; null drops the current one, which takes the view out of the
     *   frame (the pipeline unregisters its passes when its last handle goes away). A presenting view keeps
     *   being one across a change: the area resolves the view's pass afresh, so a REBUILT pipeline (a recipe
     *   change) is followed without re-declaring anything.
     * - The view is drawn where its area is - the whole of it. It is the CALLER's business to have built a
     *   pipeline for this view: one built for another camera draws that camera's picture into this view's area
     *   (the engine reports the shape mismatch when the two disagree).
     *
     * @param pipeline Pipeline that draws this view; null drops the current one.
     */
    void setPipeline(intrusive_ptr<Pipeline> pipeline);

    /** @brief Gets whether this view presents a surface (RenderSurface::present bound it to one).
     *
     * @details
     * - The question a host with its own surface and view asks: the surface feeds every view that presents it
     *   (RenderSurface::resize), so a presenting view needs nothing else - a view that is not one of its
     *   presentations is the only one left to feed by hand.
     *
     * @return true when this view presents a surface.
     */
    [[nodiscard]] bool hasSurface() const noexcept;

  private:
    /** @brief Binds this view to the surface it presents, or unbinds it (RenderSurface::present / dismiss).
     *
     * @details
     * - Not host API: the door is RenderSurface::present, and this pointer is what that table uses to find this
     *   view's window pass again - a host setting it by hand would make the surface believe in a presentation
     *   nobody declared.
     * - The pair is bound both ways (the surface names the view, the view names the surface) and both borrowed:
     *   either one going away unbinds both.
     *
     * @param surface Surface this view presents (borrowed), or null when it presents none.
     */
    void setSurface(raw_ptr<RenderSurface> surface) noexcept;

    /** @brief Records the surface size (and ratio) a view with NO surface derives from.
     *
     * @details
     * - The step resize() takes when this view presents nothing; a presenting view reads the surface's numbers
     *   instead (see surfaceWidth - the size has ONE owner and that is the surface, RenderSurface::resize).
     *
     * @param width        Surface width in logical pixels.
     * @param height       Surface height in logical pixels.
     * @param pixel_ratio  Device pixels per logical pixel; <= 0 keeps the current one.
     */
    void setOwnSurfaceSize(int width, int height, double pixel_ratio);

    /** @brief Turns {surface size, pixel ratio} into this view's picture, in one place.
     *
     * @details
     * - The step RenderSurface::resize fans out to the presenting view (the view is what a size is FOR), and the
     *   step this view runs on its own when it presents nothing; not host API either way - the announcement
     *   that leads here is resize() / RenderSurface::resize.
     */
    void applySurfaceGeometry();

    // The bound engine is borrowed (the host owns it and keeps it alive).
    raw_ptr<RenderEngine> engine_ = nullptr;

    // The surface this view presents, while one says so (RenderSurface::present). Borrowed, and unbound from
    // either side: the surface must not be reached once it is gone, and it must stop naming this view when the
    // view goes (see ~SceneView and ~RenderSurface).
    raw_ptr<RenderSurface> surface_ = nullptr;

    // The size this view lays its picture out with while it presents NO surface (the host's announcement). A view
    // that presents one reads the SURFACE's numbers instead (see surfaceWidth / RenderSurface::resize): the size of
    // the thing the picture ends up in has one owner, and it is not the view.
    int    own_width_       = 0;
    int    own_height_      = 0;
    double own_pixel_ratio_ = 1.0;

    intrusive_ptr<Camera>  camera_;
    intrusive_ptr<Scene>   scene_;
    intrusive_ptr<CameraManipulator> manipulator_;

    // The pipeline this view adopted, if any (setPipeline): what draws this view's camera and content into the
    // area. The handles in it are the pipeline's own - this one only keeps it alive, and dropping it is what
    // takes the passes out of the frame.
    intrusive_ptr<Pipeline> pipeline_;

    // Creator-managed surface-layout steps run on every surface resize.
    std::vector<std::function<void(int width, int height)>> surface_layouts_;
    // ... and the ones that asked for the ratio too.
    std::vector<std::function<void(int width, int height, double pixel_ratio)>> surface_layouts_with_ratio_;
};

VN_GRAPHICS_NS_END
