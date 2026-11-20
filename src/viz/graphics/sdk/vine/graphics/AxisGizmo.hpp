#pragma once
#include "graphics_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "AxesMarker.hpp"
#include "FrameUpdate.hpp"
#include "Light.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/// @brief Default side of the gizmo's square box, in DEVICE pixels (the unit the readout next door states).
inline constexpr int kAxisGizmoBoxSize = 128;

/**
 * @brief The axis-gizmo HUD's CONTENT: one coordinate-frame marker, and the camera that frames it.
 *
 * @details
 * - What this owns is the half a pass cannot: its content scene (a marker built by makeAxesMarker - three
 *   arrows along +X red, +Y green, +Z blue), the light rig that shades it, the framing camera they are drawn with,
 *   and the BOX (a device-pixel rectangle in the bottom-left corner of the area it belongs to) they are projected
 *   into. It is deliberately NOT a pass: a pass is a DECLARATION now (see PassDesc), so whoever shows this gizmo
 *   declares one -
 *   `content` = this object's scene, `camera` = this object's camera, `viewport` = boxViewport() -
 *   and registers this object with the chain as the overlay whose update() mirrors the source camera's
 *   orientation (MirrorMode::Orientation) onto the framing one (see FrameUpdate).
 * - Surface layout is creator-managed: the object's owner reports the surface size on every resize
 *   (onSurfaceResized) and points it at the corner of the area it lays out in (setAreaOrigin). The
 *   chain does both for the standard case (PassChain::resize), and a view states the area origin
 *   (SceneView).
 *
 * Example:
 * \code
 * auto gizmo = make_intrusive<AxisGizmo>();
 * gizmo->setSourceCamera(view->camera());          // the camera it mirrors
 * PassDesc hud;
 * hud.name            = u8"axis_gizmo";
 * hud.stage           = PassStage::Overlay;         // a HUD pass declared like any other
 * hud.camera          = intrusive_ptr<Camera>(gizmo->camera());
 * hud.content         = intrusive_ptr<Scene>(gizmo->content());
 * hud.viewport        = gizmo->boxViewport();
 * hud.clear_enabled   = false;                     // the picture it is drawn over stays
 * hud.clear_depth     = true;                      // ...and the depth inside the box is the marker's own
 * hud.depth_mode      = DepthMode::TestAndWrite;   // so the arrows occlude each other like solids
 * chain->addOverlay(u8"gizmo", gizmo.get(), hud);   // the chain updates it before each frame
 * \endcode
 */
class VN_GRAPHICS_API AxisGizmo : public Object, public RefCounted<AxisGizmo>, public FrameUpdate {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a gizmo with one unit coordinate-frame marker.
     *
     * @details
     * - The declaration a caller writes for it clears DEPTH only (see the class example): the marker owns the depth
     *   inside its box and never clears the colour it is drawn over. Its sub-viewport defaults to a 128px box at the
     *   origin corner until its owner reports a surface size (see onSurfaceResized).
     */
    AxisGizmo();

    /** @brief Destroys the gizmo. */
    ~AxisGizmo() override;

    /** @brief Sets the whole marker style: which arrows, their proportions, their resolution and their colours.
     *
     * @details
     * - The content is rebuilt when the style differs from the current one, and LEFT ALONE when it does not - so a
     *   host may call this on configuration without paying for a rebuild per call.
     * - This is the one setter that can express everything the marker is; `setAxisLength()` and `setThickness()`
     *   are the two scalars hosts change most often, and each rebuilds the content once.
     *
     * @param style Marker style (see AxesStyle).
     */
    void setStyle(const AxesStyle& style);

    /** @brief Gets the marker style the content is built from.
     *
     * @return The style.
     */
    [[nodiscard]] const AxesStyle& axesStyle() const;

    /** @brief Sets the arrow length in world units.
     *
     * @details
     * - The marker's proportions are fractions of this length, so the whole marker scales with it.
     *
     * @param length Positive arrow length, tip to origin (default 1).
     */
    void setAxisLength(double length);

    /** @brief Sets the arrow's widest radius, as a fraction of the arrow length.
     *
     * @details
     * - The widest radius of an arrow is its cone's; the shaft takes the same share of it that a default marker
     *   uses (kAxesShaftToTipRatio), so a thicker arrow stays an arrow instead of becoming a spike on a hairline.
     *
     * @param thickness Positive fraction of the axis length (default 0.09).
     */
    void setThickness(double thickness);

    /** @brief Sets the on-screen box size in DEVICE pixels.
     *
     * @details
     * - The same unit the FPS readout states its box in: the surface size is converted to device pixels with the
     *   pixel ratio (see onSurfaceResized), and this box is fitted inside it as stated - a host on a high-DPI
     *   display that wants a larger gizmo states a larger number here.
     *
     * @param size Side length of the square gizmo viewport (default kAxisGizmoBoxSize).
     */
    void setBoxSize(int size);

