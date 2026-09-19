#include "SurfaceWindow.hpp"

#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QObject>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurface>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>

#include <vine/window/KeyCode.hpp>
#include <vine/window/MouseButton.hpp>

V_APPFWGUI_NS_BEGIN

namespace
{

/// Shorthand for the lifecycle state this implementation works with.
using State = SurfaceWindow::SurfaceState;

/**
 * @brief Names a surface state for the log.
 *
 * @param state State to name.
 * @return The state's name, without the enum's scope.
 */
const char* stateName(State state)
{
    switch (state) {
        case State::Pending:    return "Pending";
        case State::Attached:   return "Attached";
        case State::Presenting: return "Presenting";
        case State::Failed:     return "Failed";
    }
    return "?";
}

/**
 * @brief Milliseconds elapsed since a time point.
 *
 * @param from Time point to measure from.
 * @return Elapsed milliseconds, rounded down.
 */
long long elapsedMs(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
}

/**
 * @brief Translates Qt keyboard modifiers.
 *
 * @param m Qt modifiers to translate.
 * @return The matching vine modifiers.
 */
vine::window::ModifierKey toModifiers(Qt::KeyboardModifiers m)
{
    using namespace vine::window;
    ModifierKey r = ModifierKey::None;
    if (m & Qt::ShiftModifier) {
        r |= ModifierKey::Shift;
    }
    if (m & Qt::ControlModifier) {
        r |= ModifierKey::Control;
    }
    if (m & Qt::AltModifier) {
        r |= ModifierKey::Alt;
    }
    if (m & Qt::MetaModifier) {
        r |= ModifierKey::Super;
    }
    return r;
}

/**
 * @brief Translates a Qt mouse button.
 *
 * @param b Qt button to translate.
 * @return The matching vine button, or None for a button the framework does not model.
 */
vine::window::MouseButton toMouseButton(Qt::MouseButton b)
{
    using namespace vine::window;
    switch (b) {
        case Qt::LeftButton:   return MouseButton::Left;
        case Qt::RightButton:  return MouseButton::Right;
        case Qt::MiddleButton: return MouseButton::Middle;
        case Qt::XButton1:     return MouseButton::XButton1;
        case Qt::XButton2:     return MouseButton::XButton2;
        default:               return MouseButton::None;
    }
}

/**
 * @brief Translates a Qt key code.
 *
 * @param key Qt key code.
 * @param numpad Whether the key came from the numeric keypad, which decides between the digit row
 *               and the keypad codes.
 * @return The matching vine key code, or Unknown for a key the framework does not model.
 */
vine::window::KeyCode toKeyCode(int key, bool numpad)
{
    using namespace vine::window;
    using KC = KeyCode;

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return static_cast<KC>(static_cast<int>(KC::A) + (key - Qt::Key_A));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return numpad ? static_cast<KC>(static_cast<int>(KC::Numpad0) + (key - Qt::Key_0))
                      : static_cast<KC>(static_cast<int>(KC::D0) + (key - Qt::Key_0));
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return static_cast<KC>(static_cast<int>(KC::F1) + (key - Qt::Key_F1));
    }

    switch (key) {
        case Qt::Key_Space:        return KC::Space;
        case Qt::Key_Return:       return numpad ? KC::NumpadEnter : KC::Enter;
        case Qt::Key_Enter:        return KC::NumpadEnter;
        case Qt::Key_Tab:          return KC::Tab;
        case Qt::Key_Backspace:    return KC::Backspace;
        case Qt::Key_Delete:       return KC::Delete;
        case Qt::Key_Insert:       return KC::Insert;
        case Qt::Key_Home:         return KC::Home;
        case Qt::Key_End:          return KC::End;
        case Qt::Key_PageUp:       return KC::PageUp;
        case Qt::Key_PageDown:     return KC::PageDown;
        case Qt::Key_Left:         return KC::Left;
        case Qt::Key_Right:        return KC::Right;
        case Qt::Key_Up:           return KC::Up;
        case Qt::Key_Down:         return KC::Down;
        case Qt::Key_Shift:        return KC::Shift;
        case Qt::Key_Control:      return KC::Control;
        case Qt::Key_Alt:          return KC::Alt;
        case Qt::Key_Meta:         return KC::Super;
        case Qt::Key_Minus:        return numpad ? KC::NumpadSubtract : KC::Minus;
        case Qt::Key_Equal:        return KC::Equal;
        case Qt::Key_Plus:         return numpad ? KC::NumpadAdd : KC::Equal;
        case Qt::Key_Asterisk:     return numpad ? KC::NumpadMultiply : KC::Unknown;
        case Qt::Key_Slash:        return numpad ? KC::NumpadDivide : KC::Slash;
        case Qt::Key_Period:       return numpad ? KC::NumpadDecimal : KC::Period;
        case Qt::Key_BracketLeft:  return KC::BracketLeft;
        case Qt::Key_BracketRight: return KC::BracketRight;
        case Qt::Key_Backslash:    return KC::Backslash;
        case Qt::Key_Semicolon:    return KC::Semicolon;
        case Qt::Key_Apostrophe:   return KC::Apostrophe;
        case Qt::Key_Comma:        return KC::Comma;
        case Qt::Key_QuoteLeft:    return KC::Grave;
        case Qt::Key_Escape:       return KC::Escape;
        case Qt::Key_Print:        return KC::PrintScreen;
        case Qt::Key_Pause:        return KC::Pause;
        case Qt::Key_Menu:         return KC::Menu;
        case Qt::Key_Context1:     return KC::ContextMenu;
        case Qt::Key_CapsLock:     return KC::CapsLock;
        case Qt::Key_NumLock:      return KC::NumLock;
        case Qt::Key_ScrollLock:   return KC::ScrollLock;
        default:                   break;
    }
    return KC::Unknown;
}

