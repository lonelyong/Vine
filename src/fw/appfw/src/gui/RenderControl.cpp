#include <vine/appfw/gui/RenderControl.hpp>

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
#include <QVariant>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <string>

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/logging/Log.hpp>

#include <vine/window/InputEvent.hpp>
#include <vine/window/KeyCode.hpp>
#include <vine/window/MouseButton.hpp>

#include <functional>

V_APPFWGUI_NS_BEGIN

V_OBJECT_META_IMPL(RenderControl, Control)

namespace
{

/**
 * @brief Creates the host QWidget with a nested native QWindow render surface.
 *
 * The QWindow is the actual render surface the backend binds to; it is nested
 * inside a QWidget (QWidget::createWindowContainer) so the QWidget-based
 * Control can host it.
 *
 * @return The host QWidget (owned by the Control base).
 */
QWidget* makeHost()
{
    auto* surface = new QWindow();
    // Vulkan surface: Qt does not composite a raster backing store over the
    // render surface, so the Vulkan content stays visible.
    surface->setSurfaceType(QSurface::VulkanSurface);

    auto* widget = QWidget::createWindowContainer(surface);

    // Keep the surface pointer on the host so RenderControl can recover it:
    // windowHandle() on a non-top-level container returns null, so the QWindow
    // cannot be queried back reliably.
    widget->setProperty("_vine_surface", QVariant::fromValue(static_cast<void*>(surface)));
    return widget;
}

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
 * @brief Translates Qt events from the render surface and its host widget.
 *
 * Installed as an event filter on the native QWindow render surface and on
 * the host QWidget. It forwards resizes and input events to RenderControl and
 * reports native-surface destruction through std::function callbacks, so the
 * render control handles Qt's native-window lifecycle directly (no separate
 * window-context object).
 */
class SurfaceHostFilter : public QObject {
  public:
    using MouseFn  = std::function<void(const vine::window::MouseEvent&)>;
    using KeyFn    = std::function<void(const vine::window::KeyEvent&)>;
    using ScrollFn = std::function<void(const vine::window::ScrollEvent&)>;
    using ResizeFn = std::function<void(int, int)>;
    using VoidFn   = std::function<void()>;

    SurfaceHostFilter(QObject* surface, QObject* host)
      : QObject(nullptr)
    {
        if (surface != nullptr) {
            surface->installEventFilter(this);
        }
        if (host != nullptr) {
            host->installEventFilter(this);
        }
    }

    MouseFn on_mouse;
    KeyFn on_key;
    ScrollFn on_scroll;
    ResizeFn on_resize;
    VoidFn on_created;
    VoidFn on_destroyed;
    VoidFn on_update;

  private:
    // Last size forwarded through on_resize, used to de-duplicate the host
    // and surface resize events of the same layout pass.
    int last_w_ = -1;
    int last_h_ = -1;

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        switch (event->type()) {
            case QEvent::Resize: {
                auto* e = static_cast<QResizeEvent*>(event);
                const int w = e->size().width();
                const int h = e->size().height();
                // A container resize delivers a QResizeEvent to both the host
                // QWidget and the nested surface QWindow with the same final
                // size. Forward only the first so we do not rebuild/render
                // twice per resize (an identical size needs no rebuild anyway).
                if (w == last_w_ && h == last_h_) {
                    break;
                }
                last_w_ = w;
                last_h_ = h;
                if (on_resize) {
                    on_resize(w, h);
                }
                break;
            }
            case QEvent::PlatformSurface: {
                auto* e = static_cast<QPlatformSurfaceEvent*>(event);
                // Qt may destroy and recreate the native platform surface (new
                // HWND) on layout changes. Report both phases: the backend is
                // released on destruction and re-attached once the new surface
                // is created and laid out.
                if (e->surfaceEventType()
                    == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                    if (on_destroyed) {
                        on_destroyed();
                    }
                } else if (e->surfaceEventType()
                           == QPlatformSurfaceEvent::SurfaceCreated) {
                    if (on_created) {
                        on_created();
                    }
                }
                break;
            }
            case QEvent::UpdateRequest: {
                if (on_update) {
                    on_update();
                }
                break;
            }
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease: {
                auto* e = static_cast<QMouseEvent*>(event);
                vine::window::MouseEvent me;
                me.button    = toMouseButton(e->button());
                me.modifiers = toModifiers(e->modifiers());
                me.x         = e->position().x();
                me.y         = e->position().y();
                me.pressed   = (event->type() == QEvent::MouseButtonPress);
                if (on_mouse) {
                    on_mouse(me);
                }
                break;
            }
            case QEvent::MouseMove: {
                auto* e = static_cast<QMouseEvent*>(event);
                vine::window::MouseEvent me;
                me.button    = vine::window::MouseButton::None;
                me.modifiers = toModifiers(e->modifiers());
                me.x         = e->position().x();
                me.y         = e->position().y();
                if (on_mouse) {
                    on_mouse(me);
                }
                break;
            }
            case QEvent::Wheel: {
                auto* e     = static_cast<QWheelEvent*>(event);
                const auto delta = e->angleDelta();
                vine::window::ScrollEvent se;
                // Qt angleDelta is in 1/8-degree units; convert to notches/lines.
                se.deltaX    = delta.x() / 120.0;
                se.deltaY    = delta.y() / 120.0;
                se.modifiers = toModifiers(e->modifiers());
                if (on_scroll) {
                    on_scroll(se);
                }
                break;
            }
            case QEvent::KeyPress:
            case QEvent::KeyRelease: {
                auto* e          = static_cast<QKeyEvent*>(event);
                const bool numpad = bool(e->modifiers() & Qt::KeypadModifier);
                vine::window::KeyEvent ke;
                ke.code      = toKeyCode(e->key(), numpad);
                ke.modifiers = toModifiers(e->modifiers());
                ke.pressed   = (event->type() == QEvent::KeyPress);
                ke.repeat    = e->isAutoRepeat();
                if (on_key) {
                    on_key(ke);
                }
                break;
            }
            default:
                break;
        }
        return false;
    }
};

}  // namespace

