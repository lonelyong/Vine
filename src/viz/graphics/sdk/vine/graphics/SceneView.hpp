#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/window/window_global.hpp>

#include "RenderPipeline.hpp"

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
 * SceneView bundles everything that makes up one interactive 3D view into a
 * single host-independent object:
 *
 *   - a Camera (the view / projection of this view),
 *   - a content Scene (the root drawn by this view),
 *   - a CameraManipulator (default: an orbit manipulator bound to the camera
 *     and scene) that receives window input, and
 *   - viewport bookkeeping: it keeps the camera projection aspect in step with
 *     the rendering surface as the host reports resizes.
 *
 * A SceneView is not a RenderEngine. It borrows one (setEngine): the engine
 * remains the pure pass scheduler / backend owner, while the view owns the
 * "primary view state" a camera used to be given by the engine. The engine
 * holds no content scene, and neither does this class hold a DRAWING RECIPE: a
 * view is WHAT is drawn (its camera and content) and WHERE (the whole surface it
 * presents - its area, see RenderSurface), while WHICH passes draw it is a
 * Pipeline it ADOPTS (setPipeline) - built by RenderPipelineBuilder, whose
 * PipelineOptions carry the recipe (chain, shading mode, overlays). That split
 * is why switching a shading mode or hiding an overlay is a call on the
 * pipeline (Pipeline::setShadingMode / setGizmoVisible) and never something a
 * view rebuilds, and why a view with no pipeline simply draws nothing.
 *
 * Whether a pass presents a SURFACE is the surface's own declaration
 * (RenderSurface): a host whose own main pass presents the surface says so
 * THERE (present(pass)), and a view is never suppressed because some pass
 * elsewhere happens to carry its camera.
 *
 * Several views may share one engine, and each area draws ONE of them (see RenderSurface): one area, one picture.
 * A host that wants more than one picture at a time owns the layout that produces them - its own passes with their
 * own sub-viewports (RenderPass::setViewport) and their own clear policy - and the engine never manages layout.
 *
 * FIVE ROLES, ONE CLASS. The public surface is grouped by what a method is FOR, in this order: CAMERA AND
 * CONTENT (what it draws), NAVIGATION (the manipulator and the two camera moves), GEOMETRY (the surface size it
 * derives its picture from, and the layout steps told about it) and INPUT (the window events it forwards), then
 * PRESENTATION (the engine it borrows, the pipeline it adopts, the surface it presents through, and the frame
 * drive).
 *
 * TWO OF ITS STEPS ARE NOT HOST API - a presentation drives them, and a host calling one by hand would make that
 * presentation believe in a view nobody declared:
 *   - setSurface: binds this view to the area it presents (RenderSurface::present / dismiss);
 *   - applySurfaceGeometry: re-derives its picture after the surface size changed (RenderSurface::resize).
 *
 * The engine stays camera- and content-agnostic: it forwards no mouse /
 * scroll / key input, holds no camera and no content scene. The host pushes
 * window input and surface resizes to the view (pushEvent /
 * resize), which forwards them to the manipulator and keeps the
 * camera projection correct.
 */
class VN_GRAPHICS_API SceneView : public RefCounted<SceneView> {
    friend class RenderSurface;  // binds this view to the area it presents, and hands its size back on unbind

  public:
    /** @brief Constructs a view with a default camera and an empty content
     * scene.
     *
     * The view is not attached to an engine until setEngine() is called, and it
     * draws nothing until a pipeline is adopted (setPipeline).
     */
    SceneView();

    /** @brief Destroys the view, dropping the pipeline it adopted and leaving the
     * surface it presents through (the pipeline's own destructor unregisters its
     * passes from the engine it was built on).
     *
     * The bound engine must outlive the view.
     */
    ~SceneView();

  public:
    // ---- Camera and content ----

    /** @brief Gets the view camera (non-owning). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the content scene drawn by this view.
     *
     * The returned reference shares ownership with the view, so it can be
     * bound explicitly to engine passes (addPass(pass, content, order)) that
     * render this view's content.
     *
     * @return The content scene (never null; an empty scene draws nothing).
     */
    intrusive_ptr<Scene> scene() const;

    /** @brief Sets the content scene drawn by this view.
     *
     * The view keeps a reference.
     *
     * @param scene Content scene, or null to clear (draws nothing).
     */
    void setScene(intrusive_ptr<Scene> scene);

  public:
    // ---- Navigation ----

