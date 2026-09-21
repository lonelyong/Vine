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
 * This class is the widget-side wrapper: it embeds the surface in its widget
 * tree and forwards the public interface below. The surface itself - the native
 * QWindow a backend binds to, the RenderEngine and the SceneView (the
 * interactive primary view holding the camera, content scene and orbit
 * manipulator), the attach rules, the deferred resize path and the translation
 * of Qt events into view input - lives in the private SurfaceWindow
 * (src/gui/SurfaceWindow.hpp). Unless a backend was attached via
 * engine()->setBackend(), the first backend registered in RenderBackendRegistry
 * is used by default.
 *
 * THE HOST GIVES THE TIMING, THE CONTROL MAINTAINS THE SESSION. Nothing is
 * attached until the host calls init(), at the moment it chooses (right after
 * embedding the control, once the engine has the backend and the pipeline it is
 * meant to run); a call that lands before the native window is laid out reports
 * false instead of guessing, and calling again is the host's retry. What the
 * control keeps for itself is an ESTABLISHED session: when Qt destroys and
 * recreates the native platform window (a dock drag, a screen change, a
 * reparent), the new handle is picked up and re-announced to the backend without
 * the host having to notice.
 *
 * The area this control occupies shows render output exactly when a frame is in the surface: the widget that holds the
 * surface stays HIDDEN until then, and a hidden widget is not painted at all. That is what makes the window hosting
 * this control safe to uncover at any moment - an unpresented native window is a hole (and Qt's window container
 * punches its own rectangle transparent for the native window it holds, so a VISIBLE container without a frame in it
 * is a hole the desktop shows through, whatever a widget would paint there). What is in its place until the first
 * frame is the window's own background, painted by the widgets around this control. Attaching does not need the
 * surface to be visible (a window system that disagrees fails the attach, and the control falls back to showing the
 * area first - see state()), and the first frame of a session is rendered while the area is still hidden: the host's
 * init() called from a plugin's load() lands before the host has laid its widgets out, and that frame is a WARM-UP -
 * it pays the one-time build (pass graphs, program slots, compiled pipelines) at whatever size the platform window
 * has at that moment, and the size change that follows is served in place. The whole path is observable through
 * state() / state_changed.
 */
class V_APPFW_API RenderControl : public Control {
    V_OBJECT_META_DECL;

  public:
    /**
     * @brief Lifecycle of the render surface, reported by state().
     *
     * The order is the order the states are reached in a healthy session:
     * Pending -> Attached -> Presenting. Failed replaces the tail when the
     * backend cannot come up at all. A recreated platform window walks the
     * sequence again from Pending: what changed is the surface, not the session.
     */
    enum class SurfaceState
    {
        /// No usable native surface - not laid out yet, or a platform window that is being replaced -
        /// so nothing is attached to one. The first attach starts here, and a recreation returns here.
        Pending,
        /// The backend is bound to a live surface, but nothing has reached the screen yet (the surface is off
        /// screen until it has something to show - see the class comment).
        Attached,
        /// A frame is in the surface and the surface is on screen from here on: the area shows render output.
        Presenting,
        /// The backend cannot come up at all (no render backend is registered); see failureReason().
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

    /** @brief Attaches the backend to the native surface and initializes it.
     *
     * The host's entry point: call it after putting the control into its window, at the moment it
     * wants the surface up (the engine may still be getting its backend and pipeline). A render
     * surface can only be attached once its native window exists and has been laid out; a call
     * that lands earlier reports false rather than guessing a delay, and calling again is the
     * host's retry - the call is idempotent and cheap when the session is already bound to the
     * live surface. When no backend was attached via engine()->setBackend(), the first registered
     * render backend (RenderBackendRegistry) is used by default.
     *
     * A session that is already established maintains itself: when Qt later destroys and recreates
     * the native platform window, the control re-announces the new handle to the backend on its
     * own (the backend moves, keeping its device and pipelines, or rebuilds when it cannot serve
     * the new window). Resize and surface-created handling is deferred until after Qt's layout
     * pass so the native window is at its final size when the swapchain is rebuilt.
     *
     * @return true once the engine initialized successfully, false when the surface was not ready
     *         yet, no render backend is registered, or the backend refused the attach (the
     *         surface stays hidden in that case).
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
     * @return Device pixel ratio the surface reports.
     */
    double devicePixelRatio() const;

    /**
     * @brief Gets where the surface lifecycle currently stands.
     *
     * @return The current state, one of SurfaceState.
     */
    SurfaceState state() const;

    /**
     * @brief Whether a frame is in the surface, or never will be.
     *
     * This is also the answer to "is the area showing render output rather than the window's own background": true
     * while state() is Presenting, and true once state() is Failed - a backend that cannot come up will not put a
     * frame in the surface either, so a host waiting for a picture is done waiting. Nothing has to wait for it to
     * uncover a window: this control never leaves an empty native window on screen.
     *
     * @return true while state() is Presenting or Failed.
     */
    bool hasPresented() const;

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
     * @brief Fired on every surface lifecycle transition, on the application thread.
     *
     * A host that wants its own presentation while the area is not rendering yet (a spinner, a
     * placeholder, a disabled pane) or that wants to report an initialization failure subscribes
     * here instead of guessing a delay; the control's own log line already carries the timings.
     */
    Signal<SurfaceState> state_changed;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