/**
 * @brief Translates a Qt mouse event into the window input event the view consumes.
 *
 * @param event Qt event to translate.
 * @param button Already-translated button the event is about.
 * @param pressed true for a press, false for a release.
 * @return The translated event.
 */
vine::window::MouseEvent toMouseEvent(const QMouseEvent& event, vine::window::MouseButton button, bool pressed)
{
    vine::window::MouseEvent e;
    e.button    = button;
    e.modifiers = toModifiers(event.modifiers());
    e.x         = event.position().x();
    e.y         = event.position().y();
    e.pressed   = pressed;
    return e;
}

/**
 * @brief Translates a Qt key event into the window input event the view consumes.
 *
 * @param event Qt event to translate.
 * @param pressed true for a press, false for a release.
 * @return The translated event.
 */
vine::window::KeyEvent toKeyEvent(const QKeyEvent& event, bool pressed)
{
    const bool numpad = bool(event.modifiers() & Qt::KeypadModifier);
    vine::window::KeyEvent e;
    e.code      = toKeyCode(event.key(), numpad);
    e.modifiers = toModifiers(event.modifiers());
    e.pressed   = pressed;
    e.repeat    = event.isAutoRepeat();
    return e;
}

/**
 * @brief Translates a Qt wheel event into the window scroll event the view consumes.
 *
 * @param event Qt event to translate.
 * @return The translated event.
 */
vine::window::ScrollEvent toScrollEvent(const QWheelEvent& event)
{
    const auto delta = event.angleDelta();
    vine::window::ScrollEvent e;
    // Qt angleDelta is in 1/8-degree units; convert to notches/lines.
    e.deltaX    = delta.x() / 120.0;
    e.deltaY    = delta.y() / 120.0;
    e.modifiers = toModifiers(event.modifiers());
    return e;
}

}  // namespace

