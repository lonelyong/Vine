#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "CameraMirror.hpp"
#include "RenderPass.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief A small world-orientation indicator drawn as a HUD render pass.
 *
 * AxisGizmo is a self-contained HUD pass: it owns a content scene with three
 * coloured sticks along +X (red), +Y (green) and +Z (blue), rendered into a
 * square sub-viewport in the bottom-left corner of the surface, and mirrors a
 * source camera's orientation (MirrorMode::Orientation) on every execute()
 * while keeping its own framing distance. Register it like any other pass,
 * with an order above the main view's (RenderEngine::addPass); the engine
 * draws it on top of lower-order passes. Surface layout is creator-managed:
 * the code that owns the gizmo reports the surface size on every resize by
 * calling onSurfaceResized(w, h) (e.g. via a SceneView layout step).
 *
 * Example:
 * \code
 * auto gizmo = make_intrusive<AxisGizmo>();
 * gizmo->setSourceCamera(view->camera());   // the view's primary camera
 * view->addSurfaceLayout([gizmo](int w, int h) { gizmo->onSurfaceResized(w, h); });
 * engine->addPass(gizmo, 10);               // draws on top of the order-0 window pass
 * \endcode
 */
class V_GRAPHICS_API AxisGizmo : public RenderPass {
    V_OBJECT_META_DECL;

  public:
    /** @brief Constructs a gizmo with three unit axis sticks.
     *
     * The pass never clears (it draws over the previous content). Its
     * sub-viewport defaults to a 96px box at the origin corner until its
     * owner reports a surface size (see onSurfaceResized).
     */
    AxisGizmo();

    /** @brief Destroys the gizmo. */
    ~AxisGizmo() override;

    /** @brief Sets the stick length in world units.
     *
     * @param length Positive stick length (default 1).
     */
    void setAxisLength(double length);

    /** @brief Sets the stick cross-section half extent in world units.
     *
     * @param thickness Positive half thickness (default 0.09).
     */
    void setThickness(double thickness);

    /** @brief Sets the on-screen box size in device pixels.
     *
     * @param size Side length of the square gizmo viewport (default 96).
     */
    void setBoxSize(int size);

    /** @brief Sets the ratio between logical surface size and device pixels.
     *
     * The gizmo is positioned in device pixels because the render backend
     * draws into a native surface sized in device pixels. Qt reports logical
     * sizes, so hosts on high-DPI displays must supply their devicePixelRatio
     * (default 1).
     *
     * @param ratio Device pixel ratio (> 0).
     */
    void setPixelRatio(double ratio);

    /** @brief Sets the camera the gizmo mirrors (non-owning).
     *
     * Usually the camera of the SceneView presenting the main content (e.g.
     * view->camera()). The gizmo follows @p camera's orientation each time it
     * executes; pass nullptr to stop following.
     *
     * @param camera Source camera, or null to disable mirroring.
     */
    void setSourceCamera(raw_ptr<Camera> camera);

    /** @brief Gets the camera the gizmo mirrors, or nullptr when unset. */
    raw_ptr<Camera> sourceCamera() const;

    /** @brief Gets the content scene drawn by the gizmo. */
    raw_ptr<Scene> content() const;

    /** @brief Re-anchors the gizmo viewport to the bottom-left corner for a
     * new surface size.
     *
     * Called by the gizmo's owner whenever the surface changes (e.g. from a
     * SceneView layout step, see SceneView::addSurfaceLayout). The size is
     * in the same space as the other layout sizes (device pixels, top-left
     * origin); the configured devicePixelRatio is applied when converting.
     *
     * @param width  Surface width in pixels.
     * @param height Surface height in pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Draws the gizmo.
     *
     * Applies the orientation mirror from the configured source camera onto
     * the framing camera, then draws the gizmo's own content scene through
     * the base pass machinery (camera, disabled clearing, sub-viewport). The
     * scene supplied by the engine is ignored: the gizmo is self-contained.
     *
     * @param scene   Ignored (the gizmo draws its own content scene).
     * @param backend Backend to render with.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend) override;

  private:
    /** @brief Applies the orientation mirror onto the framing camera. */
    void applyMirror();

    /** @brief Rebuilds the axis content from the current geometry settings. */
    void rebuild();

    /// Owned framing camera (kept alive for this pass' raw camera pointer).
    intrusive_ptr<Camera> camera_;
    /// Content scene holding the three sticks (owned).
    intrusive_ptr<Scene> content_;
    /// Source camera followed each frame (non-owning).
    raw_ptr<Camera> source_camera_ = nullptr;
    double axis_length_ = 1.0;
    double thickness_ = 0.09;
    double pixel_ratio_ = 1.0;
    int margin_px_ = 16;
    int size_px_ = 96;
    int surface_w_ = 0;
    int surface_h_ = 0;
};

using AxisGizmoPtr = intrusive_ptr<AxisGizmo>;

V_GRAPHICS_NS_END
