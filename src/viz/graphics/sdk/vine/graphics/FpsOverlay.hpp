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
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief The frame-rate readout's CONTENT: a 3-digit seven-segment row, the camera that frames it, and its box.
 *
 * @details
 * - What this owns is the half a pass cannot: the content scene (thin bars, one geometry for all three
 *   digits), the static framing camera, the BOX (a device-pixel rectangle anchored to the bottom-right
 *   corner of the area it belongs to) and the measurement. It is deliberately NOT a pass - whoever
 *   shows it declares one (see PassDesc): `content` = content(), `camera` = camera(), `viewport` =
 *   boxViewport(), `before_draw` = update().
 * - update() feeds the wall-clock frame delta between calls to the @ref Sampler, which counts FRAMES
 *   over a window of wall time (default 0.5 s) and publishes a figure about twice a second; a new
 *   figure rewrites the row of bars. No source camera is needed: the digits stay pinned to their
 *   corner while the scene camera orbits.
 * - WHY THE FIGURE IS A WINDOW AVERAGE, AND NOTHING IS BLENDED INTO IT. The first version smoothed
 *   the instantaneous rate 1/dt with a per-frame EMA (weight 0.2) and repainted every 0.15 s. That
 *   reports a quantity no interval ever had, and both halves of it are wrong for a readout whose
 *   whole job is to say what the loop really did:
 *   - 1/dt is one frame's sample, not a rate, and the EMA's time constant is 1/(0.2 * fps) - about
 *     83 ms at 60 fps and 25 ms at 200 fps, i.e. it depends on the very number it displays;
 *   - measured on a real step (60 fps for 3 s, then 30 for 3 s) it printed 60, 60, **40, 33**, 31, 30 -
 *     values that never happened - while the window average prints 60, 60, 60, 60, 60, **54**, 30, 30:
 *     the one straddling window shows the true average of a half-second that really was part 60 and
 *     part 30, and nothing else is invented;
 *   - measured on a drag-shaped stream (60 fps nominal, a quarter of the frames 1.5-2.5x slow) it
 *     changed 75 times in 10 s with single steps up to 17 fps - jumping faster than a rate can
 *     meaningfully change.
 * - So the figure IS the last window's exact average (frames / elapsed over 0.5 s by default, about
 *   two figures a second): every published number is the truth about its own interval, a real change
 *   appears within one window, and a 50 ms stall inside a 60 fps window is reported at its true cost
 *   (30 frames over 0.533 s = 56.25), not hidden. window_seconds IS the sampling interval and the only
 *   trade-off a host can tune: a longer window estimates more steadily but takes longer to reflect a
 *   real change.
 * - The row is ONE geometry with ONE material, so the whole readout is one draw — the shape a HUD has
 *   everywhere else. Seven bars per digit as seven nodes with seven materials was 7 to 21 draw
 *   commands (and as many material blocks) for what is at most 21 flat bars. A bar its digit does not
 *   light is collapsed onto a single point instead of being drawn: it covers no pixels, so the picture
 *   is the one the previous per-segment visibility gate produced (no dark "8" behind the number),
 *   while the vertex count never changes — which is what lets a change be served as ONE in-place
 *   stream refresh (Geometry::bumpRevision) rather than a geometry rebuild. A change therefore costs
 *   one packed position buffer and one revision bump, whatever the digits are.
 * - The overlay needs no source camera: it uses its own static framing camera, so the digits stay
 *   pinned to the corner while the scene camera orbits. Surface layout is creator-managed — report the
 *   surface size on every resize by calling onSurfaceResized(w, h) (e.g. via a RenderPipeline::resize() /
 *   SceneView surface-layout step), which re-anchors the box to the bottom-right corner.
 *
 * Example:
 * \code
 * auto fps = make_intrusive<FpsOverlay>();
 * fps->setPixelRatio(view->devicePixelRatio());
 * PassDesc hud;
 * hud.name = u8"fps_overlay"; hud.tag = u8"Hud"; hud.stage = PipelineStage::Overlay;
 * hud.camera = intrusive_ptr<Camera>(fps->camera());
 * hud.content = intrusive_ptr<Scene>(fps->content());
 * hud.viewport = fps->boxViewport(); hud.clear_enabled = false; hud.depth_mode = DepthMode::Disabled;
 * hud.before_draw = [fps] { fps->update(); };
 * engine->addPass(hud);
 * \endcode
 */