struct SurfaceWindow::Impl {
    /// The widget the surface is embedded in: asked whether the control is on screen, which the
    /// surface's own flags cannot answer, and used as the context menu's parent.
    QWidget* host = nullptr;
    vine::intrusive_ptr<vine::graphics::RenderEngine> engine;
    vine::intrusive_ptr<vine::graphics::SceneView> view;
    /// Whether the default backend was already picked (the first init() does it; idempotent).
    bool backend_selected = false;
    // Two flags, two different questions. backend_live: did the last attach attempt succeed (is the
    // backend up?). has_session: is there a session at all (a device and its pipelines built, on
    // session_handle) - which is what keeps the surface following a recreated window after a failed
    // re-announce, and what makes a new handle a re-announce rather than a first attach. backend_live
    // true always implies has_session true (both are set together in initializeBackend()'s success
    // branch); the reverse does not hold.
    bool has_session = false;
    bool backend_live = false;
    // Whether the VINE_RECREATE_SURFACE_MS test hatch has been armed (the first init() arms it).
    bool recreate_hatch_armed = false;
    // Surface lifecycle, published through state()/on_state_changed.
    State state = State::Pending;
    // Why the state reached Failed (empty otherwise).
    String failure_reason;
    // Whether the native surface is shown. It starts hidden: an unpresented native window is
    // a hole the compositor fills with whatever it likes, and attaching does not need it to
    // be visible.
    bool surface_shown = false;
    // Set when the platform refused to attach to a hidden surface (a window system that wants the
    // surface configured first): from then on a failed attempt on a hidden surface shows the surface
    // instead of leaving the area blank, and the log line about it is said once. This is the fallback
    // the whole hidden-surface path exists to avoid.
    bool needs_visible_surface = false;
    // Whether the "waiting for a usable surface" line was already logged.
    bool deferral_logged = false;
    // Whether a mouse button is currently held (drives drag refresh).
    bool mouse_down = false;
    // Right-button click tracking: distinguishes a plain right-click (opens
    // the context menu) from a right-drag (pan).
    bool right_press_active = false;
    double right_press_x = 0.0;
    double right_press_y = 0.0;
    // Whether the native surface currently exists and is laid out (cleared on
    // SurfaceAboutToBeDestroyed, set again on resize/surface-created).
    bool surface_ok = false;
    // Coalesces resize/surface-created notices into one deferred update.
    bool resize_pending = false;
    // Remaining display-synced settle frames owed after the last resize.
    int settle_frames = 0;
    // The native handle the session is bound to (the one backend_live speaks about).
    void* session_handle = nullptr;
    // Last client size reported: a container resize delivers the same final size to both the host
    // widget and this window, and the second one must not rebuild the swapchain again.
    int last_width = -1;
    int last_height = -1;
    // Timings for the lifecycle log lines (construction -> attached -> first frame).
    std::chrono::steady_clock::time_point created_at{};
};

SurfaceWindow::SurfaceWindow(QWidget* host)
  : d(new Impl())
{
    d->host = host;

    // Vulkan surface: Qt does not composite a raster backing store over the render surface, so the
    // Vulkan content stays visible.
    setSurfaceType(QSurface::VulkanSurface);
    // Starts hidden: an unpresented native window is a hole the compositor fills with whatever it
    // likes. Attaching does not need it to be visible - what it needs is a created platform window
    // with a real size, and the size follows the container's layout whether or not the window is
    // shown (measured while hidden on both xcb and offscreen) - so the surface is shown again only
    // once the backend is bound to it (see setSurfaceShown()).
    setVisible(false);

    d->created_at = std::chrono::steady_clock::now();
    d->engine     = vine::intrusive_ptr<vine::graphics::RenderEngine>(
        new vine::graphics::RenderEngine());
    // Design B: RenderEngine starts empty (no passes) and is a pure
    // scheduler - it holds no camera and no content scene. The interactive
    // primary view - its camera, content scene and orbit manipulator - lives
    // in a SceneView that borrows the engine and binds its content to the
    // window pass it registers (SceneView::ensureWindowPass, called in
    // init()).
    d->view = vine::intrusive_ptr<vine::graphics::SceneView>(
        new vine::graphics::SceneView());
    d->view->setEngine(d->engine.get());

    // Backend diagnostics become log records. This is what makes a failing draw
    // visible in a GUI app at all: the backend reports what it could not serve
    // (a rejected geometry, a shader that fell back to the built-in one, an
    // off-screen target it could not build) through the engine, and without a
    // listener that information only ever reaches stderr, which a windowed app
    // never shows. The engine stores the sink, so it also applies to the backend
    // created later by initialize().
    d->engine->setDiagnosticSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        auto&             logger  = vine::logging::defaultLogger();
        const std::string message = diagnostic.message.as_std_str();
        switch (diagnostic.severity) {
            case vine::graphics::DiagnosticSeverity::Error:
                logger.error("[graphics] {}", message);
                break;
            case vine::graphics::DiagnosticSeverity::Warning:
                logger.warn("[graphics] {}", message);
                break;
            case vine::graphics::DiagnosticSeverity::Info:
                logger.info("[graphics] {}", message);
                break;
        }
    });

    // Nothing attaches here: the host calls RenderControl::init() when it wants the surface up - it
    // has just put the control into a window and may still be configuring the engine or building the
    // pipeline. Until then the surface stays hidden; after the attach the surface only follows its
    // own window (see handleDestroyed()/handleUpdate()).
    useDefaultBackend();
}

SurfaceWindow::~SurfaceWindow()
{
    delete d;
}

vine::graphics::RenderEngine* SurfaceWindow::engine() const
{
    return d->engine.get();
}

vine::graphics::SceneView* SurfaceWindow::view() const
{
    return d->view.get();
}

void* SurfaceWindow::nativeHandle() const
{
    return reinterpret_cast<void*>(winId());
}

SurfaceWindow::SurfaceState SurfaceWindow::state() const
{
    return d->state;
}

