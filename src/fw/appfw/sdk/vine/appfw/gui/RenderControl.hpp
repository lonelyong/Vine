#pragma once

#include <functional>

#include <vine/intrusive_ptr.hpp>
#include <vine/Signal.hpp>
#include <vine/String.hpp>

#include <vine/async/Task.hpp>

#include "Control.hpp"
#include "ViewerOptions.hpp"

namespace vn::graphics
{
class RenderEngine;
class RenderSurface;
class SceneView;
}

VN_APPFWGUI_NS_BEGIN

class Viewer;

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
 * QWindow a backend binds to, the RenderEngine, the SceneView (the interactive
 * primary view holding the camera, content scene and orbit manipulator) and the
 * Viewer that manages the area's panes and their requests - lives in the private
 * SurfaceWindow (src/gui/SurfaceWindow.hpp), together with the attach rules, the
 * deferred resize path and the translation of Qt events into view input. Unless
 * a backend was attached via engine()->setDevice(), the first backend registered
 * in RenderBackendRegistry is used by default.
 *
 * CONSTRUCTION ONLY COMPOSES. The control builds its engine, its view and its viewer and installs the diagnostics
 * route; it does not pick a backend and does not attach. The first attach happens on the first turn of the event loop
 * (a fallback), which is exactly what makes "configure right after new" work: engine()->setDevice(),
 * viewer()->setViewerOptions() (or a host's own view()->setPipeline()) and content assembly are synchronous code and
 * therefore always run BEFORE it. A host that wants the attach inside its
 * own ordering calls init()/initAsync() itself (synchronous, idempotent) - that call IS the attach, and the fallback
 * then finds the session already bound. Window events (show, resize, a recreated platform window) keep the session
 * going afterwards. What the control keeps for itself is an ESTABLISHED session: when Qt destroys and recreates the
 * native platform window (a dock drag, a screen change, a reparent), the new handle is picked up and re-announced to
 * the backend without the host having to notice.
 *
 * The area this control occupies shows render output exactly when a frame is in the surface: the widget that holds the
 * surface stays HIDDEN until then, and a hidden widget is not painted at all. That is what makes the window hosting
 * this control safe to uncover at any moment - an unpresented native window is a hole (and Qt's window container
 * punches its own rectangle transparent for the native window it holds, so a VISIBLE container without a frame in it
 * is a hole the desktop shows through, whatever a widget would paint there). What is in its place until the first
 * frame is the window's own background, painted by the widgets around this control. Attaching does not need the
 * surface to be visible (a window system that disagrees fails the attach, and the control falls back to showing the
 * area first - see state()), and the first frame of a session is rendered by the attach itself, before the host has
 * laid its widgets out: it pays the one-time build (pass graphs, program slots, compiled pipelines) at whatever
 * size the platform window has at that moment, and the size change that follows is served in place. The whole path
 * is observable through state() / state_changed.
 */
class VN_APPFW_API RenderControl : public Control {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Lifecycle of the render surface, reported by state().
     *
     * The order is the order the states are reached in a healthy session: Pending -> Presenting. Failed replaces
     * the tail when the backend cannot come up at all. A recreated platform window walks the sequence again from
     * Pending: what changed is the surface, not the session.
     */
    enum class SurfaceState
    {
        /// No usable native surface (not laid out yet, or a platform window that is being replaced), or nothing
        /// rendered into one yet: the area stays hidden. The first attach starts here, and a recreation returns
        /// here.
        Pending,
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
     * The caller attaches a render backend to it (engine()->setDevice(...))
     * before calling init().
     *
     * @return The engine, or nullptr when creation failed.
     */
    vn::graphics::RenderEngine* engine() const;

    /** @brief Gets the interactive primary view created by this control.
     *
     * The view owns this control's camera, content scene and orbit
     * manipulator. Application code fills the content scene via
     * view()->scene() and reads the camera via view()->camera() when it
     * assembles an explicit pipeline.
     *
     * What the view DRAWS is the pipeline it has adopted, and by default that is this control's viewer pipeline: the
     * control's Viewer builds it (see ViewerOptions) and adopts it on the view before the first attach, so a host
     * that never touches pipelines nevertheless gets a picture. A host that assembles its own adopts it instead
     * (view()->setPipeline(...)) and from then on owns that view's drawing - including rebuilding it (see Pipeline).
     *
     * One control holds one session and one view: a document that renders brings its OWN control (and with it
     * its own surface, device and pipelines - see appfw-document-model.md §9).
     *
     * @return The view (never null while the control is alive).
     */
    vn::graphics::SceneView* view() const;

    /** @brief Gets the viewer managing the panes of this control's render area (see Viewer).
     *
     * ONE control hosts ONE area, and an area may be divided between several views - each with its own request
     * (ViewerOptions), its own rectangle and its own chain, all drawing in the SAME frame on one device. This is
     * the object that manages them: addView() builds a pane's chain from its request and declares it, views()
     * reads which views present this area (from the surface's presentation table), release() gives a pane back
     * to a host that draws it itself. The three calls below (viewerOptions / setViewerOptions / setShadingMode)
     * are the single-pane shortcuts: they address this control's own view, the area's first pane.
     *
     * A host that never splits its area never has to touch it.
     *
     * @return The viewer (never null while the control is alive).
     */
    Viewer* viewer() const;

    /** @brief Gets how this control's own view (the area's first pane) draws (see ViewerOptions).
     *
     * @return The request that pane is built from (the defaults until a host changes them).
     */
    ViewerOptions viewerOptions() const;

    /** @brief Changes how this control's own view (the area's first pane) draws, rebuilding its chain.
     *
     * The one call a host needs to shape the default picture: a chain, an overlay's existence and the mode the
     * chain must express are build decisions (see ViewerOptions), so this is one pipeline build - at construction
     * time, before the first attach, it is also what a viewer's settings should be made with (the attach's first
     * frame then already draws the intended picture). It is the shortcut to Viewer::setViewerOptions on the
     * area's first pane; a host with several panes states each pane's request there.
     *
     * @param options How the pane draws from now on.
     * @return true when the chain was built and adopted; false when the builder refused @p options (the previous
     *         chain then stays in place). Before the first attach there is nothing to refuse it against - a
     *         build that succeeds is adopted and the attach draws with it.
     */
    bool setViewerOptions(const ViewerOptions& options);

    /** @brief Switches the shading mode of this control's own view (the area's first pane).
     *
     * The cheap switch: a mode the built chain can express is one override on the live pipeline, taking effect on
     * the next frame with no rebuild. When the chain cannot express it, the pane is rebuilt from the current
     * request carrying the requested mode. It is the shortcut to Viewer::setShadingMode on the area's first pane.
     *
     * @param mode The viewing mode to switch to.
     * @return true when the pane now shades in @p mode; false when the rebuild failed (the previous pipeline stays
     *         in place). With no chain yet the call builds the pane from its request first, so it is also the way
     *         to state a mode before the first attach.
     */
    bool setShadingMode(vn::graphics::ShadingMode mode);

    /** @brief Gets the presentation table of the surface this control renders into.
     *
     * Which passes draw into this surface, and in which order: the ONE place that answers "who presents this
     * surface". By default the control's viewer does (its first pane - the control's own view - draws it), and a
     * host that builds its own main pass declares THAT pass instead - which is what keeps the default pipeline from
     * being built beside it:
     *
     * @code
     * auto pipeline = builder.build(options);
     * control->surface()->present(pipeline->windowPass());   // before the first attach (install is synchronous)
     * @endcode
     *
     * The control keeps the same fallback it always had: with nothing declared (and no pass declared by a host),
     * the viewer's first pane presents the surface.
     *
     * @return The control's render surface presentation table (never null while the control is alive).
     */
    vn::graphics::RenderSurface* surface() const;

    /** @brief Attaches the backend to the native surface and initializes it.
     *
     * The explicit attach: the session comes up HERE, in this call, and a host that wants its own timing (before the
     * window goes on screen, for an off-screen warm-up frame, or after configuring the view) uses this instead of
     * waiting for the event-loop fallback. This is the entry point promised by the SDK order
     * (construct -> engine()->setDevice() -> init()): idempotent while the session is bound to the live surface,
     * and the retry when an earlier call landed before the native window had been laid out (which reports false
     * rather than guessing a delay). With no backend set through engine()->setDevice(), the first registered render
     * backend (RenderBackendRegistry) is used by default - and only as a fallback: a backend registered later is
     * still picked up by the next attach.
     *
     * A session that is already established maintains itself: when Qt later destroys and recreates the native
     * platform window, the control re-announces the new handle to the backend on its own (the backend moves,
     * keeping its device and pipelines, or rebuilds when it cannot serve the new window). Resize and
     * surface-created handling is deferred until after Qt's layout pass, so the native window is at its final size
     * when the swapchain is rebuilt - and the resize presents one frame at that final size.
     *
     * @return true once the engine initialized successfully, false when the surface was not ready
     *         yet, no render backend is registered, or the backend refused the attach (the
     *         surface stays hidden in that case).
     */
    bool init();

    /** @brief Same attach as init(), in an awaitable shape.
     *
     * The attach itself is synchronous: the native window is the one thing the backend's initialize() needs, and it
     * exists as soon as the surface does, so the device, the session and every pipeline are built where the surface
     * is built. This spelling exists for a caller that is already a coroutine, and it completes without suspending.
     *
     * @return A task that completes with the same answer as init().
     */
    vn::async::Task<bool> initAsync();

    /** @brief Gets the view the user is working in: the surface's FOCUS (see RenderSurface::focus).
     *
     * A press decides it - the pane the pointer went down in, whether the point was routed by the surface's
     * rectangles (the default) or by a router this host installed - and it stays until another press moves it, so
     * a key, a command or a menu action can ask "which view does this mean" without a position to route by.
     *
     * This is the answer for that question; it is NOT a second input router (nothing here routes events). A
     * control whose surface has no view presentation at all (a host that presented its own main pass) answers with
     * its own view - the same fallback input uses.
     *
     * @return The focused view, or this control's own view when nothing (drawable) is focused.
     */
    vn::graphics::SceneView* focusedView() const;

    /** @brief Sets which view receives the surface's input, by position.
     *
     * The control hosts ONE surface, and by default that surface's input goes to the view whose RECTANGLE holds the
     * point: a host that splits the surface states each pane once - `surface()->present(view, rect)` - and input
     * follows from it (RenderSurface::viewAt), with no second statement of the layout to keep in step. A point that
     * no pane holds (or a surface that is not laid out yet) goes to the one view this control built (view()).
     *
     * A host with a rule the rectangles cannot state (an irregular layout, a view that owns a widget's border, a
     * pane that must be inert) overrides that here - one function answering "this point belongs to whom":
     *
     * @code
     * // The rectangles are the layout; this only says where the layout is not enough (a strip that stays inert):
     * control->setInputRouter([&](int x, int y) {
     *     return is_reserved_strip(x, y) ? control->view() : nullptr;   // nullptr = "not mine"
     * });
     * @endcode
     *
     * Semantics (the same the industry uses for a multi-viewport render window - VTK's interactor picks the
     * renderer under the cursor and then keeps it until the button comes up):
     *
     * - **a pointer PRESS routes, and the chosen view is CAPTURED until the button is released.** A drag that
     *   leaves that view's rectangle keeps steering it ("hold and drag" cannot be interrupted by a boundary), and
     *   the control holds a reference to the captured view for the duration, so a host dropping its own reference
     *   mid-drag cannot leave the control steering a freed view. The capture is PER BUTTON: two buttons can drag
     *   two views at once (rotate one pane with the left while the right pans another), and releasing one of them
     *   does not end the other's drag - a view that never saw its release would keep steering its camera on hover.
     *   The release is delivered to the view that STARTED that drag, wherever the pointer is by then.
     * - **the press also moves the FOCUS** (see focusedView): the pane the user pressed in is the pane a key or a
     *   command means from then on. It moves for a routed press and for a routed-by-router one alike.
     * - **hover motion, the wheel and the keyboard** do not capture: they route per event (the wheel), or go to the
     *   captured view when a drag is in flight (motion), or to the focused view, else to the control's own view
     *   (keys - there is no position to route by).
     * - the coordinates are the surface's LOGICAL pixels, top-left origin - the same space control->width() /
     *   height() report (the surface fills this control's widget).
     * - returning nullptr means "not mine": the default routing by rectangle applies, so a router that only claims
     *   the regions it knows about leaves the rest of the surface interactive.
     * - the returned view must belong to this control's engine (view()->engine()). A view bound elsewhere cannot
     *   receive this surface's coordinates; that is reported on the engine's diagnostic route and the event falls
     *   back to the control's own view.
     *
     * @param router Function taking a point (x, y) in logical surface pixels and returning the view that owns it;
     *               an empty function restores the default (the surface's rectangles).
     */
    void setInputRouter(std::function<vn::graphics::SceneView*(int x, int y)> router);

    /** @brief Drives frames on a timer instead of on demand.
     *
     * A control (like every appfw surface) renders ON DEMAND: a frame is produced when something changed - an
     * attach, a resize, an input event, a renderFrame() call. That is what keeps an idle window free, and it is
     * why the frame-rate readout only moves while the pointer does.
     *
     * Continuous rendering is the other mode, for the hosts that need it: an animation, a simulation, a live
     * camera feed, or simply a frame-rate readout that is worth reading. The control then renders one frame per
     * tick, for as long as it is enabled:
     *
     * @code
     * control->setContinuousRendering(true, 16);   // ~60 Hz
     * control->setContinuousRendering(false);      // back to on demand
     * @endcode
     *
     * It is a FRAME DRIVE, not a frame rate guarantee: a tick asks for a frame, and a frame takes as long as it
     * takes (the interval is a period, not a deadline - use RenderControl::renderFrame() plus the engine's frame
     * counters if a host needs to measure real cost). Two conditions make a tick free instead of an invisible
     * cost, both stated rather than silent: no session yet (there is no device to draw with - the attach's own
     * frame brings the first picture), and a native surface that is not exposed (a hidden dock tab or a minimized
     * window). A host that wants frames for a hidden surface drives renderFrame() itself.
     *
     * @param on          True to render one frame per tick, false to stop (the timer is kept, not destroyed).
     * @param interval_ms Tick period in milliseconds, at least 1; <= 0 means the default (16 ms, ~60 Hz).
     */
    void setContinuousRendering(bool on, int interval_ms = 16);

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

VN_APPFWGUI_NS_END