    /** @brief Gets the camera manipulator driving this view's camera.
     *
     * A default orbit manipulator is created on first access when none was
     * set with setManipulator(). Creation is lazy so the manipulator snapshots
     * its home view from the camera's final placement.
     *
     * @return The manipulator (never null).
     */
    raw_ptr<CameraManipulator> manipulator();

    /** @brief Sets the camera manipulator driving this view's camera.
     *
     * @param manipulator Manipulator bound to this view's camera; the view
     *                    keeps a reference.
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

    /** @brief Refreshes the geometry of this view for a new surface size (keeps the current pixel ratio).
     *
     * @param width  New surface width in pixels (logical, as the host reports it).
     * @param height New surface height in pixels (logical, as the host reports it).
     */
    void resize(int width, int height);

    /** @brief Refreshes the geometry of this view for a new surface size and device pixel ratio.
     *
     * The size is the whole surface the host renders into: this view's area IS that surface (one area draws one
     * picture - see RenderSurface), so there is no second rectangle to state and nothing that could disagree with
     * it. What the view derives from the one size is its camera aspect, the size its off-screen targets and HUD
     * boxes get, and the size its surface-layout steps are told.
     *
     * Then it runs this view's surface-layout steps, with the area's size: the manipulator / camera
     * aspect first, then every callback registered with addSurfaceLayout(). The pipeline it adopted (when it has
     * one) is resized here too, so a view that draws through a built pipeline needs no layout step at all.
     *
     * A view that PRESENTS a surface forwards this to RenderSurface::resize: the size of the thing the picture
     * ends up in is a fact about the SURFACE, which is where it is recorded and fanned out to the view that
     * presents it (so one call sizes the picture, and a view that presents later is sized at once). This entry is then the
     * one for a view that presents nothing - a view used on its own needs no surface to be sized.
     *
     * The verb deliberately matches RenderSurface::resize: the SAME announcement, addressed to whichever of the
     * two a host holds.
     *
     * @param width        New surface width in logical pixels.
     * @param height       New surface height in logical pixels.
     * @param pixel_ratio  Device pixels per logical pixel (devicePixelRatio); <= 0 keeps the current one.
     */
    void resize(int width, int height, double pixel_ratio);

    /** @brief Registers a creator-managed surface-layout step.
     *
     * The callback receives the new surface size every time the view's surface changes and updates whatever
     * that code owns - an off-screen target's size (target->setSize), a pass's viewport
     * (pass->setViewport(Viewport{...})), etc. This keeps each pipeline's resize policy with its creator
     * (deferred chains at native / fractional resolution, fixed shadow maps doing nothing, corner-anchored
     * previews / gizmos re-anchoring) while the engine stays a pure scheduler.
     *
     * The size is in the space the HOST reports (for the Qt host, the LOGICAL size its widget has - see
     * RenderControl). That matters because the consumers want different spaces: a pass viewport is in
     * DEVICE pixels, while AxisGizmo / FpsOverlay are laid out on the logical size and apply the ratio
     * themselves. A creator whose consumer needs device pixels therefore scales by the host's ratio here
     * (see Pipeline::resize, which takes the ratio for exactly that reason).
     *
     * @param layout Callback taking the new surface width and height the host reported.
     */
    void addSurfaceLayout(std::function<void(int width, int height)> layout);

    /** @brief Registers a creator-managed surface-layout step that also wants the device pixel ratio.
     *
     * Same contract as the two-argument overload, with the ratio the host reported added: the one consumer
     * that needs it is a creator laying out in DEVICE pixels (`pass->setViewport(Viewport{...})`, an off-screen
     * target at native resolution). Multiplying here is what keeps a layer-1 host from reaching back into
     * appfw for the ratio (RenderControl::devicePixelRatio()).
     *
     * The size is again the AREA's size (one area draws one picture - see RenderSurface), in the host's logical
     * space.
     *
     * @param layout Callback taking the view's width, height and the host's pixel ratio.
     */
    void addSurfaceLayout(std::function<void(int width, int height, double pixel_ratio)> layout);

    /** @brief Removes all registered surface-layout steps. */
    void clearSurfaceLayouts();

    /** @brief The surface width (logical px) this view derives its picture from.
     *
     * The SURFACE's while this view presents one, this view's own announcement otherwise (see resize):
     * the size has one owner, and a presenting view asks it rather than holding a second copy that could drift.
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

    /** @brief Binds the engine this view renders through (borrowed).
     *
     * Re-binding replaces any previous engine. The engine stays content-free;
     * this view binds its content to the passes it registers.
     *
     * @param engine Engine to borrow; must outlive the view. Null detaches.
     */
    void setEngine(raw_ptr<RenderEngine> engine);