String SurfaceWindow::failureReason() const
{
    return d->failure_reason;
}

bool SurfaceWindow::init()
{
    // Idempotent: repeated calls are harmless and return the current result -- but only while the session is
    // bound to the surface the window reports NOW: after Qt recreated the platform window this has to
    // re-attach (a re-announce, so the backend can move) rather than report the old attach as current.
    // backend_live already implies has_session (see the Impl flags); the handle comparison is what says
    // whether the live surface is the one the session was bound to.
    if (d->backend_live && nativeHandle() == d->session_handle) {
        return true;
    }
    if (d->engine == nullptr) {
        return false;
    }

    if (d->state == SurfaceState::Failed) {
        // An explicit init() is the host saying "try again": clear the verdict and let this
        // attempt speak for itself.
        d->failure_reason.clear();
        setState(SurfaceState::Pending);
    }

    // Pick the default backend if the host attached none, then attach when the native surface is
    // usable. If it is not usable yet the attach is deferred and this call reports false: calling
    // again is the host's retry, and an established session re-attaches by itself when Qt recreates
    // the platform window, so we never fall back to creating a separate window.
    useDefaultBackend();

    if (d->engine->backend() == nullptr) {
        // Nothing to attach to at all: no render backend plugin is registered. Waiting will
        // not change that, so say it once instead of staying Pending forever.
        failAttach(u8"no render backend is registered");
        return false;
    }

    // Design B: the engine auto-registers no pipeline and is camera-agnostic.
    // The SceneView owns the primary view (camera + content scene +
    // manipulator); it registers the minimal default viewer - an order-0
    // window pass drawing its content through its camera to the backbuffer -
    // unless an application pass already presents that camera to the window
    // (e.g. a deferred-lighting main pass carrying the view's camera). Apps
    // assembling an explicit pipeline via addPass()/RenderPipelineBuilder keep
    // full control; apps that only add helper / HUD passes (which draw
    // through their own cameras) still get the default window pass.
    d->view->ensureWindowPass();

    if (nativeHandle() != nullptr) {
        initializeBackend();
    }
    // Test hatch: force a platform-window recreation after VINE_RECREATE_SURFACE_MS milliseconds, which is
    // the event the follow path above exists for. See recreateSurface(). Armed by the FIRST init() only:
    // following a recreated window re-enters init(), and re-arming here would recreate the surface forever.
    if (!d->recreate_hatch_armed) {
        if (const char* recreate_ms = std::getenv("VINE_RECREATE_SURFACE_MS"); recreate_ms != nullptr && *recreate_ms != '\0') {
            d->recreate_hatch_armed = true;
            const int delay_ms      = std::atoi(recreate_ms);
            QTimer::singleShot(delay_ms, this, [this] { recreateSurface(); });
        }
    }
    if (d->backend_live) {
        // The host's init() can land before the dock layout has fully settled (app_shell calls it
        // right after embedding the control), and Qt may still recreate the platform window right
        // after. Re-check a few times so the backend ends up bound to and rendering at the live
        // surface size; each check is a no-op when the surface is unchanged (handleUpdate
        // deduplicates via the coalescing flag).
        QTimer::singleShot(150, this, [this] { scheduleUpdate(); });
        QTimer::singleShot(400, this, [this] { scheduleUpdate(); });
        QTimer::singleShot(900, this, [this] { scheduleUpdate(); });
    }
    return d->backend_live;
}

void SurfaceWindow::useDefaultBackend()
{
    if (d->backend_selected) {
        return;
    }
    d->backend_selected = true;

    // Default to the first registered render backend when none was attached
    // by the caller through engine()->setBackend().
    if (d->engine->backend() == nullptr) {
        const auto entries = vine::graphics::RenderBackendRegistry::instance().entries();
        if (!entries.empty()) {
            d->engine->setBackend(
                entries.front().factory->create());
        }
    }

    // A default orbit manipulator (bound to the view's camera and scene) is
    // provided lazily by the SceneView on first input / fit; the view forwards
    // window input events to it, so nothing is wired here.
}

