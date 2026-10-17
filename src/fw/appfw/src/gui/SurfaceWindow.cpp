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
#include <QScreen>
#include <QSurface>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <vine/appfw/gui/Viewer.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderSurface.hpp>
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

/// One entry (and one bit) per mouse button the window reports: a drag belongs to the button that started it, so
/// the bookkeeping is per button rather than one "a button is held" flag (see SurfaceWindow::handleMouse).
constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(vn::window::MouseButton::XButton2) + 1u;

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
    // Where the wheel turned: logical surface pixels, the same space the mouse events carry. A surface split
    // between several views routes the zoom by it (a view cannot zoom "the whole surface").
    e.x         = event.position().x();
    e.y         = event.position().y();
    return e;
}

/** @brief Says why a device handed out no surface target, in words a host can act on. */
String targetRefusalReason(vn::graphics::SurfaceTargetResult why)
{
    switch (why) {
        case vn::graphics::SurfaceTargetResult::NoPresentation:
            return u8"the render backend presents nothing, so this window has no target to draw through";
        case vn::graphics::SurfaceTargetResult::UnsupportedSurface:
            return u8"the render backend cannot serve this window (it is not a surface it presents through)";
        case vn::graphics::SurfaceTargetResult::WindowOccupied:
            return u8"this window already has a live surface target (one window carries one swapchain)";
        case vn::graphics::SurfaceTargetResult::Ok:
            break;
    }
    return u8"the render backend handed out no surface target";
}

}  // namespace

struct SurfaceWindow::Impl {
    /// The widget the surface is embedded in (the control's own); parent of the context menu.
    QWidget* host = nullptr;
    vn::intrusive_ptr<vn::graphics::RenderEngine> engine;
    /// This surface's own view: one control holds one session (see appfw-document-model.md §9).
    vn::intrusive_ptr<vn::graphics::SceneView> view;
    /// Which passes draw into this surface, and in which order (see RenderSurface). Declared after the engine so it
    /// is DESTROYED before it: the surface borrows that engine.
    vn::intrusive_ptr<vn::graphics::RenderSurface> surface;
    /// The request this area's picture is built from (see Viewer). Declared after the engine, the view and
    /// the surface so it is destroyed before them: it borrows all three.
    std::unique_ptr<Viewer> viewer;
    /// true once a device and its pipelines were built on the surface's window: what makes a new handle a
    /// re-announce rather than a first attach, and what keeps the surface following a recreated window.
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
    /// Right-press tracking: a release within 6 px is a click (context menu), a drag is a pan.
    bool right_press_active = false;
    double right_press_x = 0.0;
    double right_press_y = 0.0;
    /// Whether the native surface exists and is laid out (cleared while Qt replaces the platform window).
    bool surface_ok = false;
    /// Coalesces resize/surface-created notices into one deferred update.
    bool resize_pending = false;
    /// Last client size reported, by either the host widget or this window (the other one repeats it).
    int last_width = -1;
    int last_height = -1;
    /// Device pixel ratio that went with them: a scale-factor change is a surface change too.
    double last_ratio = 0.0;
    /// Which view owns which point of the surface: ONE area, one picture (see routedView).
    /// The view each HELD button's drag belongs to, by MouseButton index - one entry per button, so a second
    /// button pressed elsewhere starts its own drag instead of taking the first one's, and releasing it cannot end
    /// the other drag (see routedView).
    vn::intrusive_ptr<vn::graphics::SceneView> captured_views[kMouseButtonCount];
    /// Buttons currently held on this surface, one bit per button (see bitFor): "a button is down" is what makes a
    /// pointer move repaint, and what a key event belongs to (a modifier pressed during a drag is that drag's).
    unsigned held_buttons = 0;
    /// The button whose press started the most recent drag: where a MOVE goes while it is still held (an event
    /// that carries no button of its own).
    vn::window::MouseButton last_press = vn::window::MouseButton::None;
    /// The timer that drives frames when continuous rendering is on (created on first use, owned by this window).
    QTimer* continuous = nullptr;
    /// For the lifecycle log lines (construction -> attach -> first frame).
    std::chrono::steady_clock::time_point created_at{};

