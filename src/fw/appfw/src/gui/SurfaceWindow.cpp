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
#include <atomic>
#include <cstdlib>
#include <string>
#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/async/ThreadPoolScheduler.hpp>

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

/**
 * @brief Translates a Qt mouse button.
 *
 * @param b Qt button to translate.
 * @return The matching vine button, or None for a button the framework does not model.
 */
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

/**
 * @brief Translates a Qt key code.
 *
 * @param key Qt key code.
 * @param numpad Whether the key came from the numeric keypad, which decides between the digit row
 *               and the keypad codes.
 * @return The matching vine key code, or Unknown for a key the framework does not model.
 */
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

/**
 * @brief Translates a Qt mouse event into the window input event the view consumes.
 *
 * @param event Qt event to translate.
 * @param button Already-translated button the event is about.
 * @param pressed true for a press, false for a release.
 * @return The translated event.
 */
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

/**
 * @brief Translates a Qt key event into the window input event the view consumes.
 *
 * @param event Qt event to translate.
 * @param pressed true for a press, false for a release.
 * @return The translated event.
 */
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

/**
 * @brief Translates a Qt wheel event into the window scroll event the view consumes.
 *
 * @param event Qt event to translate.
 * @return The translated event.
 */
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

/// Keeps one attach marked as in flight, however it ends (the async path can also be abandoned).
class AttachScope
{
  public:
    /// @param flag Flag to hold for the object's lifetime.
    explicit AttachScope(std::atomic<bool>& flag)
      : flag_(flag)
    {
        flag_.store(true, std::memory_order_release);
    }

    ~AttachScope() { flag_.store(false, std::memory_order_release); }

    AttachScope(const AttachScope&)            = delete;
    AttachScope& operator=(const AttachScope&) = delete;

  private:
    std::atomic<bool>& flag_;
};

struct SurfaceWindow::Impl {
    /// The widget the surface is embedded in: asked whether the control is on screen, which the
    /// surface's own flags cannot answer, and used as the context menu's parent.
    QWidget* host = nullptr;
    vn::intrusive_ptr<vn::graphics::RenderEngine> engine;
    /// The interactive primary view bound to the engine (this surface's own: one control holds one session, and a
    /// document that renders brings its own control - see appfw-document-model.md §9).
    vn::intrusive_ptr<vn::graphics::SceneView> view;
    /// Whether the default backend was already picked (the first init() does it; idempotent).
    bool backend_selected = false;
    // Two flags, two different questions. backend_live: did the last attach attempt succeed (is the
    // backend up?). has_session: is there a session at all (a device and its pipelines built, on
    // session_handle) - which is what keeps the surface following a recreated window after a failed
    // re-announce, and what makes a new handle a re-announce rather than a first attach. backend_live
    // true always implies has_session true (both are set together in attach()'s success
    // branch); the reverse does not hold.
    bool has_session = false;
    bool backend_live = false;
    // Whether the VINE_RECREATE_SURFACE_MS test hatch has been armed (the first init() arms it).
    bool recreate_hatch_armed = false;
    // Whether an attach is running right now (including its warm-up frame). Read by hosts that mutate the scene
    // graph: the attach builds pipelines from the registered passes and records a frame, so scene work waits on it
    // (see SurfaceWindow::initAsync()). Atomic because the attach runs across threads and the readers are hosts.
    std::atomic<bool> attaching{ false };
    // Surface lifecycle, published through state()/on_state_changed.
    State state = State::Pending;
    // Why the state reached Failed (empty otherwise).
    String failure_reason;
    // Set when the platform refused to attach to a surface that is not on screen (a window system that wants the
    // surface configured first): the fallback asks the control to put it on screen, and the log line about it is
    // said once. This is the fallback the whole hidden-surface path exists to avoid.
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
    // The surface does NOT manage its own visibility: it is embedded in a window container, and Qt's container
    // owns the embedded window's visibility (it shows and hides it with itself - calling show()/hide() on an
    // embedded window is documented as not recommended). RenderControl hides and shows the container, which is
    // what keeps an unpresented native window (a hole the compositor fills with whatever it likes) off screen.

    d->created_at = std::chrono::steady_clock::now();
    d->engine     = vn::intrusive_ptr<vn::graphics::RenderEngine>(
        new vn::graphics::RenderEngine());
    // Design B: RenderEngine starts empty (no passes) and is a pure
    // scheduler - it holds no camera and no content scene. The interactive
    // primary view - its camera, content scene and orbit manipulator - lives
    // in a SceneView that borrows the engine and binds its content to the
    // window pass it registers (SceneView::ensureWindowPass, called in
    // init()).
    d->view = vn::intrusive_ptr<vn::graphics::SceneView>(
        new vn::graphics::SceneView());
    d->view->setEngine(d->engine.get());

