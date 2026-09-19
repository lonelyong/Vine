#pragma once

#include <functional>

#include <QWindow>

#include <vine/String.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/window/InputEvent.hpp>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QObject;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;
class QWidget;

namespace vine::graphics
{
class RenderEngine;
class SceneView;
}

V_APPFWGUI_NS_BEGIN

/**
 * @brief The render surface and the render session living on it.
 *
 * The native window a render backend binds to - a QWindow created as a Vulkan surface - plus
 * everything that keeps a session on it: the engine and the interactive primary view, the attach
 * and re-announce rules, the deferred resize path, the display-synced settle frames, and the
 * translation of the Qt events it receives into vine::window events pushed to the view.
 *
 * RenderControl embeds one of these in its widget tree (QWidget::createWindowContainer) and
 * forwards its public surface to the outside; no decision about the session is taken there. Every
 * lifecycle transition is reported through on_state_changed, which the control re-publishes as
 * its own stateChanged.
 *
 * The host gives the timing, the surface maintains the session: nothing is attached until
 * RenderControl::init() asks for it, and an established session follows a platform window Qt
 * destroyed and recreated on its own.
 *
 * @note This is a private header: the class is an implementation detail of RenderControl and is
 * not installed. The lifecycle lines it logs keep the "[RenderControl]" tag - the surface is that
 * control's surface, and the tag is what the log greps and the design notes search for.
 */
class SurfaceWindow : public QWindow {
  public:
    /// The lifecycle states published through on_state_changed; see RenderControl::SurfaceState.
    using SurfaceState = RenderControl::SurfaceState;

  public:
    /**
     * @brief Creates a hidden surface and the engine and view that render into it.
     *
     * @param host Widget the surface is embedded in. Asked whether the control is on screen, which
     *             the surface cannot answer for itself (its own flags report visible while the
     *             hosting window is hidden); also the parent of the context menu.
     */
    explicit SurfaceWindow(QWidget* host);
    ~SurfaceWindow() override;

  public:
    /**
     * @brief Gets the render engine this surface drives.
     *
     * @return The engine, or nullptr when creation failed.
     */
    vine::graphics::RenderEngine* engine() const;

    /**
     * @brief Gets the interactive primary view bound to the engine.
     *
     * @return The view (never null while the surface is alive).
     */
    vine::graphics::SceneView* view() const;

    /**
     * @brief Attaches the backend to the live native surface and initializes it.
     *
     * @return true once the engine initialized successfully, false when the surface was not ready
     *         yet, no render backend is registered, or the backend refused the attach.
     */
    bool init();

    /** @brief Renders one frame through the engine. */
    void renderFrame();

    /** @brief Fits the whole scene into the view (falling back to the home view when the scene is
     * empty) and renders a frame. */
    void fitToScreen();

    /**
     * @brief Gets where the surface lifecycle currently stands.
     *
     * @return The current state.
     */
    SurfaceState state() const;

    /**
     * @brief Gets why the surface reached Failed, or an empty string otherwise.
     *
     * @return The failure summary; empty unless state() is Failed.
     */
    String failureReason() const;

  public:
    /// Fired on every lifecycle transition, on the application thread. The control re-publishes it;
    /// a surface without a listener simply keeps its own state.
    std::function<void(SurfaceState)> on_state_changed;

  protected:
    /** @brief Reports the platform-surface phases and the display-synced update ticks. */
    bool event(QEvent* event) override;
    /** @brief Reports a resize of the window itself. */
    void resizeEvent(QResizeEvent* event) override;
    /** @brief Re-asserts the visibility rule after Qt showed the window. */
    void showEvent(QShowEvent* event) override;
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
    void handleMouse(const vine::window::MouseEvent& event);
    /** @brief Pushes a scroll event to the view and refreshes the frame. */
    void handleScroll(const vine::window::ScrollEvent& event);
    /** @brief Pushes a key event to the view and refreshes the frame. */
    void handleKey(const vine::window::KeyEvent& event);
    /** @brief Reports the client size once per change, whichever object saw it first. */
    void handleResized(int width, int height);
    /** @brief Marks the native surface usable and schedules the update that follows it. */
    void noteSurfaceUsable();
    /** @brief Handles native-surface destruction: marks the surface unusable and hides it until the
     * backend is bound to the new one. */
    void handleDestroyed();
    /** @brief Re-asserts the surface's visibility after Qt showed it with its container. */
    void handleShown();
    /** @brief Coalesces and defers a surface update until after Qt's layout pass. */
    void scheduleUpdate();
    /** @brief Settles a deferred surface update: follows a recreated platform window onto its new
     * surface, or resizes the backend to the final surface size and requests settle frames. */
    void handleUpdate();
    /** @brief Renders one display-synced settle frame (UpdateRequest). */
    void handleUpdateTick();
    /** @brief Requests a few extra display-synced frames after a resize. */
    void requestSettleFrames();
    /** @brief Binds the backend to the live native surface (and re-announces a new handle). */
    void initializeBackend();
    /** @brief Picks the first registered render backend unless the host attached one. Idempotent. */
    void useDefaultBackend();
    /** @brief Publishes a lifecycle transition (silent when the state is unchanged). */
    void setState(SurfaceState next);
    /** @brief Shows or hides the native surface (hidden until a frame can go into it). */
    void setSurfaceShown(bool shown);
    /** @brief Records a permanent attach failure and publishes it. */
    void failAttach(String reason);
    /** @brief Pops up the view context menu at the cursor position. */
    void showContextMenu();
    /** @brief Gets the native handle of the render surface (HWND on Windows). */
    void* nativeHandle() const;

    /** @brief Recreates the native render surface, forcing the host's follow path.
     *
     * ONLY a test hatch (VINE_RECREATE_SURFACE_MS, see init()): a window-system recreation - a screen
     * change, a reparent, a dock drag-out - cannot be produced on demand from outside the process,
     * and the host's answer to it (re-announce the new handle; the backend moves or rebuilds) is the
     * thing worth gating.
     */
    void recreateSurface();

    struct Impl;
    Impl* const d;
};

V_APPFWGUI_NS_END