void SurfaceWindow::handleMouse(const vine::window::MouseEvent& event)
{
    // A right press starts a pan drag; a release close to the press (no
    // movement) is a plain right-click and opens the context menu.
    const bool is_right = event.button == vine::window::MouseButton::Right;
    if (is_right) {
        if (event.pressed) {
            d->right_press_active = true;
            d->right_press_x      = event.x;
            d->right_press_y      = event.y;
        } else if (d->right_press_active) {
            d->right_press_active = false;
            const double dx       = event.x - d->right_press_x;
            const double dy       = event.y - d->right_press_y;
            if ((dx * dx + dy * dy) < 36.0) {  // within 6 px: a click
                QTimer::singleShot(0, this, [this] { showContextMenu(); });
            }
        }
    }
    // The event is pushed to the view (whose manipulator drives the camera), and a frame is then
    // rendered so the view follows the interaction live: the Vulkan surface is render-on-demand,
    // so without a refresh here orbit/pan/zoom would update the camera but never repaint. Pure
    // hover moves are skipped (no button held => the manipulator does not change the view);
    // press/release and scroll/key always refresh.
    d->view->pushEvent(event);
    if (event.button != vine::window::MouseButton::None) {
        d->mouse_down = event.pressed;
    }
    if (event.button != vine::window::MouseButton::None || d->mouse_down) {
        renderFrame();
    }
}

void SurfaceWindow::handleScroll(const vine::window::ScrollEvent& event)
{
    d->view->pushEvent(event);
    renderFrame();
}

void SurfaceWindow::handleKey(const vine::window::KeyEvent& event)
{
    d->view->pushEvent(event);
    renderFrame();
}

void SurfaceWindow::handleShown()
{
    // Qt shows the embedded QWindow together with its container, so a surface that is meant to stay
    // hidden until the backend is bound has to be re-hidden here, after that show. Without it, adding
    // the control to a window that is already on screen - which is what a plugin loading its UI into
    // a shown main window does - would put the unpresented native window on screen, which is the
    // transparent hole this lifecycle exists to avoid.
    setVisible(d->surface_shown);
}

void SurfaceWindow::handleDestroyed()
{
    // Qt destroys and recreates the native platform surface (a new HWND / xcb window) on layout changes,
    // screen changes and reparents. The SESSION is not torn down for that: the surface is marked unusable so
    // nothing renders into a dead window (renderFrame also compares the live handle against the bound one),
    // and the backend is handed the NEW handle when it appears -- which is the move the SDK contract
    // describes (RenderBackend::setWindowHandle "re-announce the handle to follow a new surface"), and which
    // keeps the device and every compiled pipeline. Shutting the engine down here was what made every
    // recreation cost a full session rebuild.
    d->surface_ok = false;

    // The "the surface is on screen" state goes away with the window: its replacement comes back with the
    // container, so Qt shows it before anything has been drawn into it, and a shown-but-unpresented native
    // window is the transparent hole the hidden-until-attached rule exists to avoid. Only the flag is cleared
    // here (this runs while the platform window is being destroyed); the Show that follows is re-asserted
    // against it (see showEvent()), and the surface is shown again once the backend is bound to the new window
    // (initializeBackend()).
    d->surface_shown = false;

    // The published state goes back with it: Presenting claims the area shows render output, which is not
    // true of a window that is being replaced, and a host showing its own placeholder wants that transition.
    // The session itself is untouched (see the flags above); the next attach republishes Attached.
    setState(SurfaceState::Pending);
}

void SurfaceWindow::noteSurfaceUsable()
{
    d->surface_ok = true;
    // Everything a resize means is settled in the deferred update: it rebuilds the swapchain at the final
    // native size on an established session, and it is what follows a recreated platform window (a new
    // handle) onto its new surface. A surface that merely BECAME usable does not attach here - the first
    // attach is the host's init() to make.
    scheduleUpdate();
}

void SurfaceWindow::handleResized(int width, int height)
{
    if (width == d->last_width && height == d->last_height) {
        return;
    }
    d->last_width  = width;
    d->last_height = height;
    noteSurfaceUsable();
}

void SurfaceWindow::scheduleUpdate()
{
    if (d->resize_pending) {
        return;
    }
    d->resize_pending = true;
    // Run after the current Qt layout pass, not synchronously inside the
    // resize dispatch: the native child window must be at its final geometry
    // before the backend rebuilds its swapchain (vsg's Win32 window resize()
    // reads the real HWND client rect, so a rebuild mid-layout would keep the
    // old size and the view would never refresh).
    QTimer::singleShot(0, this, [this] { handleUpdate(); });
}