    // Backend diagnostics become log records. This is what makes a failing draw
    // visible in a GUI app at all: the backend reports what it could not serve
    // (a rejected geometry, a shader that fell back to the built-in one, an
    // off-screen target it could not build) through the engine, and without a
    // listener that information only ever reaches stderr, which a windowed app
    // never shows. The engine stores the sink, so it also applies to the backend
    // created later by initialize().
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

    // Nothing attaches here: the host calls RenderControl::init() when it wants the surface up - it
    // has just put the control into a window and may still be configuring the engine or building the
    // pipeline. Until then the surface stays hidden; after the attach the surface only follows its
    // own window (see handleDestroyed()/handleUpdate()).
    useDefaultBackend();

    // 构造即试一次：QWindow 一 new 出来句柄就有，而"建后端"这件事本来就不需要宿主挑时机。拿不到可用窗口
    // （句柄还没建、或尺寸还是 0x0）时它会自己退回去，由后面的 resize/show 再试（见 handleUpdate()）——
    // 也就是"构造时建、显示时自动出帧"这条路，宿主不再必须调 init()（但它仍然可以，幂等）。
    // 后端插件必须已经注册：插件按依赖关系声明（`VN_DECLARE_PLUGIN(..., { u8"gfx_backend_vsg" })`），
    // 于是"在 load() 里建控件"也安全。
    init();
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

    // The synchronous form of initAsync(): the backend's initialize() runs inline, so nothing is handed to the
    // pool and result() never waits on the event loop it is called from (which is what lets a host attach before
    // it has one). Everything else - the handle, the state, the size, the frame - is attach()'s.
    return attach(/*initialize_on_pool=*/false).result();
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
    // A right press starts a pan drag; a release close to the press (no
    // movement) is a plain right-click and opens the context menu.
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
    // The event is pushed to the view (whose manipulator drives the camera), and a frame is then
    // rendered so the view follows the interaction live: the Vulkan surface is render-on-demand,
    // so without a refresh here orbit/pan/zoom would update the camera but never repaint. Pure
    // hover moves are skipped (no button held => the manipulator does not change the view);
    // press/release and scroll/key always refresh.
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
    // Qt destroys and recreates the native platform surface (a new HWND / xcb window) on layout changes,
    // screen changes and reparents. The SESSION is not torn down for that: the surface is marked unusable so
    // nothing renders into a dead window (renderFrame also compares the live handle against the bound one),
    // and the backend is handed the NEW handle when it appears -- which is the move the SDK contract
    // describes (RenderBackend::setWindowHandle "re-announce the handle to follow a new surface"), and which
    // keeps the device and every compiled pipeline. Shutting the engine down here was what made every
    // recreation cost a full session rebuild.
    d->surface_ok = false;

