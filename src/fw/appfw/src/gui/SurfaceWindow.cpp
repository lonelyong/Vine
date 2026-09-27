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

VN_APPFWGUI_NS_BEGIN

namespace
{

/// Shorthand for the lifecycle state this implementation works with.
using State = SurfaceWindow::SurfaceState;

/** @brief Names a surface state for the log. */
const char* stateName(State state)
{
    switch (state) {
        case State::Pending:    return "Pending";
        case State::Presenting: return "Presenting";
        case State::Failed:     return "Failed";
    }
    return "?";
}

/** @brief Milliseconds elapsed since a time point. */
long long elapsedMs(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
}

/** @brief Translates Qt keyboard modifiers. */
vn::window::ModifierKey toModifiers(Qt::KeyboardModifiers m)
{
    using namespace vn::window;
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

/** @brief Translates a Qt mouse button (None for one this framework does not model). */
vn::window::MouseButton toMouseButton(Qt::MouseButton b)
{
    using namespace vn::window;
    switch (b) {
        case Qt::LeftButton:   return MouseButton::Left;
        case Qt::RightButton:  return MouseButton::Right;
        case Qt::MiddleButton: return MouseButton::Middle;
        case Qt::XButton1:     return MouseButton::XButton1;
        case Qt::XButton2:     return MouseButton::XButton2;
        default:               return MouseButton::None;
    }
}

/** @brief Translates a Qt key code (Unknown for one this framework does not model). */
vn::window::KeyCode toKeyCode(int key, bool numpad)
{
    using namespace vn::window;
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

/** @brief Translates a Qt mouse event into the input event the view consumes. */
vn::window::MouseEvent toMouseEvent(const QMouseEvent& event, vn::window::MouseButton button, bool pressed)
{
    vn::window::MouseEvent e;
    e.button    = button;
    e.modifiers = toModifiers(event.modifiers());
    e.x         = event.position().x();
    e.y         = event.position().y();
    e.pressed   = pressed;
    return e;
}

/** @brief Translates a Qt key event into the input event the view consumes. */
vn::window::KeyEvent toKeyEvent(const QKeyEvent& event, bool pressed)
{
    const bool numpad = bool(event.modifiers() & Qt::KeypadModifier);
    vn::window::KeyEvent e;
    e.code      = toKeyCode(event.key(), numpad);
    e.modifiers = toModifiers(event.modifiers());
    e.pressed   = pressed;
    e.repeat    = event.isAutoRepeat();
    return e;
}

/** @brief Translates a Qt wheel event into the input event the view consumes. */
vn::window::ScrollEvent toScrollEvent(const QWheelEvent& event)
{
    const auto delta = event.angleDelta();
    vn::window::ScrollEvent e;
    // Qt angleDelta is in 1/8-degree units; convert to notches/lines.
    e.deltaX    = delta.x() / 120.0;
    e.deltaY    = delta.y() / 120.0;
    e.modifiers = toModifiers(event.modifiers());
    return e;
}

}  // namespace

struct SurfaceWindow::Impl {
    /// The widget the surface is embedded in (the control's own); parent of the context menu.
    QWidget* host = nullptr;
    vn::intrusive_ptr<vn::graphics::RenderEngine> engine;
    /// This surface's own view: one control holds one session (see appfw-document-model.md §9).
    vn::intrusive_ptr<vn::graphics::SceneView> view;
    /// Whether the default backend was already picked (idempotent).
    bool backend_selected = false;
    /// true once a device and its pipelines were built on session_handle: what makes a new handle a re-announce
    /// rather than a first attach, and what keeps the surface following a recreated window.
    bool has_session = false;
    /// true while the last attach attempt reported success (implies has_session).
    bool backend_live = false;
    /// Whether the VINE_RECREATE_SURFACE_MS test hatch has been armed.
    bool recreate_hatch_armed = false;
    /// Lifecycle, published through state() / on_state_changed.
    State state = State::Pending;
    /// Why the state reached Failed.
    String failure_reason;
    /// The platform refused a surface that is not on screen: the control was asked to show it, once.
    bool needs_visible_surface = false;
    /// Whether the "waiting for a usable surface" line was already said.
    bool deferral_logged = false;
    /// Whether a mouse button is held (drives the drag refresh).
    bool mouse_down = false;
    /// Right-press tracking: a release within 6 px is a click (context menu), a drag is a pan.
    bool right_press_active = false;
    double right_press_x = 0.0;
    double right_press_y = 0.0;
    /// Whether the native surface exists and is laid out (cleared while Qt replaces the platform window).
    bool surface_ok = false;
    /// Coalesces resize/surface-created notices into one deferred update.
    bool resize_pending = false;
    /// The handle the session is bound to.
    void* session_handle = nullptr;
    /// Last client size reported, by either the host widget or this window (the other one repeats it).
    int last_width = -1;
    int last_height = -1;
    /// For the lifecycle log lines (construction -> attach -> first frame).
    std::chrono::steady_clock::time_point created_at{};
};

SurfaceWindow::SurfaceWindow(QWidget* host)
  : d(new Impl())
{
    d->host = host;

    // Vulkan surface: Qt does not composite a raster backing store over the render surface.
    setSurfaceType(QSurface::VulkanSurface);
    // The surface does not manage its own visibility: RenderControl shows and hides the window container it is
    // embedded in (Qt owns an embedded window's visibility), which is what keeps an unpresented native window off
    // screen.

    d->created_at = std::chrono::steady_clock::now();
    d->engine     = vn::intrusive_ptr<vn::graphics::RenderEngine>(
        new vn::graphics::RenderEngine());
    // The engine is a pure scheduler (no passes, camera-agnostic); the view owns the camera, the content scene,
    // the orbit manipulator and the window pass that presents them.
    d->view = vn::intrusive_ptr<vn::graphics::SceneView>(
        new vn::graphics::SceneView());
    d->view->setEngine(d->engine.get());

    // Backend diagnostics become log records: a windowed app never shows stderr, and a rejected geometry, a
    // fallback shader or an off-screen target that could not be built would otherwise go unseen.
    d->engine->setDiagnosticSink([](const vn::graphics::RenderDiagnostic& diagnostic) {
        auto&             logger  = vn::logging::defaultLogger();
        const std::string message = diagnostic.message.as_std_str();
        switch (diagnostic.severity) {
            case vn::graphics::DiagnosticSeverity::Error:
                logger.error("[graphics] {}", message);
                break;
            case vn::graphics::DiagnosticSeverity::Warning:
                logger.warn("[graphics] {}", message);
                break;
            case vn::graphics::DiagnosticSeverity::Info:
                logger.info("[graphics] {}", message);
                break;
        }
    });

    // 构造即试一次：句柄在 QWindow 一 new 出来时就有。拿不到可用窗口（句柄空、尺寸 0x0）或后端还没注册时
    // 退回去 —— 后端后注册是 SDK 写的正常次序（构造 ⇒ setBackend() ⇒ init()），那不是失败；窗口显示时的
    // 重试会把它开起来（见 SurfaceWindow::eventFilter 与 RenderControl 装的事件过滤器）。
    if (d->engine->backend() != nullptr) {
        init();
    }
}

SurfaceWindow::~SurfaceWindow()
{
    delete d;
}

vn::graphics::RenderEngine* SurfaceWindow::engine() const
{
    return d->engine.get();
}

vn::graphics::SceneView* SurfaceWindow::view() const
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

vn::async::Task<bool> SurfaceWindow::initAsync()
{
    // The awaitable spelling of init() for a caller that is already a coroutine. Nothing is handed to the pool: the
    // native window is the one thing the backend's own initialize() needs and it exists as soon as the surface does,
    // so this completes without suspending (see the declaration).
    co_return init();
}

void SurfaceWindow::armRecreateHatch()
{
    if (d->recreate_hatch_armed) {
        return;
    }
    if (const char* recreate_ms = std::getenv("VINE_RECREATE_SURFACE_MS"); recreate_ms != nullptr && *recreate_ms != '\0') {
        d->recreate_hatch_armed = true;
        const int delay_ms      = std::atoi(recreate_ms);
        QTimer::singleShot(delay_ms, this, [this] { recreateSurface(); });
    }
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
        const auto entries = vn::graphics::RenderBackendRegistry::instance().entries();
        if (!entries.empty()) {
            d->engine->setBackend(
                entries.front().factory->create());
        }
    }

    // A default orbit manipulator (bound to the view's camera and scene) is
    // provided lazily by the SceneView on first input / fit; the view forwards
    // window input events to it, so nothing is wired here.
}

void SurfaceWindow::handleMouse(const vn::window::MouseEvent& event)
{
    // A right press starts a pan drag; a release within 6 px of it is a click and opens the context menu.
    const bool is_right = event.button == vn::window::MouseButton::Right;
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
    // The view is render-on-demand: without a frame here, orbit/pan/zoom would move the camera and show nothing.
    // A pure hover (no button held) only refreshes while a drag is in flight.
    d->view->pushEvent(event);
    if (event.button != vn::window::MouseButton::None) {
        d->mouse_down = event.pressed;
    }
    if (event.button != vn::window::MouseButton::None || d->mouse_down) {
        renderFrame();
    }
}

void SurfaceWindow::handleScroll(const vn::window::ScrollEvent& event)
{
    d->view->pushEvent(event);
    renderFrame();
}

void SurfaceWindow::handleKey(const vn::window::KeyEvent& event)
{
    d->view->pushEvent(event);
    renderFrame();
}

void SurfaceWindow::handleDestroyed()
{
    // Qt replaces the native platform window (layout change, screen change, reparent) - the session is NOT torn
    // down with it: the new handle is re-announced to the backend (RenderBackend::setWindowHandle), so the device
    // and every compiled pipeline stay. Marking the surface unusable keeps frames off the dead window.
    d->surface_ok = false;

    // Pending claims nothing about the screen, which is what makes the control hide the area again: a visible
    // window container without a frame in it is a hole. The next attach republishes Presenting.
    setState(SurfaceState::Pending);
}

void SurfaceWindow::noteSurfaceUsable()
{
    d->surface_ok = true;
    // A surface that merely BECAME usable does not attach here: the deferred update is where both the resize and
    // the follow of a recreated window are settled (see handleUpdate()).
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
    // After the current layout pass, not inside the resize dispatch: a backend that rebuilds its swapchain from
    // the window's real client rect would keep the old size mid-layout (see handleUpdate()).
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
        // The window Qt shows now is not the one the backend is bound to: a recreated platform window (which an
        // established session follows), or a first attach that had no usable window yet. Both attach here.
        init();
        return;
    }