    /** @brief Gets the bound engine, or nullptr when unset. */
    raw_ptr<RenderEngine> engine() const;

    /** @brief Gets the pipeline this view adopted (setPipeline), or nullptr.
     *
     * The handle for inspection and micro-tuning - `windowPass()` (what presents this view),
     * `gizmoPass()` / `fpsOverlayPass()` (their state IS the overlay's visibility), `gizmo()` / `fpsOverlay()`
     * (the content objects), `offscreenTarget()`, `setShadingMode()` - and the answer to "is this view drawing
     * anything at all": null means no pipeline was adopted, and a view with none draws nothing.
     *
     * The view holds the pipeline (adopting shares ownership), so it stays alive while the view draws through
     * it; letting it go is what removes its passes from the frame.
     *
     * @return The adopted pipeline, or nullptr when this view has none.
     */
    raw_ptr<Pipeline> pipeline() const;

    /** @brief Adopts @p pipeline as what draws this view (build one with RenderPipelineBuilder).
     *
     * THE one step that connects a view to a drawing: the view brings what a pipeline cannot know (this view's
     * camera and content are the builder's inputs, and the area's size and its layout steps are applied
     * HERE - what the off-screen targets are sized to, where the HUD boxes anchor), and the
     * pipeline brings what a view must not own (which passes exist, the shading mode, the overlays).
     *
     * Adopting replaces any previous pipeline, and passing null drops the current one - which is how a view is
     * taken out of the frame: the pipeline unregisters its passes when its last handle goes away, so a dropped
     * view draws nothing instead of leaving passes to run with nobody holding them. A view that presents a
     * surface (RenderSurface::present) keeps being one across a change: the area resolves the view's pass
     * afresh, so a REBUILT pipeline (a recipe change) is followed without re-declaring anything.
     *
     * The view is drawn where its area is - the whole of it (see RenderSurface) - and it is the CALLER's business
     * to have built a pipeline for this view: a pipeline built for another camera draws that camera's picture into
     * this view's area (the engine reports the shape mismatch when the two disagree).
     *
     * @param pipeline Pipeline that draws this view; null drops the current one.
     */
    void setPipeline(intrusive_ptr<Pipeline> pipeline);

    /** @brief Gets whether this view presents a surface (RenderSurface::present bound it to one).
     *
     * The question a host asks when it has a surface of its own and a view of its own: the surface feeds every
     * view that presents it (RenderSurface::resize), so a view that IS one of its presentations needs nothing
     * else, and a view that is not (the host declared a pass of its own) is the only one left to feed by hand.
     *
     * @return true when this view presents a surface.
     */
    [[nodiscard]] bool hasSurface() const noexcept;

    /** @brief Renders one frame through the bound engine.
     *
     * @param dt Seconds elapsed since the previous frame.
     */
    void frame(double dt = 0.0);

  private:
    /** @brief Binds this view to the surface it presents, or unbinds it (RenderSurface::present / dismiss).
     *
     * Not host API: the door is RenderSurface::present, and this pointer is what that table uses to find this
     * view's window pass again - a host that set it by hand would make the surface believe in a presentation
     * nobody declared. Binds both directions of the pair (the surface names the view, the view names the surface),
     * and both are borrowed: they are unbound when either one goes away.
     *
     * @param surface Surface this view presents (borrowed), or null when it presents none.
     */
    void setSurface(raw_ptr<RenderSurface> surface) noexcept;

    /** @brief Records the surface size (and ratio) a view with NO surface derives from.
     *
     * The step resize() takes when this view presents nothing. A view that presents a surface does not
     * need it: it reads the surface's numbers (see surfaceWidth), because the size of the thing the picture ends
     * up in has ONE owner and that is the surface (RenderSurface::resize).
     *
     * @param width        Surface width in logical pixels.
     * @param height       Surface height in logical pixels.
     * @param pixel_ratio  Device pixels per logical pixel; <= 0 keeps the current one.
     */
    void setOwnSurfaceSize(int width, int height, double pixel_ratio);

    /** @brief Turns {surface size, pixel ratio} into this view's picture, in one place.     *
     * The step RenderSurface::resize fans out to the view that presents it (the view is what a size is FOR), and
     * the step this view runs on its own when it presents nothing - not host API either way: the announcement that
     * leads here is resize() / RenderSurface::resize.
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