struct RenderControl::Impl {
    vine::intrusive_ptr<vine::graphics::RenderEngine> engine;
    vine::intrusive_ptr<vine::graphics::SceneView> view;
    QWindow* surface = nullptr;
    QObject* surface_filter = nullptr;
    bool wired = false;
    bool initialized = false;
    bool init_ok = false;
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
    void* initialized_handle = nullptr;
};

RenderControl::RenderControl()
  : Control(makeHost())
  , d(new Impl())
{
    // Recover the render surface QWindow stashed by makeHost (windowHandle()
    // is unreliable for a non-top-level container widget).
    QWindow* surface = static_cast<QWindow*>(
        impl<QWidget>()->property("_vine_surface").value<void*>());
    d->surface = surface;
    d->engine  = vine::intrusive_ptr<vine::graphics::RenderEngine>(
        new vine::graphics::RenderEngine());
    // Design B: RenderEngine starts empty (no passes) and is a pure
    // scheduler - it holds no camera and no content scene. The interactive
    // primary view - its camera, content scene and orbit manipulator - lives
    // in a SceneView that borrows the engine and binds its content to the
    // window pass it registers (SceneView::ensureWindowPass, called in
    // init()).
    d->view  = vine::intrusive_ptr<vine::graphics::SceneView>(
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
        const std::string message = diagnostic.message.stdstr();
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
}

RenderControl::~RenderControl()
{
    delete d->surface_filter;
    delete d;
}

vine::graphics::RenderEngine* RenderControl::engine() const
{
    return d->engine.get();
}

vine::graphics::SceneView* RenderControl::view() const
{
    return d->view.get();
}

void* RenderControl::nativeHandle() const
{
    return (d->surface != nullptr) ? reinterpret_cast<void*>(d->surface->winId()) : nullptr;
}

int RenderControl::surfaceWidth() const
{
    return (d->surface != nullptr) ? d->surface->width() : 0;
}

int RenderControl::surfaceHeight() const
{
    return (d->surface != nullptr) ? d->surface->height() : 0;
}

double RenderControl::devicePixelRatio() const
{
    return (d->surface != nullptr) ? d->surface->devicePixelRatio() : 1.0;
}

bool RenderControl::surfaceVisible() const
{
    return (d->surface != nullptr) && d->surface->isVisible();
}

bool RenderControl::init()
{
    // Idempotent: repeated calls are harmless and return the current result.
    if (d->initialized) {
        return d->init_ok;
    }
    if (d->engine == nullptr || d->surface == nullptr) {
        return false;
    }

    // Wire the backend + event handling once, then attach when the native
    // surface is usable (the host calls init() after the window is shown). If
    // the surface is not ready yet the attach is deferred: the host retries
    // init(), or a later surface/resize notice re-attaches, so we never fall
    // back to creating a separate window.
    wireEvents();

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
    if (d->init_ok) {
        // The deferred app_shell init() can land before the dock layout has
        // fully settled, and Qt may still recreate the platform window right
        // after. Re-check a few times so the backend ends up bound to and
        // rendering at the live surface size; each check is a no-op when the
        // surface is unchanged (handleSurfaceUpdate deduplicates via the
        // coalescing flag).
        QTimer::singleShot(150, d->surface, [this] { scheduleSurfaceUpdate(); });
        QTimer::singleShot(400, d->surface, [this] { scheduleSurfaceUpdate(); });
        QTimer::singleShot(900, d->surface, [this] { scheduleSurfaceUpdate(); });
    }
    return d->init_ok;
}

void RenderControl::wireEvents()
{
    if (d->wired) {
        return;
    }
    d->wired = true;

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

    // The host widget is the Control's own QWidget; the surface QWindow is
    // nested inside it. One filter observes both (resize on the host also
    // covers maximize, on which the embedded QWindow misses its resize).
    auto* filter = new SurfaceHostFilter(d->surface, impl<QWidget>());
    d->surface_filter = filter;

    // Qt-translated input is pushed to the view (whose manipulator drives the
    // camera), then a frame is rendered so the view follows the interaction
    // live. The Vulkan surface is render-on-demand: vsg only draws when
    // renderFrame() runs, so without a refresh here orbit/pan/zoom would
    // update the camera but never repaint. Pure hover moves are skipped (no
    // button held => the manipulator does not change the view); press/release
    // and scroll/key always refresh.
    filter->on_mouse = [this](const vine::window::MouseEvent& e) {
        // A right press starts a pan drag; a release close to the press (no
        // movement) is a plain right-click and opens the context menu.
        const bool is_right = e.button == vine::window::MouseButton::Right;
        if (is_right) {
            if (e.pressed) {
                d->right_press_active = true;
                d->right_press_x = e.x;
                d->right_press_y = e.y;
            } else if (d->right_press_active) {
                d->right_press_active = false;
                const double dx = e.x - d->right_press_x;
                const double dy = e.y - d->right_press_y;
                if ((dx * dx + dy * dy) < 36.0) {  // within 6 px: a click
                    QTimer::singleShot(0, [this] { showContextMenu(); });
                }
            }
        }
        d->view->pushEvent(e);
        if (e.button != vine::window::MouseButton::None) {
            d->mouse_down = e.pressed;
        }
        if (e.button != vine::window::MouseButton::None || d->mouse_down) {
            renderFrame();
        }
    };
    filter->on_scroll = [this](const vine::window::ScrollEvent& e) {
        d->view->pushEvent(e);
        renderFrame();
    };
    filter->on_key = [this](const vine::window::KeyEvent& e) {
        d->view->pushEvent(e);
        renderFrame();
    };

    filter->on_resize    = [this](int, int) { onSurfaceResized(); };
    filter->on_created   = [this] { onSurfaceResized(); };
    filter->on_destroyed = [this] { onSurfaceDestroyed(); };
    filter->on_update    = [this] { onSurfaceUpdate(); };
}

void RenderControl::onSurfaceDestroyed()
{
    // Qt destroys and recreates the native platform surface (new HWND) on
    // layout changes. Release the backend now; the next created/resize notice
    // re-attaches to the new surface.
    d->surface_ok = false;
    if (d->initialized) {
        d->engine->shutdown();
    }
    d->initialized = false;
    d->init_ok = false;
    d->initialized_handle = nullptr;
}

void RenderControl::onSurfaceResized()
{
    d->surface_ok = true;
    scheduleSurfaceUpdate();
}

void RenderControl::scheduleSurfaceUpdate()
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
    QTimer::singleShot(0, d->surface_filter, [this] { handleSurfaceUpdate(); });
}