    /// The bit a button owns in held_buttons: None carries no button, so it owns none.
    static unsigned bitFor(vn::window::MouseButton button)
    {
        const auto index = static_cast<unsigned>(button);
        return index == 0u ? 0u : 1u << (index - 1u);
    }

    /// The view one button's drag was captured from: null when that button is not dragging a view (or nothing
    /// claimed the point under its press).
    vn::graphics::SceneView* captureFor(vn::window::MouseButton button) const
    {
        const auto index = static_cast<std::size_t>(button);
        if (index == 0u || index >= kMouseButtonCount || (held_buttons & bitFor(button)) == 0u) {
            return nullptr;
        }
        return captured_views[index].get();
    }

    /// The view the drag in flight belongs to: where an event that carries no button of its own - a pointer move,
    /// a key - goes. The most recent press' capture when that button is still held, else whichever drag is still
    /// in flight, else nothing (the caller then routes by position).
    vn::graphics::SceneView* dragTarget() const
    {
        if (vn::graphics::SceneView* const recent = captureFor(last_press); recent != nullptr) {
            return recent;
        }
        for (std::size_t index = 1u; index < kMouseButtonCount; ++index) {
            const auto button = static_cast<vn::window::MouseButton>(index);
            if (vn::graphics::SceneView* const captured = captureFor(button); captured != nullptr) {
                return captured;
            }
        }
        return nullptr;
    }
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
    // The engine is a pure scheduler (no passes, camera-agnostic); the view owns the camera, the content scene
    // and the orbit manipulator, and the pipeline it has adopted owns the passes that present them.
    d->view = vn::intrusive_ptr<vn::graphics::SceneView>(
        new vn::graphics::SceneView());
    d->view->setEngine(d->engine.get());
    // The one thing that knows which passes draw into THIS surface: the control falls back to its own view (see
    // init()), and a host that brought a main pass of its own declares it here (RenderSurface::present).
    d->surface = vn::intrusive_ptr<vn::graphics::RenderSurface>(
        new vn::graphics::RenderSurface(*d->engine));
    // The area's picture (see Viewer): the control's own view, registered with the default request but neither a
    // chain nor a declaration - the first attach is where that becomes a picture, so a host that never touches the
    // viewer pays no build until there is an area to draw into.
    d->viewer = std::make_unique<Viewer>(d->engine.get(), d->surface.get(), d->view);

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

    // 构造只组装：这里**不选后端、不 attach**。第一次 attach 排在事件循环第一拍（兜底），所以宿主"new 之后
    // 立刻配置"的同步代码（engine()->setDevice()、view()->setOptions()、装内容）天生排在它前面 —— 这正是
    // SDK 写的次序（构造 ⇒ setDevice() ⇒ init()），也是"配置窗口"永远打开的原因。
    //
    // 兜底不是竞争者：宿主显式调 init()/initAsync()（同步、幂等）就是立刻 attach；窗口事件（show / resize /
    // 句柄重建）也照旧接力。谁先到谁算。
    QTimer::singleShot(0, this, [this] { init(); });