void SurfaceWindow::handleUpdate()
{
    d->resize_pending = false;
    const int w  = width();
    const int h  = height();
    void* handle = nativeHandle();

    // Nothing to update while the surface is not there (Qt destroying/recreating the platform window, or the
    // window not laid out): the surface events that follow bring this back in.
    if (!d->surface_ok || handle == nullptr || w <= 0 || h <= 0) {
        return;
    }

    if (handle != d->session_handle) {
        // The window Qt shows now is not the one the backend is bound to: the platform window was recreated
        // underneath. An ESTABLISHED session follows it by itself -- that is the surface owning its own
        // window, not the surface guessing a timing -- while a session that was never established (the host
        // has not called init(), or its call did not succeed) is left alone: that attach is the host's.
        if (d->has_session) {
            init();
        }
        return;
    }

    // Normal resize of the attached surface: rebuild the swapchain at the
    // final native size, refresh the view's camera projection aspect, present,
    // then request settle frames so the resized view is actually displayed.
    //
    // ONE frame, and no frame before the layout step. That step resizes the
    // creator's off-screen chain (the deferred G-buffer, the composite target and
    // every program slot that samples them), and the rebuild it triggers costs a
    // frame's worth of work -- measured ~250 ms for the deferred demo on a
    // maximize (six fullscreen programs and two off-screen targets). A frame
    // presented BEFORE it is possible (the swapchain already follows the window,
    // and a fullscreen program samples its source through vine_uv, so the
    // previous picture would be scaled to the new size) and fills the window
    // sooner -- but it fills it DISTORTED: the old picture stretched to the new
    // aspect, then snapping back when this frame lands. That was tried on
    // Windows and rejected: the picture is never distorted, and the part of the
    // client area the window just grew by is simply filled when this frame
    // lands. See .ai/memory/graphics.md (2026-09-17).
    d->engine->resize(w, h);
    d->view->onSurfaceResized(w, h);
    renderFrame();
    requestSettleFrames();
}

void SurfaceWindow::requestSettleFrames()
{
    // A single present right after a size change can be dropped by the
    // presentation pipeline while the native surface settles (e.g. Vulkan
    // returns VK_ERROR_OUT_OF_DATE_KHR after a swapchain rebuild and, with
    // render-on-demand, no later frame re-presents), leaving a stale image.
    // This is backend-independent. Re-render on a few display-synced updates
    // (QWindow::requestUpdate() -> QEvent::UpdateRequest); each frame also
    // lets the backend re-sync to the current surface size.
    d->settle_frames = 3;
    requestUpdate();
}

void SurfaceWindow::handleUpdateTick()
{
    if (d->settle_frames > 0) {
        --d->settle_frames;
        renderFrame();
        if (d->settle_frames > 0) {
            requestUpdate();
        }
    }
}

void SurfaceWindow::initializeBackend()
{
    void* h = nativeHandle();
    if (d->backend_live && h != nullptr && h == d->session_handle) {
        // Already bound to this very surface: nothing to do (backend_live implies has_session).
        return;
    }
    if (h == nullptr || width() <= 0 || height() <= 0) {
        // No usable native surface yet (Qt destroying/recreating the platform window, or the
        // window not laid out yet): defer so we never attach to a dead or empty handle. The
        // surface events that follow (a resize, a recreated surface) bring the caller back.
        if (!d->deferral_logged) {
            // Once per session: this line is the answer to "why is my render area empty?".
            // It also says how long the host's own startup kept the surface unusable.
            d->deferral_logged = true;
            vine::logging::defaultLogger().info("[RenderControl] waiting for a usable surface ({}x{}, {} ms after construction)",
                                                width(),
                                                height(),
                                                elapsedMs(d->created_at));
        }
        return;
    }
    if (d->has_session && h != d->session_handle) {
        // The platform window was recreated and this is the new one. Re-announce the handle instead of
        // tearing the session down: setWindowHandle is the contract for "follow me onto this surface", so
        // the backend moves -- the device and every compiled pipeline stay -- and rebuilds the session
        // itself when it cannot serve the new window (a different swapchain format). Shutting the engine
        // down here forced the expensive path on every recreation; the vsg backend's windowBuildCount() is
        // what tells the two apart (flat after a move, +1 after a rebuild).
        vine::logging::defaultLogger().info(
            "[RenderControl] the render surface was recreated: re-announcing the new handle so the backend can follow it");
    }
    // Give the engine the native window the backend must attach to; the
    // handle is refreshed here so a re-announce after a surface recreate uses
    // the new handle (and a backend that can move does exactly that).
    d->engine->setWindowHandle(h);
    d->backend_live = d->engine->initialize();
    if (d->backend_live) {
        d->has_session    = true;
        d->session_handle = h;
        setState(SurfaceState::Attached);
        // The surface may only be shown once something can go into it: showing it before the
        // attach is what left a transparent hole at startup (an unpresented native window
        // shows whatever the compositor decides). Shown here, the first present follows
        // within a frame. The same rule covers a recreated platform window: it stayed hidden
        // while the backend was being bound to it (see handleDestroyed()).
        setSurfaceShown(true);
        // Deliver the current surface size so the view's camera projection
        // aspect is set on the first frame (undistorted) and the backend
        // viewport tracks the surface.
        d->engine->resize(width(), height());
        d->view->onSurfaceResized(width(), height());
        renderFrame();
        // The first attach can land mid-layout (the host calls init() right after embedding the
        // control, before its dock layout has settled), so the
        // native surface may still be resized afterwards. Request settle frames
        // so a few display-synced updates re-sync the swapchain to the final
        // size and the first content is actually presented (a single present
        // against a soon-to-resize surface can otherwise be dropped, leaving
        // the view empty).
        requestSettleFrames();
    }
    else if (!d->surface_shown) {
        // The platform would not build a surface for an unmapped window (Wayland wants the
        // surface configured first; X11 and Windows accept an unmapped one). Show the surface so the
        // next attach can use it: shown-but-unpresented is the state the hidden-surface path exists
        // to avoid, so this is the fallback, not the design - and it is said out loud once, because
        // it also explains a longer startup on that platform. The next attach is the host's init()
        // on a first attach and the surface's own follow of a recreated window on an established
        // one; each of those shows the surface again, because a recreated window comes back hidden.
        if (!d->needs_visible_surface) {
            d->needs_visible_surface = true;
            vine::logging::defaultLogger().info(
                "[RenderControl] attaching to a hidden surface failed: this platform wants a visible window, showing the surface so the next attach can use it");
        }
        setSurfaceShown(true);
    }
    // On failure backend_live stays false, so the call after this one (the host's init(), or the
    // surface's own when it follows an established session onto a recreated window) tries again.
}

