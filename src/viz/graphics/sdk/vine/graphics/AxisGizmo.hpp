#pragma once
#include "graphics_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief The axis-gizmo HUD's CONTENT: three coloured sticks, and the camera that frames them.
 *
 * What this owns is the half a pass cannot: its content scene (three sticks along +X red, +Y green, +Z blue),
 * the framing camera they are drawn with, and the BOX (a device-pixel rectangle in the bottom-left corner of the
 * area it belongs to) they are projected into. It is deliberately NOT a pass: a pass is a DECLARATION now (see
 * PassDesc), so whoever shows this gizmo declares one - `content` = this object's scene, `camera` = this object's
 * camera, `viewport` = boxViewport(), `before_draw` = update(), which mirrors the source camera's orientation
 * (MirrorMode::Orientation) onto the framing one and keeps its own framing distance.
 *
 * Surface layout is creator-managed: the object's owner reports the surface size on every resize
 * (onSurfaceResized) and points it at the corner of the area it lays out in (setAreaOrigin). The pipeline does
 * both for the standard case (Pipeline::resize), and a view states the area origin (SceneView).
 *
 * Example:
 * \code
 * auto gizmo = make_intrusive<AxisGizmo>();
 * gizmo->setSourceCamera(view->camera());          // the camera it mirrors
 * PassDesc hud;
 * hud.name            = u8"axis_gizmo";
 * hud.tag             = u8"Hud";                   // a HUD pass declared like any other
 * hud.stage           = PipelineStage::Overlay;
 * hud.camera          = intrusive_ptr<Camera>(gizmo->camera());
 * hud.content         = intrusive_ptr<Scene>(gizmo->content());
 * hud.viewport        = gizmo->boxViewport();
 * hud.clear_enabled   = false;
 * hud.depth_mode      = DepthMode::Disabled;
 * hud.before_draw     = [gizmo] { gizmo->update(); };
 * engine->addPass(hud);
 * \endcode
 */
class VN_GRAPHICS_API AxisGizmo : public Object, public RefCounted<AxisGizmo> {
    VN_OBJECT_META_DECL;

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

    /** @brief Gets the camera that frames the sticks (the one a declaration states as its camera). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the device-pixel rectangle the sticks are drawn into (the viewport a declaration states).
     *
     * The box follows the surface (onSurfaceResized) and the area origin (setAreaOrigin); a zero-area box means
     * "this overlay does not fit in the area right now" and is skipped by the backend's empty-rectangle rule.
     *
     * @return The box, in device pixels, top-left origin.
     */
    [[nodiscard]] Viewport boxViewport() const;

    /** @brief Re-anchors the gizmo viewport to the bottom-left corner for a
     * new surface size.
     *
     * Called by the gizmo's owner whenever the surface changes (e.g. from a
     * SceneView layout step, see SceneView::addSurfaceLayout). The size is the
     * LOGICAL surface size the host reports - the same number it hands to
     * SceneView::resize - and the configured devicePixelRatio is
     * applied HERE, which is what turns it into the device-pixel rectangle a
     * backend viewport needs. Handing it device pixels scales the box twice.
     *
     * @param width  Surface width in logical pixels.
     * @param height Surface height in logical pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Sets the origin of the area this overlay lays its box out in, in device pixels.
     *
     * The overlay anchors to the corner of its AREA (bottom-left for the gizmo, bottom-right for the
     * readout), and its area starts at (0, 0) by default - the whole surface. A view that draws into a part
     * of a shared surface (a split view) points this at that part's origin so the overlay lands inside it.
     *
     * @param x Area origin x in device pixels.
     * @param y Area origin y in device pixels.
     */
    void setAreaOrigin(int x, int y);

    /** @brief Does the gizmo's per-frame work: mirror the source camera's orientation.
     *
     * The one thing that must happen before the gizmo is drawn, and the reason the declaration that shows it has
     * a `before_draw` hook at all (PassDesc). It writes this object's framing camera and touches no backend.
     */
    void update();

  private:
    /** @brief Applies the orientation mirror onto the framing camera. */
    void applyMirror();

    /** @brief Rebuilds the axis content from the current geometry settings. */
    void rebuild();

    /// Owned framing camera (the one a declaration states; the gizmo keeps it alive).
    intrusive_ptr<Camera> camera_;
    /// Content scene holding the three sticks (owned).
    intrusive_ptr<Scene> content_;
    /// The box the sticks are drawn into, in device pixels (see boxViewport).
    Viewport box_{};
    /// Source camera followed each frame (non-owning).
    raw_ptr<Camera> source_camera_ = nullptr;
    double axis_length_ = 1.0;
    double thickness_ = 0.09;
    double pixel_ratio_ = 1.0;
    int margin_px_ = 16;
    int size_px_ = 96;
    int surface_w_ = 0;
    int surface_h_ = 0;
    int area_x_ = 0;
    int area_y_ = 0;
};

using AxisGizmoPtr = intrusive_ptr<AxisGizmo>;

VN_GRAPHICS_NS_END
