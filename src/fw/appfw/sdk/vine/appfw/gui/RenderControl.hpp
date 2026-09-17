#pragma once

#include <vine/intrusive_ptr.hpp>
#include <vine/Signal.hpp>
#include <vine/String.hpp>

#include "Control.hpp"

namespace vine::graphics
{
class RenderEngine;
class SceneView;
}

V_APPFWGUI_NS_BEGIN

/**
 * @brief Render view control: hosts a native QWindow render surface inside a
 * QWidget container and drives a RenderEngine.
 *
 * The actual render surface is a QWindow (the layer a render backend binds
 * to). It is nested inside a QWidget via QWidget::createWindowContainer so it
 * can be hosted by the QWidget-based Control — this keeps Control unchanged
 * (it holds a QWidget) while the render layer gets the native QWindow it
 * needs.
 *
 * RenderControl owns the surface and the wiring: it creates the RenderEngine
 * and a SceneView (the interactive primary view holding the camera, content
 * scene and orbit manipulator) internally and defaults to the first
 * registered render backend on init() (an explicit backend can be attached
 * via engine()->setBackend() first); Qt events on the surface and its host
 * widget are translated and pushed to the view (view()->pushEvent()) for its
 * camera manipulator.
 *
 * THE CONTROL DRIVES ITS OWN LIFECYCLE. A render surface can only be attached
 * once its native window exists and has been laid out, which is later than the
 * moment the host builds its UI - and whether that has happened yet is not
 * something a host can guess, so it does not have to: the control watches its
 * own surface and window events and attaches by itself, retrying on a short
 * backoff ladder while the window is still settling. Adding a RenderControl to a
 * layout is therefore enough; init() remains as the explicit entry point for a
 * host that turned auto-initialization off, and is idempotent either way.
 *
 * Until then the native surface stays HIDDEN, and the host's layout shows the
 * container's own background: an unpresented native window is a hole that the
 * compositor fills with whatever it likes, which is what used to make the render
 * area flicker transparent at startup. Attaching does not need the surface to be
 * shown (a window system that disagrees fails the attach, and the control falls
 * back to showing it first - see state()), so the surface appears only once a
 * frame can be put into it, and the whole startup path is observable through
 * state() / stateChanged.
 */
class V_APPFW_API RenderControl : public Control {
    V_OBJECT_META_DECL;

  public:
    /**
     * @brief Lifecycle of the render surface, reported by state().
     *
     * The order is the order the states are reached in a healthy session:
     * Pending -> Attached -> Presenting. Failed replaces the tail when the
     * backend cannot come up at all.
     */
    enum class SurfaceState
    {
        /// No usable native surface yet, or the host asked to wait; nothing is attached.
        Pending,
        /// The backend is bound to a live surface, but nothing has reached the screen yet.
        Attached,
        /// A frame has been handed to a visible surface: the area shows render output.
        Presenting,
        /// The surface is usable but the backend would not initialize; see failureReason().
        Failed,
    };

  public:
    RenderControl();
    ~RenderControl() override;

  public:
    /** @brief Gets the render engine created by this control.
     *
     * The caller attaches a render backend to it (engine()->setBackend(...))
     * before calling init().
     *
     * @return The engine, or nullptr when creation failed.
     */
    vine::graphics::RenderEngine* engine() const;

    /** @brief Gets the interactive primary view created by this control.
     *
     * The view owns this control's camera, content scene and orbit
     * manipulator, and registers the window pass that presents them (see
     * SceneView). Application code fills the content scene via
     * view()->scene() and reads the camera via view()->camera() when it
     * assembles an explicit pipeline.
     *
     * @return The view (never null while the control is alive).
     */
    vine::graphics::SceneView* view() const;

    /** @brief Wires the native surface into the engine and initializes it.
     *
     * Called by the control's own lifecycle as soon as the surface is usable (see
     * ensureAttached()), so a host that only embeds the control never calls it. When no
     * backend was attached via engine()->setBackend(), the first registered render
     * backend (RenderBackendRegistry) is used by default. The backend attaches to the
     * native surface, which only has a usable size once the window is shown and laid
     * out. Idempotent and re-entrant: when Qt later destroys and recreates the native
     * surface, the backend is released via the surface-destroy event and the control
     * re-attaches automatically once the new surface is created and laid out (calling
     * init() remains safe). Resize and surface-created handling is deferred until after
     * Qt's layout pass so the native window is at its final size when the swapchain is
     * rebuilt.
     *
     * A host calls it to attach at a moment of its own choosing (with
     * setAutoInitialize(false)), to ask for one more attempt after a Failed state, or to
     * pre-check readiness.
     *
     * @return true once the engine initialized successfully, false when the surface was
     *         not ready yet or the backend refused (the control keeps retrying on its
     *         own while auto-initialization is on).
     */
    bool init();