void RenderControl::handleSurfaceUpdate()
{
    d->resize_pending = false;
    void* h = nativeHandle();
    const int w = surfaceWidth();
    const int sh = surfaceHeight();

    // Before init() has wired a backend, the host drives the first attach
    // itself (via init()); never auto-initialize here.
    if (!d->wired || !d->surface_ok || h == nullptr || w <= 0 || sh <= 0) {
        return;
    }

    if (!d->initialized || h != d->initialized_handle) {
        // The native surface was recreated by Qt (new handle) or the backend
        // is down after such a shutdown: attach again now that the surface is
        // created and laid out.
        initializeBackend();
        requestSettleFrames();
        return;
    }

    // Normal resize of the attached surface: rebuild the swapchain at the
    // final native size, refresh the view's camera projection aspect, present,
    // then request settle frames so the resized view is actually displayed.
    d->engine->resize(w, sh);
    d->view->onSurfaceResized(w, sh);
    renderFrame();
    requestSettleFrames();
}

void RenderControl::requestSettleFrames()
{
    // A single present right after a size change can be dropped by the
    // presentation pipeline while the native surface settles (e.g. Vulkan
    // returns VK_ERROR_OUT_OF_DATE_KHR after a swapchain rebuild and, with
    // render-on-demand, no later frame re-presents), leaving a stale image.
    // This is backend-independent. Re-render on a few display-synced updates
    // (QWindow::requestUpdate() -> QEvent::UpdateRequest); each frame also
    // lets the backend re-sync to the current surface size.
    d->settle_frames = 3;
    if (d->surface != nullptr) {
        d->surface->requestUpdate();
    }
}

