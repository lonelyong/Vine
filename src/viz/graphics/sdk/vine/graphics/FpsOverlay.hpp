#pragma once
#include "graphics_global.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/raw_ptr.hpp>

#include "Geometry.hpp"
#include "RenderPass.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief A frame-rate readout drawn as a HUD render pass in a corner.
 *
 * FpsOverlay is a self-contained HUD pass (like AxisGizmo): it owns a content
 * scene holding a 3-digit seven-segment display built from thin bars, rendered
 * into a sub-viewport anchored to the BOTTOM-RIGHT corner of the surface
 * (device pixels). Each execute() it measures the actual render-loop frame
 * rate (steady clock), EMA-smooths it and, at a throttled cadence, writes the
 * value into the row of bars.
 *
 * The row is ONE geometry with ONE material, so the whole readout is one draw — the shape a HUD has
 * everywhere else. Seven bars per digit as seven nodes with seven materials was 7 to 21 draw commands
 * (and as many material blocks) for what is at most 21 flat bars. A bar its digit does not light is
 * collapsed onto a single point instead of being drawn: it covers no pixels, so the picture is the one
 * the previous per-segment visibility gate produced (no dark "8" behind the number), while the vertex
 * count never changes — which is what lets a change be served as ONE in-place stream refresh
 * (Geometry::bumpRevision) rather than a geometry rebuild. A change therefore costs one packed position
 * buffer and one revision bump, whatever the digits are.
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
class VN_GRAPHICS_API FpsOverlay : public RenderPass {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a readout whose digits are hidden until the first measurement.
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
     * SceneView surface-layout step / Pipeline::resize()). The size is the
     * LOGICAL surface size the host reports - the same number it hands to
     * SceneView::onSurfaceResized - and the configured devicePixelRatio is
     * applied HERE, which is what turns it into the device-pixel rectangle a
     * backend viewport needs. Handing it device pixels scales the box twice.
     *
     * @param width  Surface width in logical pixels.
     * @param height Surface height in logical pixels.
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

    /** @brief Updates the displayed value from the smoothed frame rate.
     *
     * Writes the shown digits into the row of bars (see writePattern) and does nothing while the value
     * does not change, so a steady frame rate costs no data work at all.
     *
     * @param dt Seconds since the previous call.
     */
    void updateReadout(double dt);

    /** @brief Writes the segments @p pattern lights into the row of bars.
     *
     * Bar i of the row is bit i of the pattern: digit d occupies bits d * 7 .. d * 7 + 6, bit 0 being
     * segment a, the way a seven-segment decoder orders them. A lit bar keeps the box the template
     * holds, an unlit one is collapsed onto its first corner: the geometry's positions are rewritten and
     * the revision bumped, so the backend serves the edit in place.
     *
     * @param pattern The 21 segment bits to light.
     */
    void writePattern(std::uint32_t pattern);

    // Framing camera (owned so the pass' raw camera pointer never dangles).
    intrusive_ptr<Camera> camera_;
    // Content scene holding the row of bars (owned).
    intrusive_ptr<Scene> content_;
    // The row itself: all three digits' bars in ONE geometry (see the class note). Held so a change can
    // rewrite its positions without walking the scene.
    intrusive_ptr<Geometry> readout_;
    // The row's positions with EVERY segment lit -- the template writePattern() copies its lit bars
    // from. Keeping it is what makes a change a memcpy of the boxes to keep plus one collapsed point per
    // box to drop, instead of a mesh built from a bar spec.
    std::vector<vn::math::Vec3f> row_positions_;

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

VN_GRAPHICS_NS_END