void SurfaceWindow::fitToScreen()
{
    if (d->view != nullptr) {
        if (!d->view->fitToScreen()) {
            d->view->home();
        }
    }
    renderFrame();
}

void SurfaceWindow::recreateSurface()
{
    // destroy() + create() is how a platform window is recreated (the surface is nested in the host widget, so
    // it comes back in the same place with a NEW handle); the follow path then does the work.
    vine::logging::defaultLogger().info("[RenderControl] test hatch: recreating the render surface");
    destroy();
    create();
    show();
    scheduleUpdate();
}

void SurfaceWindow::showContextMenu()
{
    if (d->view == nullptr) {
        return;
    }
    QMenu menu(d->host);
    QAction* fit = menu.addAction(QString::fromUtf8("适应屏幕"));
    QObject::connect(fit, &QAction::triggered, [this] { fitToScreen(); });
    menu.exec(QCursor::pos());
}

void SurfaceWindow::renderFrame()
{
    if (d->engine == nullptr) {
        return;
    }
    // Never run the vsg frame loop (acquire/present) against a stale or hidden
    // surface: acquireNextFrame() calls Window::resize() on a dead HWND and
    // spams validation errors. Only render while the backend is attached to
    // the surface the QWindow currently reports and the control is on screen.
    //
    // On screen is asked of the HOST widget, not of the QWindow: Qt shows the
    // embedded window together with its container, so the surface's own flags say
    // "shown" even while the window hosting it is not visible - measured: a surface
    // shown while its top-level window is hidden reports visible=1 and exposed=1 under
    // the offscreen platform. The host widget answers the other direction of the question as
    // well: it becomes visible only once the layout has given the widget tree its geometry,
    // and that is the pass that carries the size this surface keeps - so a frame from here is
    // built once, at the final size, instead of being built for a size the surface is about
    // to leave (the window system hands the real one over through the event loop).
    void* h = nativeHandle();
    if (h == nullptr) {
        return;
    }
    if (!d->surface_shown || d->host == nullptr || !d->host->isVisible()) {
        return;
    }
    if (d->has_session && h != d->session_handle) {
        // Qt recreated the native surface (new HWND) but no surface/resize
        // notice was observed; rebind to the live window so we never keep
        // presenting to a dead handle. initializeBackend() hands the new handle
        // to the backend and renders the first frame on that surface, then
        // returns.
        initializeBackend();
        return;
    }
    if (d->backend_live) {
        d->engine->frame();
        // A frame was handed to a visible, attached surface, so the area shows render output
        // from here on. A backend that could not present reports that on the diagnostics
        // channel rather than through this call, which is why the state is "a frame was
        // submitted", not "the swapchain confirmed it".
        setState(SurfaceState::Presenting);
    }
}