void RenderControl::onSurfaceUpdate()
{
    if (d->settle_frames > 0) {
        --d->settle_frames;
        renderFrame();
        if (d->settle_frames > 0 && d->surface != nullptr) {
            d->surface->requestUpdate();
        }
    }
}

void RenderControl::initializeBackend()
{
    void* h = nativeHandle();
    if (d->initialized) {
        // Already bound; only re-attach when the native surface was swapped
        // for a new one (Qt recreated the platform window).
        if (h != nullptr && h != d->initialized_handle) {
            d->engine->shutdown();
            d->initialized = false;
            d->init_ok = false;
            d->initialized_handle = nullptr;
        } else {
            return;
        }
    }
    if (h == nullptr || surfaceWidth() <= 0 || surfaceHeight() <= 0) {
        // No usable native surface yet (Qt destroying/recreating the platform
        // window, or the window not laid out yet): defer so we never attach to
        // a dead or empty handle. Retried on SurfaceCreated/expose/resize.
        return;
    }
    // Give the engine the native window the backend must attach to; the
    // handle is refreshed here so re-initialization after a surface recreate
    // uses the new HWND.
    d->engine->setWindowHandle(h);
    d->init_ok = d->engine->initialize();
    if (d->init_ok) {
        d->initialized = true;
        d->initialized_handle = h;
        // Deliver the current surface size so the view's camera projection
        // aspect is set on the first frame (undistorted) and the backend
        // viewport tracks the surface.
        d->engine->resize(surfaceWidth(), surfaceHeight());
        d->view->onSurfaceResized(surfaceWidth(), surfaceHeight());
        renderFrame();
        // The first attach can land mid-layout (e.g. the deferred init from
        // app_shell runs at 100ms, before the dock layout has settled), so the
        // native surface may still be resized afterwards. Request settle frames
        // so a few display-synced updates re-sync the swapchain to the final
        // size and the first content is actually presented (a single present
        // against a soon-to-resize surface can otherwise be dropped, leaving
        // the view empty).
        requestSettleFrames();
    }
    // On failure, initialized stays false so expose/resize can retry.
}

void RenderControl::fitToScreen()
{
    if (d->view != nullptr) {
        if (!d->view->fitToScreen()) {
            d->view->home();
        }
    }
    renderFrame();
}

void RenderControl::showContextMenu()
{
    if (d->view == nullptr) {
        return;
    }
    QMenu menu(impl<QWidget>());
    QAction* fit = menu.addAction(QString::fromUtf8("适应屏幕"));
    QObject::connect(fit, &QAction::triggered, [this] { fitToScreen(); });
    menu.exec(QCursor::pos());
}

void RenderControl::renderFrame()
{
    if (d->engine == nullptr || d->surface == nullptr) {
        return;
    }
    // Never run the vsg frame loop (acquire/present) against a stale or hidden
    // surface: acquireNextFrame() calls Window::resize() on a dead HWND and
    // spams validation errors. Only render while the backend is attached to
    // the surface the QWindow currently reports and the window is visible.
    void* h = nativeHandle();
    if (h == nullptr || !surfaceVisible()) {
        return;
    }
    if (d->initialized && h != d->initialized_handle) {
        // Qt recreated the native surface (new HWND) but no surface/resize
        // notice was observed; rebind to the live window so we never keep
        // presenting to a dead handle. initializeBackend() releases the old
        // backend and renders the first frame on the new surface, then
        // returns.
        initializeBackend();
        return;
    }
    if (d->initialized) {
        d->engine->frame();
    }
}

V_APPFWGUI_NS_END