    /** @brief Renders one frame through the engine. */
    void renderFrame();

    /** @brief Fits the whole scene into the view (falls back to the home
     * view when the scene is empty) and renders a frame. */
    void fitToScreen();

    /** @brief Gets the render surface device pixel ratio.
     *
     * Qt reports widget sizes in logical pixels; a render backend draws into
     * the native surface in device pixels, so HUD / sub-viewport positioning
     * (e.g. the axis gizmo) on high-DPI displays must scale by this factor.
     *
     * @return Device pixel ratio (1.0 when the surface is not available).
     */
    double devicePixelRatio() const;

    /**
     * @brief Gets where the surface lifecycle currently stands.
     *
     * @return The current state, one of SurfaceState.
     */
    SurfaceState state() const;

    /**
     * @brief Gets why the surface reached Failed, or an empty string otherwise.
     *
     * The backend's own explanation goes to the diagnostics channel (which the control forwards
     * to the log), so this is the host-facing summary.
     *
     * @return The failure summary; empty unless state() is Failed.
     */
    String failureReason() const;

    /**
     * @brief Sets whether the control attaches by itself once the surface is usable.
     *
     * On (the default) is what makes embedding the control a one-liner. Off hands the timing to
     * the host, which then drives init() itself - to configure the engine or prepare content
     * first, or to start rendering on a user action. Turning it on later attaches on the next
     * opportunity.
     *
     * @param on true to attach automatically.
     */
    void setAutoInitialize(bool on);

    /**
     * @brief Fired on every surface lifecycle transition, on the application thread.
     *
     * A host that wants its own presentation while the area is not rendering yet (a spinner, a
     * placeholder, a disabled pane) or that wants to report an initialization failure subscribes
     * here instead of guessing a delay; the control's own log line already carries the timings.
     */
    Signal<SurfaceState> stateChanged;

  private:
    /** @brief Publishes a lifecycle transition (silent when the state is unchanged). */
    void setState(SurfaceState next);

    /** @brief Attaches now when auto-initialization is on and nothing is attached yet. */
    void ensureAttached();

    /** @brief Schedules one more attach attempt after the backend refused. */
    void scheduleAttachRetry();

    /** @brief Whether the native surface exists and has a real size to attach to. */
    bool surfaceUsable() const;

    /** @brief Shows or hides the native surface (hidden until a frame can go into it). */
    void setSurfaceShown(bool shown);

    /** @brief Records a permanent attach failure and publishes it. */
    void failAttach(String reason);

    /** @brief One-time wiring of the default backend and the surface/host
     * event filter. Idempotent. */
    void wireEvents();

    /** @brief Handles native-surface destruction (Qt recreated the HWND):
     * releases the backend and clears the attach state. */
    void onSurfaceDestroyed();

    /** @brief Handles a surface resize/creation notice by scheduling a
     * deferred update. */
    void onSurfaceResized();

    /** @brief Coalesces and defers a surface update until after Qt's layout
     * pass. */
    void scheduleSurfaceUpdate();

    /** @brief Rebuilds/resizes the backend for the final surface size and
     * requests settle frames. */
    void handleSurfaceUpdate();

    /** @brief Requests a few extra display-synced frames after a resize. */
    void requestSettleFrames();

    /** @brief Renders one display-synced settle frame (UpdateRequest). */
    void onSurfaceUpdate();

    /** @brief Initializes the engine once the native surface is exposed. */
    void initializeBackend();

    /** @brief Recreates the native render surface, forcing the host's follow path.
     *
     * ONLY a test hatch (VINE_RECREATE_SURFACE_MS, see init()): a window-system recreation -- a screen
     * change, a reparent, a dock drag-out -- cannot be produced on demand from outside the process, and the
     * host's answer to it (re-announce the new handle; the backend moves or rebuilds) is the thing worth
     * gating. The backend's own self-test covers ITS half (VsgRenderer::moveSessionToHostSurface); this
     * covers the host's.
     */
    void recreateSurface();

    /** @brief Pops up the view context menu at the cursor position. */
    void showContextMenu();

    /** @brief Gets the native handle of the render surface (HWND on Windows). */
    void* nativeHandle() const;

    /** @brief Gets the render surface width (Qt logical pixels, as the engine is told it). */
    int surfaceWidth() const;

    /** @brief Gets the render surface height (Qt logical pixels, see surfaceWidth). */
    int surfaceHeight() const;

    struct Impl;
    Impl* const d;
};

V_APPFWGUI_NS_END
