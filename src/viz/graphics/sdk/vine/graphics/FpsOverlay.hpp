#pragma once
#include "graphics_global.hpp"

#include <chrono>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Material.hpp"
#include "RenderPass.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief A frame-rate readout drawn as a HUD render pass in a corner.
 *
 * FpsOverlay is a self-contained HUD pass (like AxisGizmo): it owns a content
 * scene holding a 3-digit seven-segment display built from thin bars, rendered
 * into a sub-viewport anchored to the BOTTOM-RIGHT corner of the surface
 * (device pixels). Each execute() it measures the actual render-loop frame
 * rate (steady clock), EMA-smooths it and, at a throttled cadence, lights or
 * dims each segment's material colour — riding the shared-material hot path
 * (the backend rewrites the DYNAMIC Phong UBO in place; no geometry rebuild).
 *
 * The overlay needs no source camera: it uses its own static framing camera,
 * so the digits stay pinned to the corner while the scene camera orbits.
 * Register it like any other pass with an order above the main view's
 * (RenderEngine::addPass); surface layout is creator-managed — report the
 * surface size on every resize by calling onSurfaceResized(w, h) (e.g. via a
 * Pipeline::resize() / SceneView surface-layout step), which re-anchors the
 * sub-viewport to the bottom-right corner.
 *
 * Example:
 * \code
 * auto fps = make_intrusive<FpsOverlay>();
 * fps->setPixelRatio(view->devicePixelRatio());
 * view->addSurfaceLayout([fps](int w, int h) { fps->onSurfaceResized(w, h); });
 * engine->addPass(fps, 30);             // draws above the order-0 window pass
 * \endcode
 */
class V_GRAPHICS_API FpsOverlay : public RenderPass {
    V_OBJECT_META_DECL;

  public:
    /** @brief Constructs a readout with a dim 3-digit seven-segment display.
     *
     * The pass never clears (it draws over the previous content). Its
     * sub-viewport defaults to a box at the origin corner until its owner
     * reports a surface size (see onSurfaceResized).
     */
    FpsOverlay();

    /** @brief Destroys the overlay. */
    ~FpsOverlay() override;

    /** @brief Sets the ratio between logical surface size and device pixels.
     *
     * The readout is positioned in device pixels because the render backend
     * draws into a native surface sized in device pixels. Qt reports logical
     * sizes, so hosts on high-DPI displays must supply their devicePixelRatio
     * (default 1).
     *
     * @param ratio Device pixel ratio (> 0).
     */
    void setPixelRatio(double ratio);

    /** @brief Sets the on-screen box size in device pixels.
     *
     * The box is wide (a 3-digit row); its aspect also frames the overlay
     * camera so the digits fill the box without distortion. Defaults to a
     * compact 105 x 36 box (bottom-right); see setSize.
     *
     * @param width  Box width in device pixels (default 105).
     * @param height Box height in device pixels (default 36).
     */
    void setSize(int width, int height);

    /** @brief Gets the content scene drawn by the overlay. */
    raw_ptr<Scene> content() const;

    /** @brief Re-anchors the overlay viewport to the bottom-right corner for a
     * new surface size.
     *
     * Called by the overlay's owner whenever the surface changes (e.g. from a
     * SceneView surface-layout step / Pipeline::resize()). Sizes are in the
     * same space as the other layout sizes (device pixels, top-left origin);
     * the configured devicePixelRatio is applied when converting.
     *
     * @param width  Surface width in pixels.
     * @param height Surface height in pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Draws the readout.
     *
     * Measures the current frame rate, updates the digit segments, then draws
     * the overlay's own content scene through the base pass machinery
     * (camera, disabled clearing, bottom-right sub-viewport). The scene
     * supplied by the engine is ignored: the overlay is self-contained.
     *
     * @param scene   Ignored (the overlay draws its own content scene).
     * @param backend Backend to render with.
     */
    void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend) override;

  private:
    /** @brief Rebuilds the seven-segment content scene. */
    void rebuild();

    /** @brief Updates the displayed value from the smoothed frame rate. */
    void updateReadout(double dt);

    // Framing camera (owned so the pass' raw camera pointer never dangles).
    intrusive_ptr<Camera> camera_;
    // Content scene holding the digit bars (owned).
    intrusive_ptr<Scene> content_;
    // One material per (digit, segment); colour flips light/dim a segment.
    std::vector<intrusive_ptr<Material>> segment_materials_;

    double pixel_ratio_ = 1.0;
    int    box_width_px_ = 105;
    int    box_height_px_ = 36;
    int    margin_px_ = 8;
    int    surface_w_ = 0;
    int    surface_h_ = 0;

    // Frame-rate smoothing / readout throttle state.
    std::chrono::steady_clock::time_point last_tick_{};
    double fps_smoothed_ = 0.0;
    double readout_elapsed_ = 0.0;
    int    shown_value_ = -1;
};

using FpsOverlayPtr = intrusive_ptr<FpsOverlay>;

V_GRAPHICS_NS_END
