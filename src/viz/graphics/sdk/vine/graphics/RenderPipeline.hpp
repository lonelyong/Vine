#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "ShaderProgram.hpp"

V_GRAPHICS_NS_BEGIN

class AxisGizmo;
class Camera;
class FpsOverlay;
class RenderPass;
class RenderTarget;

/**
 * @brief Named presets for the main-window pipeline of a view.
 *
 * A preset is assembled by RenderPipelineBuilder::build(). The shadowed
 * variants are placeholders: the shadow slice (an order < 0 depth-only pass
 * plus shadowed lighting) is not implemented yet, so they currently assemble
 * the same pipeline as their unshadowed counterpart. Asking for one is not
 * silent: the builder reports a DiagnosticCategory::UnsupportedRequest telling
 * the host that the picture it gets is unshadowed.
 */
enum class PipelinePreset {
    Forward,           ///< One window scene pass (order 0) drawing the content.
    ForwardShadowed,   ///< Forward + shadow (placeholder: builds Forward and reports it).
    Deferred,          ///< G-buffer (offscreen MRT) + fullscreen lighting.
    DeferredShadowed,  ///< Deferred + shadow (placeholder: builds Deferred and reports it).
};

/**
 * @brief Optional HUD overlay: a world-orientation axis gizmo.
 *
 * The gizmo is a self-contained HUD pass (AxisGizmo) stacked above the window
 * pass; it mirrors the source camera's orientation each frame. When
 * @ref source_camera is null the overlay is disabled and nothing is added.
 */
struct V_GRAPHICS_API AxisGizmoOptions {
    /** @brief Camera the gizmo mirrors (e.g. the view's primary camera).
     *
     * Null disables the overlay.
     */
    raw_ptr<Camera> source_camera = nullptr;

    /** @brief Ratio between logical surface size and device pixels.
     *
     * Hosts on high-DPI displays supply their devicePixelRatio (default 1).
     */
    double pixel_ratio = 1.0;

    /** @brief Side length of the square gizmo viewport, in device pixels. */
    int box_size = 96;

    /** @brief Stick length in world units (default 1). */
    double axis_length = 1.0;

    /** @brief Stick cross-section half extent in world units (default 0.09). */
    double thickness = 0.09;

    /** @brief Draw order, above the order-0 window pass (default 10). */
    int order = 10;
};

/**
 * @brief Optional HUD overlay: a frame-rate readout in the bottom-right
 * corner.
 *
 * The readout is a self-contained HUD pass (FpsOverlay) stacked above the
 * window pass; it measures the actual render-loop frame rate and needs no
 * source camera. Like the axis gizmo it is opt-in per application — set @ref
 * enabled to true to draw it (the app-shell demo enables it by default).
 */
struct V_GRAPHICS_API FpsOverlayOptions {
    /** @brief Whether the readout is drawn. Disabled by default. */
    bool enabled = false;

    /** @brief Ratio between logical surface size and device pixels.
     *
     * Hosts on high-DPI displays supply their devicePixelRatio (default 1).
     */
    double pixel_ratio = 1.0;

    /** @brief Readout box width in device pixels (default 105). */
    int width_px = 105;

    /** @brief Readout box height in device pixels (default 36). */
    int height_px = 36;

    /** @brief Draw order, above the gizmo and window pass (default 30). */
    int order = 30;
};

/**
 * @brief Options controlling RenderPipelineBuilder::build().
 */
struct V_GRAPHICS_API PipelineOptions {
    /** @brief Off-screen G-buffer size in pixels for the Deferred presets.
     *
     * 0 (the default) uses the engine's current surface size, falling back to
     * a fixed 640 x 360 when the surface is not known yet. The host keeps the
     * G-buffer in step with the surface via Pipeline::resize() (e.g. from a
     * SceneView surface-layout step).
     */
    int offscreen_width = 0;
    int offscreen_height = 0;

    /** @brief Deferred G-buffer geometry program (one scene traversal -> MRT).
     *
     * Must write the canonical four outputs the builder's G-buffer declares
     * (albedo / view normal + shininess / specular / view position; see the
     * builder docs). Optional: when omitted, RenderPipelineBuilder::build()
     * supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> gbuffer_program;

    /** @brief Deferred fullscreen lighting program (samples the G-buffer).
     *
     * Runs as the window pass and shades from the resolved G-buffer
     * attachments. Optional: when omitted, RenderPipelineBuilder::build()
     * supplies its built-in temporary default program.
     */
    intrusive_ptr<ShaderProgram> lighting_program;