    // The resize: swapchain at the final size, view aspect, ONE frame at that size - and no frame before the
    // layout step. A present before it would be the OLD picture stretched to the new aspect (a fullscreen program
    // samples its source through vine_uv), and the frame that follows snaps it back; that was tried on Windows and
    // rejected in favour of never showing a distorted picture (the grown part of the client area is simply filled
    // by this frame). See .ai/memory/graphics.md (2026-09-17).
    d->engine->resize(w, h);
    d->view->onSurfaceResized(w, h);
    renderFrame();
}

bool SurfaceWindow::init()
{
    // The one attach, and the entry point: idempotent while the session is bound to the surface the window reports
    // NOW - after Qt recreated the platform window this has to re-attach (a re-announce, so the backend can move)
    // rather than report the old attach as current.
    if (d->backend_live && nativeHandle() == d->session_handle) {
        return true;  // Already bound to this very surface (backend_live implies has_session).
    }

    if (d->state == SurfaceState::Failed) {
        // An explicit attach is the caller saying "try again": clear the verdict and let this attempt speak for
        // itself.
        d->failure_reason.clear();
        setState(SurfaceState::Pending);
    }

    // No backend yet? Take the first registered one; a host that wants its own sets it before this.
    useDefaultBackend();

    if (d->engine->backend() == nullptr) {
        // Nothing to attach to at all: no render backend plugin is registered. Waiting will not change that, so
        // say it once instead of staying Pending forever.
        failAttach(u8"no render backend is registered");
        return false;
    }

    // The view registers the default window pass (an order-0 pass drawing its content through its camera to the
    // backbuffer) unless the app already presents that camera with a pass of its own.
    d->view->ensureWindowPass();
    // Test hatch: force a platform-window recreation after VINE_RECREATE_SURFACE_MS, once per session (following a
    // recreated window re-enters here, and re-arming would recreate it forever). See recreateSurface().
    armRecreateHatch();

    void* const handle = nativeHandle();
    if (handle == nullptr || width() <= 0 || height() <= 0) {
        // Defer: attaching to a dead or empty handle is worse than waiting. The surface events that follow bring
        // the caller back (a first attach is retried by handleUpdate(), an established session follows the window).
        if (!d->deferral_logged) {
            // Once per session: this line is the answer to "why is my render area empty?". It also says how long
            // the caller's own startup kept the surface unusable.
            d->deferral_logged = true;
            vn::logging::defaultLogger().info(
                "[RenderControl] waiting for a usable surface ({}x{}, {} ms after construction)",
                width(),
                height(),
                elapsedMs(d->created_at));
        }
        return d->backend_live;
    }
    if (d->has_session && handle != d->session_handle) {
        // The platform window was recreated: re-announce the new handle (RenderBackend::setWindowHandle) instead
        // of tearing the session down, so the device and every compiled pipeline stay.
        vn::logging::defaultLogger().info(
            "[RenderControl] the render surface was recreated: re-announcing the new handle so the backend can follow it");
    }
    // Give the engine the native window the backend must attach to; the handle is refreshed here so a re-announce
    // after a surface recreate uses the new handle (and a backend that can move does exactly that).
    d->engine->setWindowHandle(handle);

    // The attachment: the device, the session and every pipeline are built behind this call, on this thread. It
    // returns before its caller does, so nothing can be "in the middle of an attach" - a host installs its content
    // afterwards (see the declaration).
    d->backend_live = d->engine->initialize();

    if (!d->backend_live) {
        // This platform will not attach to a surface that is not on screen (Wayland): ask the control to show the
        // area so the next attach can use it. Said out loud once - it also explains a longer startup there.
        if (!d->needs_visible_surface) {
            d->needs_visible_surface = true;
            vn::logging::defaultLogger().info(
                "[RenderControl] attaching to a surface that is not on screen failed: this platform wants a visible window, showing the surface so the next attach can use it");
            if (on_needs_visible_surface) {
                on_needs_visible_surface();
            }
        }
        return false;
    }

    // The attach's own timing, on its own line: this is the device + session + pipeline build, the half a startup
    // cares about (the frame below is the other half, and the Presenting transition logs it).
    vn::logging::defaultLogger().info("[RenderControl] session up after {} ms (device, session and pipelines)",
                                        elapsedMs(d->created_at));
    d->has_session    = true;
    d->session_handle = handle;
    // The size the surface has NOW goes to the backend and the view, so the frame below is built at it (undistorted
    // aspect, viewport tracking). At construction that is a placeholder size (measured 160x160 against the 752x480
    // of the settled window); the layout's real size arrives through handleUpdate() and is served in place.
    d->engine->resize(width(), height());
    d->view->onSurfaceResized(width(), height());
    // The frame the attach owes - the same frame as any other, and it is what pays the session's one-time build
    // (pass graphs, program slots, compiled pipelines - measured 183.6 ms for the demo's 5 slots).
    renderFrame();
    return true;
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
    // destroy() + create() is how a platform window is recreated; the follow path does the rest.
    vn::logging::defaultLogger().info("[RenderControl] test hatch: recreating the render surface");
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
    // A recreated platform window is the case this is for: follow it now rather than present to a window the
    // session is not bound to (acquireNextFrame() against the dead handle spams validation errors).
    void* const h = nativeHandle();
    if (h == nullptr) {
        return;
    }
    if (d->has_session && h != d->session_handle) {
        init();
        return;
    }
    d->engine->frame();
    // A frame is in the surface: this is the transition the control shows its render area on. A backend that could
    // not present reports it on the diagnostics channel, which is why this says "submitted", not "confirmed".
    setState(SurfaceState::Presenting);
}