    /** @brief Sets the ratio between logical surface size and device pixels.
     *
     * @details
     * - The gizmo is positioned in device pixels because the render backend draws into a native surface
     *   sized in device pixels. Qt reports logical sizes, so hosts on high-DPI displays must supply
     *   their devicePixelRatio (default 1).
     *
     * @param ratio Device pixel ratio (> 0).
     */
    void setPixelRatio(double ratio);

    /** @brief Sets the camera the gizmo mirrors (non-owning).
     *
     * @details
     * - Usually the camera of the SceneView presenting the main content (e.g. view->camera()). The
     *   gizmo follows @p camera's orientation each time it executes; pass nullptr to stop following.
     *
     * @param camera Source camera, or null to disable mirroring.
     */
    void setSourceCamera(raw_ptr<Camera> camera);

    /** @brief Gets the camera the gizmo mirrors, or nullptr when unset. */
    raw_ptr<Camera> sourceCamera() const;

    /** @brief Gets the content scene drawn by the gizmo. */
    raw_ptr<Scene> content() const;

    /** @brief Gets the camera that frames the arrows (the one a declaration states as its camera). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the device-pixel rectangle the marker is drawn into (the viewport a declaration states).
     *
     * @details
     * - The box follows the surface (onSurfaceResized) and the area origin (setAreaOrigin); a
     *   zero-area box means "this overlay does not fit in the area right now" and is skipped by the
     *   backend's empty-rectangle rule.
     *
     * @return The box, in device pixels, top-left origin.
     */
    [[nodiscard]] Viewport boxViewport() const;

    /** @brief Re-anchors the gizmo viewport to the bottom-left corner for a new surface size.
     *
     * @details
     * - Called by the gizmo's owner whenever the surface changes (e.g. from a SceneView layout step,
     *   see SceneView::addSurfaceLayout). The size is the LOGICAL surface size the host reports - the
     *   same number it hands to SceneView::resize - and the configured devicePixelRatio is applied
     *   HERE, which is what turns it into the device-pixel rectangle a backend viewport needs. Handing
     *   it device pixels scales the box twice.
     *
     * @param width  Surface width in logical pixels.
     * @param height Surface height in logical pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Sets the origin of the area this overlay lays its box out in, in device pixels.
     *
     * @details
     * - The overlay anchors to the corner of its AREA (bottom-left for the gizmo, bottom-right for the
     *   readout), and its area starts at (0, 0) by default - the whole surface. A view that draws into
     *   a part of a shared surface (a split view) points this at that part's origin so the overlay
     *   lands inside it.
     *
     * @param x Area origin x in device pixels.
     * @param y Area origin y in device pixels.
     */
    void setAreaOrigin(int x, int y);

    /** @brief Does the gizmo's per-frame work: mirror the source camera's orientation.
     *
     * @details
     * - The one thing that must happen before the gizmo is drawn, and the reason it implements FrameUpdate: a
     *   chain (or a host driving the engine) calls it once per frame before the frame is built. It writes this
     *   object's framing camera and touches no backend.
     */
    /** @brief Mirrors the source camera's orientation into this overlay's own camera and posts the result.
     *
     * @details
     * - The gizmo needs nothing from @p context: what it mirrors is the source CAMERA, which it reads through
     *   `setSourceCamera`. The argument is part of the hook's one shape (see FrameUpdate), not a value this object
     *   uses - the FPS readout next door is the one that wants the interval.
     *
     * @param context This frame's context (see FrameContext); not used here.
     */
    void onFrame(const FrameContext& context) override;

  private:
    /** @brief Applies the orientation mirror onto the framing camera. */
    void applyMirror();

    /** @brief Rebuilds the axis content from the current geometry settings. */
    void rebuild();

    /// Owned framing camera (the one a declaration states; the gizmo keeps it alive).
    intrusive_ptr<Camera> camera_;
    /// Content scene holding the marker (owned).
    intrusive_ptr<Scene> content_;
    /// The box the marker is drawn into, in device pixels (see boxViewport).
    Viewport box_{};
    /// Source camera followed each frame (non-owning).
    raw_ptr<Camera> source_camera_ = nullptr;
    /// The light rig the marker is shaded by; both are added to the content scene (see rebuild).
    intrusive_ptr<Light> ambient_;
    intrusive_ptr<Light> key_light_;
    /// How the marker looks; the content is rebuilt when it changes (see setStyle).
    AxesStyle style_{};
    double    pixel_ratio_ = 1.0;
    int             margin_px_     = 16;
    int             size_px_       = kAxisGizmoBoxSize;
    int             surface_w_     = 0;
    int             surface_h_     = 0;
    int             area_x_        = 0;
    int             area_y_        = 0;
};

using AxisGizmoPtr = intrusive_ptr<AxisGizmo>;

VN_GRAPHICS_NS_END
