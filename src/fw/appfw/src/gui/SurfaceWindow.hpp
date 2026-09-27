#pragma once

#include <functional>

#include <QWindow>

#include <vine/String.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/async/Task.hpp>
#include <vine/window/InputEvent.hpp>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QObject;
class QResizeEvent;
class QWheelEvent;
class QWidget;

namespace vn::graphics
{
class RenderEngine;
class SceneView;
}

VN_APPFWGUI_NS_BEGIN

/**
 * @brief The render surface and the render session living on it.
 *
 * The native window a render backend binds to, plus what keeps a session on it: the engine and its view, the
 * attach / re-announce rules, the deferred resize path, and the Qt events translated into view input. RenderControl
 * embeds one inside a window container and re-publishes its state_changed; every lifecycle transition is reported
 * through on_state_changed.
 *
 * The surface attaches itself: the constructor tries, and a show/resize of the host retries (see handleUpdate()).
 * An established session then follows a platform window Qt replaced on its own, and nothing is re-checked on a
 * timer. The surface does not manage its own visibility - the control shows the container it lives in exactly when
 * a frame is in the surface.
 *
 * @note Private header: an implementation detail of RenderControl, not installed. The lifecycle lines it logs keep
 * the "[RenderControl]" tag, which is what the log greps search for.
 */
class SurfaceWindow : public QWindow {
  public:
    /// The lifecycle states published through on_state_changed; see RenderControl::SurfaceState.
    using SurfaceState = RenderControl::SurfaceState;

  public:
    /**
     * @brief Creates the surface, its engine and its view, and tries the attach once.
     *
     * @param host Widget the surface is embedded in, and the parent of the context menu. Its show - and the runtime
     *             container's show/resize - is what the attach is retried on.
     */
    explicit SurfaceWindow(QWidget* host);
    ~SurfaceWindow() override;

  public:
    /**
     * @brief Gets the render engine this surface drives.
     *
     * @return The engine, or nullptr when creation failed.
     */
    vn::graphics::RenderEngine* engine() const;

    /**
     * @brief Gets the interactive primary view bound to the engine.
     *
     * @return The view (never null while the surface is alive).
     */
    vn::graphics::SceneView* view() const;

    /**
     * @brief Attaches the backend to the live native surface and opens the session on it.
     *
     * The whole attach: the handle goes to the backend (a re-announce when Qt replaced the platform window), the
     * backend's initialize() builds the device, the session and every pipeline, the size goes to backend and view,
     * and the attach's one frame is rendered. Synchronous, on the calling thread, idempotent while the session is
     * bound to the surface the window reports now.
     *
     * @return true once the session is up; false when the surface is not ready yet (the next update retries, see
     *         handleUpdate()), no render backend is registered, or the backend refused the attach.
     */
    bool init();

    /**
     * @brief The awaitable spelling of init(), for a caller that is already a coroutine.
     *
     * The attach is synchronous, so this completes without suspending. It reads the scene graph (the pipelines come
     * from the registered passes, and the frame records it), so a host installs its content after it - automatic
     * here, since the attach returns before the call that started it does.
     *
     * @return A task that completes with the same answer as init().
     */
    vn::async::Task<bool> initAsync();

    /** @brief Renders one frame through the engine (the attach's own frame is the first one).
     *
     * The control shows its render area on the transition this publishes, so the surface ends up on screen once a
     * frame is in it. */
    void renderFrame();

    /** @brief Fits the whole scene into the view (home view when the scene is empty) and renders a frame. */
    void fitToScreen();

    /** @brief Gets where the surface lifecycle currently stands. */
    SurfaceState state() const;

    /** @brief Gets why the surface reached Failed (empty otherwise). */
    String failureReason() const;

  public:
    /// Fired on every lifecycle transition, on the application thread (RenderControl re-publishes it).
    std::function<void(SurfaceState)> on_state_changed;

    /// Fired when the platform refused a surface that is not on screen, so the control shows it before the retry.
    std::function<void()> on_needs_visible_surface;

  protected:
    /** @brief Reports the phases of the native platform surface. */
    bool event(QEvent* event) override;
    /** @brief Reports a resize of the window itself. */
    void resizeEvent(QResizeEvent* event) override;
    /** @brief Reports a resize or a show of the hosting container widget. */
    bool eventFilter(QObject* watched, QEvent* event) override;
    /** @brief Reports a mouse button press. */
    void mousePressEvent(QMouseEvent* event) override;
    /** @brief Reports a mouse button release. */
    void mouseReleaseEvent(QMouseEvent* event) override;
    /** @brief Reports a mouse motion (no button: that is how a drag differs from a hover). */
    void mouseMoveEvent(QMouseEvent* event) override;
    /** @brief Reports a wheel notch. */
    void wheelEvent(QWheelEvent* event) override;
    /** @brief Reports a key press. */
    void keyPressEvent(QKeyEvent* event) override;
    /** @brief Reports a key release. */
    void keyReleaseEvent(QKeyEvent* event) override;

  private:
    /** @brief Pushes a mouse event to the view, refreshes the frame, and opens the context menu on
     * a right-click. */
    void handleMouse(const vn::window::MouseEvent& event);
    /** @brief Pushes a scroll event to the view and refreshes the frame. */
    void handleScroll(const vn::window::ScrollEvent& event);
    /** @brief Pushes a key event to the view and refreshes the frame. */
    void handleKey(const vn::window::KeyEvent& event);
    /** @brief Reports the client size once per change, whichever object saw it first. */
    void handleResized(int width, int height);
    /** @brief Marks the native surface usable and schedules the update that follows it. */
    void noteSurfaceUsable();
    /** @brief Handles native-surface destruction: marks the surface unusable and takes the state back to
     * Pending, so the control stops showing an area that has nothing to show. */
    void handleDestroyed();
    /** @brief Coalesces and defers a surface update until after Qt's layout pass. */
    void scheduleUpdate();
    /** @brief Settles a deferred surface update: follows a recreated platform window onto its new
     * surface, or resizes the backend to the final surface size and renders one frame. */
    void handleUpdate();
    /** @brief Arms the platform-window recreation test hatch (VINE_RECREATE_SURFACE_MS), once per session. */
    void armRecreateHatch();

    /** @brief Picks the first registered render backend unless the host attached one. Idempotent. */
    void useDefaultBackend();
    /** @brief Publishes a lifecycle transition (silent when the state is unchanged). */
    void setState(SurfaceState next);
    /** @brief Records a permanent attach failure and publishes it. */
    void failAttach(String reason);
    /** @brief Pops up the view context menu at the cursor position. */
    void showContextMenu();
    /** @brief Gets the native handle of the render surface (HWND on Windows). */
    void* nativeHandle() const;

    /** @brief Recreates the native render surface, forcing the follow path (test hatch, VINE_RECREATE_SURFACE_MS).
     *
     * A window-system recreation cannot be produced on demand from outside the process, and the answer to it
     * (re-announce the handle) is the thing worth gating. */
    void recreateSurface();

    struct Impl;
    Impl* const d;
};

VN_APPFWGUI_NS_END
