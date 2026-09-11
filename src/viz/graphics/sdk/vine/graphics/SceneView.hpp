#pragma once
#include "graphics_global.hpp"

#include <functional>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/window/window_global.hpp>

V_WINDOW_NS_BEGIN
struct KeyEvent;
struct MouseEvent;
struct ScrollEvent;
V_WINDOW_NS_END

V_GRAPHICS_NS_BEGIN

class Camera;
class CameraManipulator;
class Pipeline;
class RenderEngine;
class RenderPass;
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
 * holds no content scene: the view keeps its content and binds it explicitly
 * to the passes that draw it (its default window pass binds it on
 * ensureWindowPass()). When attached, ensureWindowPass() registers an
 * order-0 window pass that presents this view's camera to the backbuffer -
 * unless the application already registered such a pass (e.g. a
 * deferred-lighting main pass that carries this view's camera).
 *
 * Several views may share one engine, but each view's DEFAULT window pass
 * clears the whole surface and draws full-surface: with two such passes the
 * second erases the first. Side-by-side views therefore need explicit layout -
 * give each pass a sub-viewport (RenderPass::setViewport) and disable clearing
 * on all but one (setClearEnabled), or build the pipeline yourself with
 * RenderPipelineBuilder. The engine never manages viewport layout.
 *
 * The engine stays camera- and content-agnostic: it forwards no mouse /
 * scroll / key input, holds no camera and no content scene. The host pushes
 * window input and surface resizes to the view (pushEvent /
 * onSurfaceResized), which forwards them to the manipulator and keeps the
 * camera projection correct.
 */
class V_GRAPHICS_API SceneView : public RefCounted<SceneView> {
  public:
    /** @brief Constructs a view with a default camera and an empty content
     * scene.
     *
     * The view is not attached to an engine until setEngine() is called.
     */
    SceneView();

    /** @brief Destroys the view, removing its default window pass from the
     * bound engine (when this view registered one).
     *
     * The bound engine must outlive the view.
     */
    ~SceneView();

  public:
    // ---- Engine binding ----

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

    /** @brief Registers an order-0 window pass presenting this view's camera
     * to the backbuffer, drawing this view's content scene.
     *
     * The default viewer is the shared Forward preset (RenderPipelineBuilder):
     * the view assembles it through the same recipe an application would use
     * for an explicit forward pipeline. The pass is only added when the
     * engine does not already present this view's camera to the window (see
     * RenderEngine::hasWindowPass), so an application that built its own main
     * pass with this view's camera (e.g. a Deferred lighting pass) keeps full
     * control. Calling this twice is a no-op while the pipeline is
     * registered. A host convenience layer calls this once after the
     * application has registered its pipeline.
     */
    void ensureWindowPass();

  public:
    // ---- Camera / content ----

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
    // ---- Window input / surface (forwarded to the manipulator) ----

    /** @brief Pushes a mouse event to the manipulator.
     *
     * @param event Mouse event to forward.
     */
    void pushEvent(const vine::window::MouseEvent& event);

    /** @brief Pushes a scroll (wheel) event to the manipulator.
     *
     * @param event Scroll event to forward.
     */
    void pushEvent(const vine::window::ScrollEvent& event);

    /** @brief Pushes a keyboard event to the manipulator.
     *
     * @param event Key event to forward.
     */
    void pushEvent(const vine::window::KeyEvent& event);

    /** @brief Refreshes the camera projection aspect for a new surface size.
     *
     * Runs this view's surface-layout steps: the manipulator / camera aspect
     * first, then every layout callback registered with addSurfaceLayout().
     * The view does not manage off-screen target sizes or pass viewports
     * itself - the code that created them registers a callback that updates
     * them with its own policy (see addSurfaceLayout).
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Registers a creator-managed surface-layout step.
     *
     * The callback receives the new surface size (device pixels) every time
     * the view's surface changes and updates whatever that code owns - an
     * off-screen target's size (target->setSize), a pass's viewport
     * (pass->setViewport(Viewport{...})), etc. This keeps each pipeline's
     * resize policy with its creator (deferred chains at native / fractional
     * resolution, fixed shadow maps doing nothing, corner-anchored previews /
     * gizmos re-anchoring) while the engine stays a pure scheduler.
     *
     * @param layout Callback taking the new surface width and height in
     *               device pixels.
     */
    void addSurfaceLayout(std::function<void(int width, int height)> layout);

    /** @brief Removes all registered surface-layout steps. */
    void clearSurfaceLayouts();

  public:
    /** @brief Renders one frame through the bound engine.
     *
     * @param dt Seconds elapsed since the previous frame.
     */
    void frame(double dt = 0.0);

  private:
    /** @brief Removes the default window pass this view registered, if any. */
    void removeWindowPass();

    // The bound engine is borrowed (the host owns it and keeps it alive).
    raw_ptr<RenderEngine> engine_ = nullptr;

    intrusive_ptr<Camera>  camera_;
    intrusive_ptr<Scene>   scene_;
    intrusive_ptr<CameraManipulator> manipulator_;

    // The default (Forward) window pipeline this view registered, if any.
    intrusive_ptr<Pipeline> default_pipeline_;

    // Creator-managed surface-layout steps run on every surface resize.
    std::vector<std::function<void(int width, int height)>> surface_layouts_;
};

V_GRAPHICS_NS_END