class VN_GRAPHICS_API FpsOverlay : public Object, public RefCounted<FpsOverlay> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a readout whose digits are hidden until the first measurement.
     *
     * @details
     * - The pass never clears (it draws over the previous content). Its sub-viewport defaults to a box
     *   at the origin corner until its owner reports a surface size (see onSurfaceResized).
     */
    FpsOverlay();

    /** @brief Destroys the overlay. */
    ~FpsOverlay() override;

    /** @brief Sets the ratio between logical surface size and device pixels.
     *
     * @details
     * - The readout is positioned in device pixels because the render backend draws into a native
     *   surface sized in device pixels. Qt reports logical sizes, so hosts on high-DPI displays must
     *   supply their devicePixelRatio (default 1).
     *
     * @param ratio Device pixel ratio (> 0).
     */
    void setPixelRatio(double ratio);

    /** @brief Sets the on-screen box size in device pixels.
     *
     * @details
     * - The box is wide (a 3-digit row); its aspect also frames the overlay camera so the digits fill
     *   the box without distortion. Defaults to a compact 105 x 36 box (bottom-right); see setSize.
     *
     * @param width  Box width in device pixels (default 105).
     * @param height Box height in device pixels (default 36).
     */
    void setSize(int width, int height);

    /** @brief Gets the content scene drawn by the overlay. */
    raw_ptr<Scene> content() const;

    /** @brief Gets the camera that frames the digits (the one a declaration states as its camera). */
    raw_ptr<Camera> camera() const;

    /** @brief Gets the device-pixel rectangle the digits are drawn into (the viewport a declaration states).
     *
     * @details
     * - The box follows the surface (onSurfaceResized) and the area origin (setAreaOrigin), and is
     *   scaled down when the area is too small to hold it.
     *
     * @return The box, in device pixels, top-left origin.
     */
    [[nodiscard]] Viewport boxViewport() const;

    /** @brief Turns a stream of frame deltas into the figure the readout shows.
     *
     * @details
     * - Frames are COUNTED over one window of wall time (window_seconds) and the figure published when
     *   the window closes is frames / elapsed - the exact average of that interval, with nothing blended
     *   into it (see the class note: a blend would print values no interval ever had and lag real
     *   changes). At the default 0.5 s a human gets about two figures a second, and the caller only
     *   repaints when the ROUNDED value changes, so a steady frame rate costs no data work at all.
     */
    struct VN_GRAPHICS_API Sampler
    {
        /** @brief Seconds one published figure averages over: the sampling interval (must be > 0).
         *
         * @details
         * - The only trade-off there is: a longer window estimates more steadily and takes longer to
         *   reflect a real change; a shorter one is the reverse. Every figure is the truth about its
         *   own window.
         */
        double window_seconds{ 0.5 };

        /** @brief Adds one frame's wall time; true when a window closed and a figure is ready. */
        bool addFrame(double dt_seconds) noexcept;

        /** @brief Gets the figure published last, in frames per second (0 before the first window closes). */
        [[nodiscard]] double fps() const noexcept;

      private:
        double        elapsed_seconds_{ 0.0 };
        std::uint32_t frames_{ 0 };
        double        published_{ 0.0 };
    };

    /** @brief Gets the readout's measurement policy (the sampling interval).
     *
     * @details
     * - Exposed because that number IS the readout's behaviour: a host can make it steadier (a longer
     *   window) or quicker to follow a change (a shorter one), and a test can drive the sampling with no
     *   scene at all.
     */
    [[nodiscard]] Sampler& sampler() noexcept;

    /** @brief Gets the readout's measurement policy (see the non-const overload). */
    [[nodiscard]] const Sampler& sampler() const noexcept;

    /** @brief Re-anchors the overlay viewport to the bottom-right corner for a new surface size.
     *
     * @details
     * - Called by the overlay's owner whenever the surface changes (e.g. from a SceneView surface-layout
     *   step / RenderPipeline::resize()). The size is the LOGICAL surface size the host reports - the same
     *   number it hands to SceneView::resize - and the configured devicePixelRatio is applied HERE,
     *   which is what turns it into the device-pixel rectangle a backend viewport needs. Handing it
     *   device pixels scales the box twice.
     *
     * @param width  Surface width in logical pixels.
     * @param height Surface height in logical pixels.
     */
    void onSurfaceResized(int width, int height);

    /** @brief Sets the origin of the area this readout lays its box out in, in device pixels.
     *
     * @details
     * - The readout anchors to the bottom-right corner of its AREA, which starts at (0, 0) by default -
     *   the whole surface. A view that draws into a part of a shared surface (a split view) points this
     *   at that part's origin so the readout lands inside it.
     *
     * @param x Area origin x in device pixels.
     * @param y Area origin y in device pixels.
     */
    void setAreaOrigin(int x, int y);

    /** @brief Does the overlay's per-frame work: measure the frame rate and repaint the digits.
     *
     * @details
     * - Measures the wall clock since the previous call and feeds it to the sampler (see the class note),
     *   then writes the figures that changed. It touches no backend, which is what lets the pipeline
     *   state it as the `before_draw` hook of a declaration (PassDesc).
     */
    void update();

  private:
    /** @brief Rebuilds the seven-segment content scene. */
    void rebuild();

    /** @brief Feeds one frame delta to the sampler and repaints the row when a window closes.
     *
     * @details
     * - Writes the shown digits into the row of bars (see writePattern) and does nothing while the value
     *   does not change, so a steady frame rate costs no data work at all.
     *
     * @param dt Seconds since the previous call.
     */
    void updateReadout(double dt);

    /** @brief Writes the segments @p pattern lights into the row of bars.
     *
     * @details
     * - Bar i of the row is bit i of the pattern: digit d occupies bits d * 7 .. d * 7 + 6, bit 0 being
     *   segment a, the way a seven-segment decoder orders them. A lit bar keeps the box the template
     *   holds, an unlit one is collapsed onto its first corner: the geometry's positions are rewritten
     *   and the revision bumped, so the backend serves the edit in place.
     *
     * @param pattern The 21 segment bits to light.
     */
    void writePattern(std::uint32_t pattern);

    // Framing camera (owned, and stated by the declaration that shows this overlay).
    intrusive_ptr<Camera> camera_;
    // Content scene holding the row of bars (owned).
    intrusive_ptr<Scene> content_;
    // The box the digits are drawn into, in device pixels (see boxViewport).
    Viewport box_{};
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
    int    area_x_ = 0;
    int    area_y_ = 0;

    // The frame clock the sampler is fed from, the sampler itself, and the value the row currently shows.
    std::chrono::steady_clock::time_point last_tick_{};
    Sampler                               sampler_{};
    int                                   shown_value_ = -1;
};

using FpsOverlayPtr = intrusive_ptr<FpsOverlay>;

VN_GRAPHICS_NS_END
