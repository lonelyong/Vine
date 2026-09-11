#pragma once
#include "graphics_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "RenderPass.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class Scene;

/**
 * @brief Legacy helper pairing a content scene with a render pass (HUD).
 *
 * An overlay bundles a content scene with a render pass (camera, optional
 * sub-viewport and clear policy) and a per-frame update hook. The pass can be
 * registered with a RenderEngine like any other pass — HUD content is just a
 * pass at a high order (see the AxisGizmo / FpsOverlay HUD passes, which are
 * RenderPass subclasses and the recommended shape for new HUD content).
 *
 * NOTE: this class predates the pure-scheduler engine and is NOT wired into
 * it. There is no RenderEngine::addOverlay / removeOverlay and no
 * RenderBackend::releaseOverlay hook any more, so the overlay's pass must be
 * registered / released through the engine's ordinary pass API by the caller,
 * and no engine code calls update() or onSurfaceResized() for you. Treat it as
 * a convenience container only.
 */
class V_GRAPHICS_API Overlay : public Object, public RefCounted<Overlay> {
    V_OBJECT_META_DECL;

  public:
    /** @brief How the overlay camera tracks a source camera. */
    enum class MirrorMode {
        /// Camera is fully independent (e.g. a minimap or a 2D screen HUD).
        None = 0,
        /// Copy only the source orientation; keep the overlay's own framing.
        Orientation,
        /// Adopt the source view (eye / target / up) completely.
        FullView,
    };

    /** @brief Constructs an overlay with an empty pass and content scene.
     *
     * The pass has clearing disabled so the overlay draws over the previous
     * frame instead of erasing it.
     */
    Overlay();

    /** @brief Destroys the overlay. */
    ~Overlay() override;

    /** @brief Per-frame update hook.
     *
     * The default implementation applies the configured mirror mode. Derived
     * overlays override this to refresh animated or data-driven content.
     *
     * @param dt Seconds elapsed since the previous frame.
     */
    virtual void update(double dt);

    /** @brief Gets the pass that draws this overlay. */
    raw_ptr<RenderPass> pass() const;

    /** @brief Sets the pass that draws this overlay.
     *
     * The overlay draws through this pass (content scene, clear policy and
     * optional sub-viewport). Registering the pass with a RenderEngine is the
     * caller's job (addPass): the engine keeps the pass alive and asks the
     * backend to release its GPU state when the pass is removed. The overlay
     * keeps a reference.
     *
     * @param pass Render pass, or null to clear.
     */
    void setPass(intrusive_ptr<RenderPass> pass);

    /** @brief Gets the content scene drawn by this overlay. */
    raw_ptr<Scene> content() const;

    /** @brief Sets the content scene drawn by this overlay.
     *
     * The overlay keeps a reference.
     *
     * @param content Content scene, or null to clear.
     */
    void setContent(intrusive_ptr<Scene> content);

    /** @brief Gets the draw order relative to other overlays (lower first). */
    int zOrder() const;

    /** @brief Sets the draw order relative to other overlays (lower first). */
    void setZOrder(int order);

    /** @brief Returns whether the overlay is drawn this frame. */
    bool visible() const;

    /** @brief Sets whether the overlay is drawn. */
    void setVisible(bool visible);

    /** @brief Sets how the overlay camera tracks a source camera.
     *
     * @param mode Mirror mode.
     */
    void setMirrorMode(MirrorMode mode);

    /** @brief Gets the configured mirror mode. */
    MirrorMode mirrorMode() const;

    /** @brief Sets the camera the overlay mirrors (non-owning).
     *
     * Usually the engine's main camera.
     *
     * @param camera Source camera, or null to disable mirroring.
     */
    void setSourceCamera(raw_ptr<Camera> camera);

    /** @brief Hook for the host to report a rendering-surface resize.
     *
     * The engine does not call this (it manages no overlay layout): a host that
     * uses an Overlay drives its own surface-layout step, e.g. through
     * SceneView::addSurfaceLayout, and calls this when the surface changes.
     *
     * @param width  New surface width in pixels.
     * @param height New surface height in pixels.
     */
    virtual void onSurfaceResized(int width, int height) {}

  protected:
    /** @brief Applies the configured mirror mode onto the overlay camera. */
    void applyMirror();

  private:
    RenderPassPtr pass_;
    intrusive_ptr<Scene> content_;
    int z_order_ = 0;
    bool visible_ = true;
    MirrorMode mirror_mode_ = MirrorMode::None;
    raw_ptr<Camera> source_camera_ = nullptr;  ///< Non-owning source camera.
};

using OverlayPtr = intrusive_ptr<Overlay>;

V_GRAPHICS_NS_END