    // A window moving to another screen can change the device pixel ratio without changing the logical size: the
    // deferred update is where both are settled (see handleResized / handleUpdate).
    QObject::connect(this, &QWindow::screenChanged, this, [this](QScreen*) { noteSurfaceUsable(); });
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

vn::graphics::RenderSurface* SurfaceWindow::surface() const
{
    return d->surface.get();
}

Viewer* SurfaceWindow::viewer() const
{
    return d->viewer.get();
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
    // The default is a FALLBACK, not a decision: it is taken whenever the engine has no backend of its own, so
    // (a) a host that sets its own through engine()->setDevice() is never overridden, and (b) a backend registered
    // AFTER an attempt (a plugin loaded late, a host that installs one later) is still picked up by the next
    // attach instead of being locked out by a decision made too early.
    if (d->engine->device() != nullptr) {
        return;
    }

    const auto entries = vn::graphics::RenderBackendRegistry::instance().entries();
    if (!entries.empty()) {
        d->engine->setDevice(
            entries.front().factory->create());
    }

    // A default orbit manipulator (bound to the view's camera and scene) is
    // provided lazily by the SceneView on first input / fit; the view forwards
    // window input events to it, so nothing is wired here.
}

void SurfaceWindow::setContinuousRendering(bool on, int interval_ms)
{
    if (!on) {
        if (d->continuous != nullptr) {
            d->continuous->stop();
        }
        return;
    }

    // A tick period of zero/negative would be "as often as the event loop lets you", which is a busy loop, not a
    // frame rate: the caller states a period and gets at least a millisecond of it.
    if (interval_ms < 1) {
        interval_ms = 1;
    }
    if (d->continuous == nullptr) {
        d->continuous = new QTimer(this);
        QObject::connect(d->continuous, &QTimer::timeout, this, [this] { tickContinuous(); });
    }
    d->continuous->setInterval(interval_ms);
    if (!d->continuous->isActive()) {
        d->continuous->start();
    }
}

void SurfaceWindow::tickContinuous()
{
    // Two "nothing to do" cases, both of which make the tick free instead of an invisible cost:
    //  - no session yet (or one being recreated): there is no device to draw with, and the attach's own frame is
    //    what brings the first picture, not a timer;
    //  - the native surface is not exposed: a hidden dock tab or a minimized window would otherwise keep the GPU
    //    busy drawing frames nobody can see. A host that wants them anyway (a background render target) drives
    //    renderFrame() itself.
    if (!d->backend_live || !isExposed()) {
        return;
    }
    renderFrame();
}

vn::graphics::SceneView* SurfaceWindow::routedView(int, int)
{
    // ONE AREA, ONE PICTURE: this surface draws through the one view it hosts, so every event belongs to it and
    // there is nothing to route. (A host that wants several pictures at once brings its own passes and their own
    // sub-viewports - the engine never manages layout.)
    return d->view.get();
}


void SurfaceWindow::pushToView(vn::graphics::SceneView* target, const vn::window::MouseEvent& event)
{
    if (target != nullptr) {
        target->pushEvent(event);
    }
}

void SurfaceWindow::pushToView(vn::graphics::SceneView* target, const vn::window::ScrollEvent& event)
{
    if (target != nullptr) {
        target->pushEvent(event);
    }
}

void SurfaceWindow::pushToView(vn::graphics::SceneView* target, const vn::window::KeyEvent& event)
{
    if (target != nullptr) {
        target->pushEvent(event);
    }
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

    const bool is_press   = event.pressed && event.button != vn::window::MouseButton::None;
    const bool is_release = !event.pressed && event.button != vn::window::MouseButton::None;

    // A PRESS captures the view, and holds it until every button is up: a drag that leaves the surface keeps
    // steering it, and the held reference keeps that view alive while it is being steered even if the host lets go
    // of its own. Per button, so one button coming up does not end another button's drag (a view that never saw its
    // release would keep steering its camera on pure hover afterwards) - both steer the same view, which is what
    // "one area, one picture" means for input.
    if (is_press) {
        const auto index = static_cast<std::size_t>(event.button);
        d->held_buttons |= Impl::bitFor(event.button);
        d->last_press = event.button;
        d->captured_views[index] = vn::intrusive_ptr<vn::graphics::SceneView>(d->view.get(), true);
    }

    // Where this event goes: the view a held button captured (so a release lands on the view that STARTED that
    // drag, wherever the pointer has wandered), else the surface's own view.
    vn::graphics::SceneView* target = event.button != vn::window::MouseButton::None ? d->captureFor(event.button) : nullptr;
    if (target == nullptr) {
        target = d->dragTarget();
    }
    if (target == nullptr) {
        target = routedView(event.x, event.y);
    }

    // The view is render-on-demand: without a frame here, orbit/pan/zoom would move the camera and show nothing.
    // A pure hover (no button held) only refreshes while a drag is in flight.
    pushToView(target, event);
    if (is_release) {
        d->captured_views[static_cast<std::size_t>(event.button)] = nullptr;  // this drag is over
        d->held_buttons &= ~Impl::bitFor(event.button);
        if (d->held_buttons == 0u) {
            d->last_press = vn::window::MouseButton::None;
        }
    }
    if (event.button != vn::window::MouseButton::None || d->held_buttons != 0u) {
        renderFrame();
    }
}

void SurfaceWindow::handleScroll(const vn::window::ScrollEvent& event)
{
    // A wheel notch is instantaneous - there is no drag to hold - so it goes to the area's view.
    pushToView(routedView(static_cast<int>(event.x), static_cast<int>(event.y)), event);
    renderFrame();
}
void SurfaceWindow::handleKey(const vn::window::KeyEvent& event)
{
    // A key has no position: it goes to the view the pointer captured (a keyboard modifier during a drag belongs
    // to that drag), else to the area's view.
    vn::graphics::SceneView* target = d->dragTarget();
    pushToView(target != nullptr ? target : d->view.get(), event);
    renderFrame();
}

void SurfaceWindow::handleDestroyed()
{
    // Qt replaces the native platform window (layout change, screen change, reparent) - the session is NOT torn
    // down with it: the new handle is re-announced (WindowPresentTarget::setWindowHandle), so the device
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
    // The device pixel ratio is part of "the surface changed": a window dragged to a screen with another scale
    // factor has to re-announce its geometry even when the logical size is identical.
    const double ratio = devicePixelRatio();
    if (width == d->last_width && height == d->last_height && ratio == d->last_ratio) {
        return;
    }
    d->last_width  = width;
    d->last_height = height;
    d->last_ratio  = ratio;
    noteSurfaceUsable();
}

void SurfaceWindow::feedSurfaceSize(int width, int height)
{
    const double ratio = devicePixelRatio();
    // Size AND ratio, stated once on the SURFACE: the size of the thing the picture ends up in is a fact about
    // it, so it is recorded there and fanned out to the view that presents it (see RenderSurface::resize).
    d->surface->resize(width, height, ratio);
    // This surface's own view is fed separately ONLY when it does not present (a host declared a main pass of its
    // own, so nothing fans out to it): its geometry would otherwise stay at whatever it last saw.
    if (d->view != nullptr && !d->view->hasSurface()) {
        d->view->resize(width, height, ratio);
    }
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

    if (handle != nativeTargetHandle()) {
        // The window Qt shows now is not the one this area is bound to: a recreated platform window (which an
        // established session follows), or a first attach that had no usable window yet. Both attach here.
        init();
        return;
    }

    // The resize: swapchain at the final size, view aspect, ONE frame at that size - and no frame before the
    // layout step. A present before it would be the OLD picture stretched to the new aspect (a fullscreen program
    // samples its source through vine_uv), and the frame that follows snaps it back; that was tried on Windows and
    // rejected in favour of never showing a distorted picture (the grown part of the client area is simply filled
    // by this frame). See .ai/memory/graphics.md (2026-09-17).
    feedSurfaceSize(w, h);
    renderFrame();
}

bool SurfaceWindow::init()
{
    // The one attach, and the entry point: idempotent while the session is bound to the surface the window reports
    // NOW - after Qt recreated the platform window this has to re-attach (a re-announce, so the backend can move)
    // rather than report the old attach as current.
    if (d->backend_live && nativeHandle() == nativeTargetHandle()) {
        return true;  // Already bound to this very surface (backend_live implies has_session).
    }

    if (d->state == SurfaceState::Failed) {
        // An explicit attach is the caller saying "try again": clear the verdict and let this attempt speak for
        // itself.
        d->failure_reason.clear();
        setState(SurfaceState::Pending);
    }

    // The area's picture: this control's viewer declares it unless a pass the host declared presents it instead
    // (see Viewer::takeArea). That happens BEFORE the backend check on purpose: which passes
    // draw a view is decided by the pipeline it adopted, not by a device - a host with no backend registered still
    // has a view with a pipeline it can inspect (and the pass is what the backend will draw when it does come up).
    d->viewer->takeArea();

    // No backend yet? Take the first registered one; a host that wants its own sets it before this.
    useDefaultBackend();

    if (d->engine->device() == nullptr) {
        // Nothing to attach to at all: no render backend plugin is registered. Waiting will not change that, so
        // say it once instead of staying Pending forever.
        failAttach(u8"no render backend is registered");
        return false;
    }

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
    if (d->has_session && handle != nativeTargetHandle()) {
        // The platform window was recreated: re-announce the new handle on the TARGET (WindowPresentTarget::
        // setWindowHandle) instead of tearing the session down, so the device and every compiled pipeline stay.
        vn::logging::defaultLogger().info(
            "[RenderControl] the render surface was recreated: re-announcing the new handle so the backend can follow it");
    }
    // The window this area is presented through belongs to the AREA, and the DEVICE is what makes the target that
    // names it (RenderDevice::createSurfaceTarget): the device is the party that knows whether it presents at all
    // and whether this window is already taken, and it answers why when it hands none out. A FIRST attach asks for
    // one (before the session comes up, which is what a Vulkan device needs); a window Qt recreated is MOVED onto
    // the target the area already holds - the device, its caches and every compiled pipeline stay behind that.
    if (d->surface->target() == nullptr) {
        vn::graphics::SurfaceTargetResult why = vn::graphics::SurfaceTargetResult::Ok;
        if (vn::raw_ptr<vn::graphics::WindowPresentTarget> target =
                d->engine->device()->createSurfaceTarget(handle, &why);
            target != nullptr) {
            d->surface->bindTarget(target);
        }
        else {
            failAttach(targetRefusalReason(why));
            return false;
        }
    }
    else {
        d->surface->target()->setWindowHandle(handle);
    }

    // The size the surface has NOW goes to the SURFACE first: the surface owns it (PresentTarget::resize's authority
    // order), and it is what the device creates its window at. Announcing it after the device came up would buy a
    // swapchain rebuild - a counted device stop - for a size the window could have been born at.
    feedSurfaceSize(width(), height());

    // THE DEVICE BELONGS TO ITS CREATOR (RenderDevice::initialize): the area's target (a creation input) and the
    // announced size are in place, so the device is brought up HERE - and again on every attach, because that is
    // also what FOLLOWS a re-announced window (the engine, which only borrows the device, never drives this).
    // A device that does not come up is a RETRYABLE refusal, not a verdict: the reason is on the diagnostics channel
    // and the surface stays Pending, which is the same answer this attach gave when the engine drove the session.
    d->backend_live = d->engine->device()->initialize() && d->engine->initialize();

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
    d->has_session = true;
    // failureReason() is the answer only while the state is Failed: a session that comes up after a failed attempt
    // clears the old verdict, so a host that reports the reason cannot keep showing one for a running session.
    d->failure_reason.clear();
    // The size the surface has NOW goes to the frame and the view, so the frame below is built at it (undistorted
    // aspect, viewport tracking). At construction that is a placeholder size (measured 160x160 against the 752x480
    // of the settled window); the layout's real size arrives through handleUpdate() and is served in place.
    feedSurfaceSize(width(), height());
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
    if (d->has_session && h != nativeTargetHandle()) {
        init();
        return;
    }
    if (!d->backend_live) {
        // No session (never attached, or the last attempt failed): there is no device to draw with, so this frame
        // cannot put a picture in the surface - and "a frame is in the surface" is exactly what Presenting means,
        // and what makes the control show the area. Showing it without one would leave the hole the control's
        // hasPresented() says it never leaves on screen. The attach's own frame is the first picture; a host that
        // renders before attaching gets its frames from the attach. Same rule as setContinuousRendering()'s.
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

void* SurfaceWindow::nativeTargetHandle() const
{
    const vn::raw_ptr<vn::graphics::WindowPresentTarget> target = d->surface->target();
    return target != nullptr ? target->nativeHandle() : nullptr;
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