void SurfaceWindow::setState(SurfaceState next)
{
    if (d->state == next) {
        return;
    }

    const SurfaceState previous = d->state;
    d->state                    = next;

    // One line per transition, with the elapsed time: "construction -> the attach's frame" is the number that says
    // whether a startup pays the device and pipeline build before its window exists.
    if (next == SurfaceState::Failed) {
        vn::logging::defaultLogger().error("[RenderControl] surface {} -> {} after {} ms: {}",
                                             stateName(previous),
                                             stateName(next),
                                             elapsedMs(d->created_at),
                                             d->failure_reason.as_std_str());
    }
    else {
        vn::logging::defaultLogger().info("[RenderControl] surface {} -> {} after {} ms (surface visible={}, exposed={})",
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

void SurfaceWindow::failAttach(String reason)
{
    d->failure_reason = std::move(reason);
    setState(SurfaceState::Failed);
}

bool SurfaceWindow::event(QEvent* event)
{
    // The one event kind QWindow has no protected handler for: the phases of the native platform surface.
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

bool SurfaceWindow::eventFilter(QObject* watched, QEvent* event)
{
    // The widget(s) whose events are forwarded here (RenderControl installs this filter on both the window
    // container and its own widget, since the container is hidden until a frame is in the surface): a resize is
    // what carries the surface's final size, a show is the "we have reached the screen" that a surface which has
    // not attached yet is waiting for.
    if (event->type() == QEvent::Resize) {
        handleResized(static_cast<QResizeEvent*>(event)->size().width(),
                      static_cast<QResizeEvent*>(event)->size().height());
    } else if (event->type() == QEvent::Show) {
        scheduleUpdate();
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
    handleMouse(toMouseEvent(*event, vn::window::MouseButton::None, false));
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

VN_APPFWGUI_NS_END