    // The published state goes back with it: Presenting claims the area shows render output, which is not true of
    // a window that is being replaced, and the control stops showing the area on that transition - which is what
    // keeps the area (a hole the container punches for the native window) out of sight until the new window has a
    // frame in it. The session itself is untouched (see the flags above); the next attach republishes Attached.
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
        // The window Qt shows now is not the one the backend is bound to. Two cases, and both attach here:
        //  - the platform window was recreated underneath: an ESTABLISHED session follows it by itself - that is the
        //    surface owning its own window, not the surface guessing a timing;
        //  - there was never a session, because the constructor's try landed before the layout gave the window a
        //    size: this is that retry. A surface that reaches a usable window attaches there instead of waiting for
        //    the host to call init() again.
        init();
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

bool SurfaceWindow::isAttaching() const noexcept
{
    return d->attaching.load(std::memory_order_acquire);
}

vn::async::Task<bool> SurfaceWindow::initAsync()
{
    return attach(/*initialize_on_pool=*/true);
}

vn::async::Task<bool> SurfaceWindow::attach(bool initialize_on_pool)
{
    // Everything that touches Qt - the handle, the state, the view, the frames - runs on this thread; the only
    // difference between the callers is where the backend's own initialize() runs (see the declaration).
    if (d->backend_live && nativeHandle() == d->session_handle) {
        co_return true;  // Already bound to this very surface (backend_live implies has_session).
    }
    if (d->engine == nullptr) {
        co_return false;
    }

    if (d->state == SurfaceState::Failed) {
        // An explicit attach is the caller saying "try again": clear the verdict and let this attempt speak for
        // itself.
        d->failure_reason.clear();
        setState(SurfaceState::Pending);
    }

    // Pick the default backend if the host attached none, then attach when the native surface is usable. If it is
    // not usable yet the attach is deferred and this call reports the previous result: calling again is the caller's
    // retry, and an established session re-attaches by itself when Qt recreates the platform window, so we never
    // fall back to creating a separate window.
    useDefaultBackend();

    if (d->engine->backend() == nullptr) {
        // Nothing to attach to at all: no render backend plugin is registered. Waiting will not change that, so
        // say it once instead of staying Pending forever.
        failAttach(u8"no render backend is registered");
        co_return false;
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
    // Test hatch: force a platform-window recreation after VINE_RECREATE_SURFACE_MS milliseconds, which is the
    // event the follow path below exists for. See recreateSurface(). Armed by the first attach only: following a
    // recreated window re-enters here, and re-arming would recreate the surface forever.
    armRecreateHatch();

    void* const handle = nativeHandle();
    if (handle == nullptr || width() <= 0 || height() <= 0) {
        // No usable native surface yet (Qt destroying/recreating the platform window, or the window not laid out
        // yet): defer so we never attach to a dead or empty handle. The surface events that follow (a resize, a
        // recreated surface) bring the caller back: a first attach is the caller's retry (see handleUpdate()), an
        // established session follows the new window by itself.
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
        co_return d->backend_live;
    }
    if (d->has_session && handle != d->session_handle) {
        // The platform window was recreated and this is the new one. Re-announce the handle instead of tearing the
        // session down: setWindowHandle is the contract for "follow me onto this surface", so the backend moves -
        // the device and every compiled pipeline stay - and rebuilds the session itself when it cannot serve the
        // new window (a different swapchain format). Shutting the engine down here forced the expensive path on
        // every recreation; the vsg backend's windowBuildCount() is what tells the two apart (flat after a move,
        // +1 after a rebuild).
        vn::logging::defaultLogger().info(
            "[RenderControl] the render surface was recreated: re-announcing the new handle so the backend can follow it");
    }
    // Give the engine the native window the backend must attach to; the handle is refreshed here so a re-announce
    // after a surface recreate uses the new handle (and a backend that can move does exactly that).
    d->engine->setWindowHandle(handle);

    // The one stretch that only needs the handle: the device, the session and every pipeline are built here, on a
    // pool worker, while the application thread goes back to its loop (that is the point - the boot keeps reporting,
    // repainting and taking input through it). Nothing to come back to without an application (and so without an
    // event loop): the same work runs inline, exactly as it does for a synchronous caller.
    const bool attach_on_pool = initialize_on_pool && Application::current() != nullptr;

    // 整段 attach（含那一帧）都在这个作用域里：它是宿主的“现在别动场景”信号。
    const AttachScope attaching(d->attaching);
    bool              live = false;
    if (attach_on_pool) {
        vn::graphics::RenderEngine* const engine_ptr = d->engine.get();
        co_await vn::async::run([engine_ptr, &live] { live = engine_ptr->initialize(); });
        if (live) {
            // The session's one-time frame (pass graphs, program slots, compiled pipelines - measured ~166-184 ms).
            // It belongs to the attach, not to "some later tick", so it is paid here and on the same worker; what is
            // left for the application thread is a present.
            // ⚠️ It reads the scene graph: a host must not mutate the scene while this is in flight (see
            // RenderControl::initAsync()).
            co_await vn::async::run([engine_ptr] { engine_ptr->frame(); });
        }
        co_await MainThreadDispatcher::resumeOnMainThread();
    }
    else {
        live = d->engine->initialize();
    }

    d->backend_live = live;
    if (!live) {
        // The platform would not build a surface for an unmapped window (Wayland wants the surface configured
        // first; X11 and Windows accept an unmapped one). Ask the control to put the surface on screen so the next
        // attach can use it: shown-but-unpresented is the state the hidden-surface path exists to avoid, so this is
        // the fallback, not the design - and it is said out loud once, because it also explains a longer startup on
        // that platform. The next attach is the caller's retry on a first attach and the surface's own follow of a
        // recreated window on an established one.
        if (!d->needs_visible_surface) {
            d->needs_visible_surface = true;
            vn::logging::defaultLogger().info(
                "[RenderControl] attaching to a surface that is not on screen failed: this platform wants a visible window, showing the surface so the next attach can use it");
            if (on_needs_visible_surface) {
                on_needs_visible_surface();
            }
        }
        co_return false;
    }

    d->has_session    = true;
    d->session_handle = handle;
    setState(SurfaceState::Attached);
    // What the caller sees is the widget that holds this surface, and RenderControl keeps it hidden until a frame is
    // in the surface (see its constructor) - a hidden widget is not painted, so the hole Qt's window container
    // punches for the embedded window is not there either. The same rule covers a recreated platform window: it went
    // back to Pending while the new window was being taken over (see handleDestroyed()).
    //
    // The current surface size goes to the backend and the view so the frame below is built at the size the surface
    // has now (undistorted aspect, viewport tracking the surface). At a first attach that is the size the platform
    // window happens to have (the layout has not run yet - measured 160x160 here against the 752x480 of the settled
    // window), which is why the frame below is the warm-up one while the control is not on screen yet.
    d->engine->resize(width(), height());
    d->view->onSurfaceResized(width(), height());
    // The frame the attach owes. On screen it is the present, and the transition to Presenting - the state the
    // control shows the render area on. Off screen there is nothing to present, so it is the warm-up frame, and only
    // when this call is the one that has to pay it: the pool already paid it when the attach ran there.
    if (isOnScreen()) {
        renderFrame();
    }
    else if (!attach_on_pool) {
        prewarmFrame();
    }
    // Settle frames come after every attach: measured, without them the gate's first sample (the window just dragged
    // to 378x247) is a flat 66.37% background, identically twice - the re-presents after an attach are what makes
    // the picture at the new size land. An attach's frame count is therefore "warm-up + present + 3 settle", not
    // redundancy (questioned 2026-09-27, denied by measurement).
    requestSettleFrames();
    co_return true;
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
    if (d->engine == nullptr) {
        return;
    }
    // Never run the vsg frame loop (acquire/present) against a stale surface: acquireNextFrame()
    // calls Window::resize() on a dead HWND and spams validation errors. Only render while the
    // backend is attached to the surface the QWindow currently reports and the control is on screen.
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
    //
    // The surface's own visibility is deliberately NOT part of that question: it is hidden until it has
    // something in it (see attach()), so the first frame of a session - the one that fills it -
    // is rendered into a hidden surface on purpose.
    void* h = nativeHandle();
    if (h == nullptr) {
        return;
    }
    if (d->host == nullptr || !d->host->isVisible()) {
        return;
    }
    if (d->has_session && h != d->session_handle) {
        // Qt recreated the native surface (new HWND) but no surface/resize
        // notice was observed; rebind to the live window so we never keep
        // presenting to a dead handle. init() hands the new handle to the
        // backend and renders the first frame on that surface, then returns.
        init();
        return;
    }
    if (d->backend_live) {
        d->engine->frame();
        // The frame is in the surface, so the surface is what the area should show from now on: this is
        // where it goes on screen (it stayed hidden until now, see attach()). A backend that
        // could not present reports that on the diagnostics channel rather than through this call, which
        // is why the state is "a frame was submitted", not "the swapchain confirmed it". The control shows the
        // area on this transition (the widget visibility rule lives there), so a frame at the current size is
        // also what makes the render output visible.
        setState(SurfaceState::Presenting);
    }
}

bool SurfaceWindow::isOnScreen() const
{
    return d->host != nullptr && d->host->isVisible();
}

void SurfaceWindow::prewarmFrame()
{
    // One frame, at the size the platform window has right now, before the control is on screen: what it buys is
    // the one-time part of a session (pass graphs, program slots, compiled pipelines - measured 183.6 ms for the
    // demo's 5 slots against a from-scratch build), paid while the host's startup frame still covers the window.
    // The size change that follows is served in place (the backend keeps the slots and replaces the images, views
    // and framebuffers), so the first frame the user can see costs a resize and a record instead of a build.
    if (d->engine == nullptr || !d->backend_live) {
        return;
    }
    if (nativeHandle() == nullptr || width() <= 0 || height() <= 0) {
        return;
    }
    // Nothing is published and nothing is shown: a frame rendered for a size the surface is about to leave is not
    // something to put on screen, and this class never decides that on its own (see renderFrame()).
    d->engine->frame();
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

bool SurfaceWindow::eventFilter(QObject* watched, QEvent* event)
{
    // The container widget the surface is embedded through: its resize covers a maximize, and its show is the
    // event that says the control is on screen now - which is what makes the first frame possible (rendering is
    // refused until then, see renderFrame()) and what carries the surface's final size (the layout pass that
    // shows the widget is the one that gives it).
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