    /** @brief Optional axis-gizmo HUD overlay stacked above the window pass.
     *
     * Disabled when AxisGizmoOptions::source_camera is null.
     */
    AxisGizmoOptions gizmo;

    /** @brief Optional frame-rate readout HUD overlay (bottom-right corner).
     *
     * Opt-in (like the gizmo); the app-shell demo enables it by default via
     * FpsOverlayOptions::enabled = true.
     */
    FpsOverlayOptions fps;
};

/**
 * @brief The main-window pipeline produced by RenderPipelineBuilder::build().
 *
 * Owns the passes and targets it created (the target engine also references
 * the passes, so they stay alive either way). Exposes the window-presenting
 * pass and - for deferred pipelines - the off-screen G-buffer, whose size is
 * maintained by its owner through resize() (the backend rebuilds the off-
 * screen attachments whenever the target size changes between frames).
 */
class V_GRAPHICS_API Pipeline : public RefCounted<Pipeline> {
    friend class RenderPipelineBuilder;

  public:
    /** @brief Constructs an empty pipeline (nothing registered). */
    Pipeline();

    /** @brief Destroys the pipeline, releasing its passes and target. */
    ~Pipeline();

  public:
    /** @brief Gets the pass presenting the view camera to the window.
     *
     * @return The order-0 window pass (a ScreenPass for the Deferred
     *         presets), or null when nothing was built.
     */
    raw_ptr<RenderPass> windowPass() const;

    /** @brief Gets the off-screen G-buffer of a Deferred pipeline.
     *
     * @return The G-buffer target, or null for the forward presets.
     */
    raw_ptr<RenderTarget> offscreenTarget() const;

    /** @brief Gets the off-screen composite target of a composite pipeline.
     *
     * A Deferred pipeline that also receives forward-only (transparent /
     * overlay) content bakes an off-screen composite target holding the lit
     * opaque image plus the opaque depth the forward content occludes
     * against; the window pass then presents that composite. Null when the
     * pipeline has no transparent content.
     *
     * @return The composite target, or null when none was created.
     */
    raw_ptr<RenderTarget> compositeTarget() const;

    /** @brief Gets the configured axis-gizmo overlay, if any.
     *
     * @return The gizmo HUD pass, or null when no gizmo was configured.
     */
    raw_ptr<AxisGizmo> gizmo() const;

    /** @brief Gets the configured frame-rate overlay, if any.
     *
     * @return The FPS HUD pass, or null when the readout was disabled.
     */
    raw_ptr<FpsOverlay> fpsOverlay() const;

    /** @brief Resizes the off-screen target and re-anchors the gizmo overlay.
     *
     * Creator-maintained sizing: the host calls this on surface changes (e.g.
     * from a SceneView::addSurfaceLayout step) so a deferred G-buffer tracks
     * the window (the backend rebuilds the off-screen attachments on the next
     * frame) and an axis-gizmo overlay stays pinned to its corner.
     *
     * @param width  New width in pixels (<= 0 is ignored).
     * @param height New height in pixels (<= 0 is ignored).
     */
    void resize(int width, int height);

  private:
    /** @brief Records a produced pass so the pipeline keeps it alive. */
    void retainPass(intrusive_ptr<RenderPass> pass);

    /** @brief Sets the window-presenting pass. */
    void setWindowPass(intrusive_ptr<RenderPass> pass);

    /** @brief Sets the off-screen G-buffer (Deferred presets). */
    void setOffscreenTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the off-screen composite target (Deferred + transparent
     * content). */
    void setCompositeTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Sets the axis-gizmo overlay (optional). */
    void setGizmo(intrusive_ptr<AxisGizmo> gizmo);

    /** @brief Sets the frame-rate overlay (optional). */
    void setFpsOverlay(intrusive_ptr<FpsOverlay> fps);

    std::vector<intrusive_ptr<RenderPass>> passes_;
    intrusive_ptr<RenderPass> window_pass_;
    intrusive_ptr<RenderTarget> offscreen_target_;
    intrusive_ptr<RenderTarget> composite_target_;
    intrusive_ptr<AxisGizmo> gizmo_;
    intrusive_ptr<FpsOverlay> fps_overlay_;
};

V_GRAPHICS_NS_END