void SurfaceWindow::setState(SurfaceState next)
{
    if (d->state == next) {
        return;
    }

    const SurfaceState previous = d->state;
    d->state                    = next;

    // One line per transition, with the elapsed time: construction -> attached -> first frame is
    // the number that says whether a startup needs the prewarm path (device and pipelines built
    // before the window exists), and the surface flags say whether the platform let the surface
    // attach while hidden.
    if (next == SurfaceState::Failed) {
        vine::logging::defaultLogger().error("[RenderControl] surface {} -> {} after {} ms: {}",
                                             stateName(previous),
                                             stateName(next),
                                             elapsedMs(d->created_at),
                                             d->failure_reason.as_std_str());
    }
    else {
        vine::logging::defaultLogger().info("[RenderControl] surface {} -> {} after {} ms (surface visible={}, exposed={})",
                                            stateName(previous),
                                            stateName(next),
                                            elapsedMs(d->created_at),
                                            isVisible(),
                                            isExposed());
    }

    if (on_state_changed) {
        on_state_changed(next);
    }
}

void SurfaceWindow::setSurfaceShown(bool shown)
{
    if (d->surface_shown == shown) {
        return;
    }
    d->surface_shown = shown;
    setVisible(shown);

    // This is the line that says when the area started showing render output, and it is per direction: the
    // hide side also appears when a platform window is recreated (see handleDestroyed()).
    vine::logging::defaultLogger().info("[RenderControl] surface {} after {} ms",
                                        shown ? "shown" : "hidden",
                                        elapsedMs(d->created_at));
}

void SurfaceWindow::failAttach(String reason)
{
    d->failure_reason = std::move(reason);
    setState(SurfaceState::Failed);
}

bool SurfaceWindow::event(QEvent* event)
{
    // The two event kinds QWindow has no protected handler for.
    switch (event->type()) {
        case QEvent::PlatformSurface: {
            auto* e = static_cast<QPlatformSurfaceEvent*>(event);
            // Qt destroys and recreates the native platform window (a new HWND / xcb window) on
            // layout changes, screen changes and reparents: report both phases, and the session
            // keeps itself and moves onto the new surface on the second one.
            if (e->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                handleDestroyed();
            } else if (e->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
                noteSurfaceUsable();
            }
            break;
        }
        case QEvent::UpdateRequest:
            // What QWindow::requestUpdate() asks for: the display-synced tick the settle frames a
            // resize owes are rendered from.
            handleUpdateTick();
            break;
        default:
            break;
    }
    return QWindow::event(event);
}

void SurfaceWindow::resizeEvent(QResizeEvent* event)
{
    handleResized(event->size().width(), event->size().height());
    QWindow::resizeEvent(event);
}

void SurfaceWindow::showEvent(QShowEvent* event)
{
    handleShown();
    QWindow::showEvent(event);
}

bool SurfaceWindow::eventFilter(QObject* watched, QEvent* event)
{
    // The container widget the surface is embedded through: its resize covers a maximize, and its
    // show is the other way this window reaches the screen (the same thing showEvent() reports).
    if (event->type() == QEvent::Resize) {
        handleResized(static_cast<QResizeEvent*>(event)->size().width(),
                      static_cast<QResizeEvent*>(event)->size().height());
    } else if (event->type() == QEvent::Show) {
        handleShown();
    }
    return QWindow::eventFilter(watched, event);
}

void SurfaceWindow::mousePressEvent(QMouseEvent* event)
{
    handleMouse(toMouseEvent(*event, toMouseButton(event->button()), true));
    QWindow::mousePressEvent(event);
}

void SurfaceWindow::mouseReleaseEvent(QMouseEvent* event)
{
    handleMouse(toMouseEvent(*event, toMouseButton(event->button()), false));
    QWindow::mouseReleaseEvent(event);
}

void SurfaceWindow::mouseMoveEvent(QMouseEvent* event)
{
    handleMouse(toMouseEvent(*event, vine::window::MouseButton::None, false));
    QWindow::mouseMoveEvent(event);
}

void SurfaceWindow::wheelEvent(QWheelEvent* event)
{
    handleScroll(toScrollEvent(*event));
    QWindow::wheelEvent(event);
}

void SurfaceWindow::keyPressEvent(QKeyEvent* event)
{
    handleKey(toKeyEvent(*event, true));
    QWindow::keyPressEvent(event);
}

void SurfaceWindow::keyReleaseEvent(QKeyEvent* event)
{
    handleKey(toKeyEvent(*event, false));
    QWindow::keyReleaseEvent(event);
}

V_APPFWGUI_NS_END
