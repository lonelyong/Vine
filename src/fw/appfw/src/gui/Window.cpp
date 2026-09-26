#include <vine/appfw/gui/Window.hpp>

#include <QChildEvent>
#include <QDialog>
#include <QEvent>
#include <QWidget>

#include "Convert.hpp"
#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(Window, Control)

namespace
{

/**
 * @brief Watches a window's widget tree, remembers that it painted and says so once.
 *
 * The window's own paint and the paint of anything inside it both count: which of them comes first is the layout's
 * business, and a window whose area is covered by opaque children paints none of it itself. Children that appear
 * later - a dock panel, the progress bar that goes into the status bar - are picked up through their parent's
 * ChildAdded notice, so nothing inside a window has to exist before the window is watched.
 */
class PaintWatcher : public QObject
{
  public:
    /**
     * @brief Watches a tree for its first paint and reports it through \a first_paint.
     *
     * @param parent      QObject parent, or null.
     * @param first_paint Signal of the window being watched; triggered once, on the first paint.
     */
    PaintWatcher(QObject* parent, vn::Signal<>& first_paint)
      : QObject(parent)
      , first_paint_(first_paint)
    {}

    /**
     * @brief Returns whether anything in the watched tree has painted.
     *
     * @return true once a paint event has been seen.
     */
    bool painted() const noexcept
    {
        return painted_;
    }

    /**
     * @brief Starts watching a widget and everything already inside it.
     *
     * @param widget Widget whose paints are of interest; must not be null.
     */
    void watch(QWidget& widget)
    {
        watchObject(widget);
        for (QWidget* child : widget.findChildren<QWidget*>()) {
            watchObject(*child);
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
            case QEvent::Paint:
                if (!painted_) {
                    painted_ = true;
                    first_paint_.trigger();
                }
                break;
            case QEvent::ChildAdded:
                // Seen from the parent's side, which is why watching a widget is enough to pick up its own children: a
                // widget that appears while the window is up paints like everything else and has to be counted.
                if (auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(event)->child())) {
                    watchObject(*child);
                }
                break;
            default:
                break;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    /// Watches one object; children it gains afterwards arrive through the notice above.
    void watchObject(QObject& object)
    {
        object.installEventFilter(this);
    }

    /// Signal of the window being watched: the watcher and the window die together (see ~Window), so this stays valid.
    vn::Signal<>& first_paint_;

    bool painted_{ false };
};

/// Watches a window's widget tree for its first paint.
///
/// @param native      Top-level widget of the window, or null for a window without one.
/// @param first_paint Signal to trigger on that first paint.
/// @return A watcher the caller owns, or null when there is no widget to watch.
PaintWatcher* makePaintWatcher(QWidget* native, vn::Signal<>& first_paint)
{
    if (native == nullptr) {
        return nullptr;
    }

    auto* watcher = new PaintWatcher(nullptr, first_paint);
    watcher->watch(*native);
    return watcher;
}

} // namespace

Window::Window(QWidget* native, bool owns)
  : Control(new WindowData(), native, owns)
{
    static_cast<WindowData*>(d)->paint_watcher = makePaintWatcher(native, first_paint);
}

Window::~Window()
{
    // The watcher is not a QObject child of the widget it watches: it belongs to the window, like the widget itself,
    // and the widget can be destroyed first when the window does not own it - so its lifetime is managed here.
    // d is released by UIElement.
    auto* data         = static_cast<WindowData*>(d);
    delete data->paint_watcher;
    data->paint_watcher = nullptr;
}

void Window::setWindowTitle(const String& t)
{
    if (auto* w = impl<QWidget>())
        w->setWindowTitle(Convert::toQString(t));
}

String Window::windowTitle() const
{
    auto* w = impl<QWidget>();
    return w ? Convert::fromQString(w->windowTitle()) : String();
}

void Window::setModal(bool on)
{
    if (auto* w = impl<QWidget>())
        w->setWindowModality(on ? Qt::WindowModal : Qt::NonModal);
}

bool Window::modal() const
{
    auto* w = impl<QWidget>();
    return w && w->windowModality() != Qt::NonModal;
}

void Window::show()
{
    if (auto* w = impl<QWidget>())
        w->show();
}

void Window::close()
{
    if (auto* w = impl<QWidget>())
        w->close();
}

int Window::exec()
{
    auto* w   = impl<QWidget>();
    auto* dlg = qobject_cast<QDialog*>(w);
    return dlg ? dlg->exec() : 0;
}

void Window::resize(int w, int h)
{
    if (auto* widget = impl<QWidget>())
        widget->resize(w, h);
}

void Window::setWindowState(WindowState state)
{
    auto* w = impl<QWidget>();
    if (!w)
        return;
    Qt::WindowState qstate;
    if (state == WindowState::Minimized)
        qstate = Qt::WindowState::WindowMinimized;
    else if (state == WindowState::Maximized)
        qstate = Qt::WindowState::WindowMaximized;
    else
        qstate = Qt::WindowState::WindowNoState;
    w->setWindowState(qstate);
}

WindowState Window::windowState() const
{
    auto*       w     = impl<QWidget>();
    WindowState state = WindowState::Normal;
    if (!w)
        return state;
    const auto qstate = w->windowState();
    if (qstate & Qt::WindowState::WindowFullScreen)
        state = WindowState::Maximized;
    else if (qstate & Qt::WindowState::WindowMaximized)
        state = WindowState::Maximized;
    else if (qstate & Qt::WindowState::WindowMinimized)
        state = WindowState::Minimized;
    else
        state = WindowState::Normal;
    return state;
}

void Window::setStartupPosition(StartupPosition position)
{
    if (auto* w = impl<QWidget>())
        w->setProperty("vine.startupPosition", static_cast<int>(position));
}

StartupPosition Window::startupPosition() const
{
    auto* w = impl<QWidget>();
    if (!w)
        return StartupPosition::Manual;
    return static_cast<StartupPosition>(w->property("vine.startupPosition").toInt());
}

void Window::activate()
{
    if (auto* w = impl<QWidget>())
        w->activateWindow();
}

bool Window::isActive() const
{
    auto* w = impl<QWidget>();
    return w && w->isActiveWindow();
}

bool Window::hasPainted() const noexcept
{
    const auto* watcher = static_cast<const PaintWatcher*>(static_cast<const WindowData*>(d)->paint_watcher);
    return watcher != nullptr && watcher->painted();
}

Window::Window(UIElementData* data, QWidget* native, bool owns)
  : Control(data, native, owns)
{
    static_cast<WindowData*>(d)->paint_watcher = makePaintWatcher(native, first_paint);
}

VN_APPFWGUI_NS_END
