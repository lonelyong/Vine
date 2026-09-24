#include <vine/appfw/gui/RenderControl.hpp>

#include <QVBoxLayout>
#include <QWidget>

#include "ControlData.hpp"
#include "SurfaceWindow.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RenderControl, Control)

struct RenderControl::Impl : public ControlData {
    /// The surface and the render session on it. It is created bare and handed to the window
    /// container, which owns it from then on, so it lives exactly as long as this control's widget
    /// tree does.
    SurfaceWindow* surface = nullptr;
    /// The widget that holds the surface (a QWindowContainer). What the host sees of this control is this
    /// widget's visibility: a frame in the surface is what makes it worth showing (see the constructor).
    QWidget* container = nullptr;
};

RenderControl::RenderControl()
  : Control(new Impl(), new QWidget())
{
    auto* data = dptr();

    // The surface is built here, where the control can simply keep it: Qt exposes no accessor for the
    // window inside a QWindowContainer (the container is not the window, and its own windowHandle() is
    // null), so a factory that only returned the host widget would leave the surface to be looked up
    // again - a round-trip this does not need.
    data->surface = new SurfaceWindow(impl<QWidget>());

    // A QWidget-based host embeds the surface through a window container, which is laid out to fill the
    // host: the native window is then exactly the area this control was given, wherever the host puts it.
    auto* container = QWidget::createWindowContainer(data->surface, impl<QWidget>());
    auto* layout    = new QVBoxLayout(impl<QWidget>());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
    data->container = container;

    // The area shows render output exactly when a frame is in the surface, and this widget is where that rule
    // lives. Qt's window container fills its own rectangle with Qt::TRANSPARENT - it punches a hole for the native
    // child window it holds - so painting a background here is pointless (that fill is erased in the same pass)
    // and a *visible* container that has no frame in it is a hole whatever is behind the window shows through.
    // Hidden, the widget is not painted at all: there is no hole, and the window's own background shows in its
    // place. The container also owns the embedded window's visibility (Qt shows and hides it together with the
    // container), so nothing has to be called on the surface itself.
    container->setVisible(false);

    // The container reports its resizes and shows to the surface: a resize of the host is what covers a
    // maximize, on which the embedded window misses its own resize, and the show is the event that says the
    // control has reached the screen (which is what allows rendering at all - see SurfaceWindow::renderFrame).
    container->installEventFilter(data->surface);

    // Everything below is forwarding. The state signal is the one part of it that has to be relayed
    // rather than merely passed on: transitions are decided on the surface, and the control re-publishes
    // them so hosts keep the single subscription point this class documents - and shows or hides the area
    // with them, which is what the host observes as "there is a picture" / "there is nothing yet".
    data->surface->on_state_changed = [this](SurfaceState state) {
        auto* data = dptr();
        // A frame is in the surface: the area can show it. Back to Pending, a platform window is being replaced
        // and the surface has nothing to show any more, so the area goes back into the background.
        data->container->setVisible(state == SurfaceState::Presenting);
        state_changed.trigger(state);
    };
    data->surface->on_needs_visible_surface = [this] {
        // The platform wants a surface that is on screen before it will attach (see SurfaceWindow): showing the
        // area is the only way to give it one, hole and all.
        dptr()->container->setVisible(true);
    };
}

RenderControl::~RenderControl()
{
    // Stop listening before the base destroys the widget: the window outlives this body by a moment,
    // and its teardown reports the platform surface going away. Nothing is freed here - the Impl is
    // the object UIElement keeps in its d and releases with the rest of the control.
    auto* data = dptr();
    if (data != nullptr && data->surface != nullptr) {
        data->surface->on_state_changed        = nullptr;
        data->surface->on_needs_visible_surface = nullptr;
    }
}

vn::graphics::RenderEngine* RenderControl::engine() const
{
    return dptr()->surface->engine();
}

vn::graphics::SceneView* RenderControl::view() const
{
    return dptr()->surface->view();
}

bool RenderControl::init()
{
    return dptr()->surface->init();
}

void RenderControl::renderFrame()
{
    dptr()->surface->renderFrame();
}

void RenderControl::fitToScreen()
{
    dptr()->surface->fitToScreen();
}

double RenderControl::devicePixelRatio() const
{
    return dptr()->surface->devicePixelRatio();
}

RenderControl::SurfaceState RenderControl::state() const
{
    return dptr()->surface->state();
}

bool RenderControl::hasPresented() const
{
    // The two terminal states: a frame is in the surface (which is how the surface came on screen), or the backend
    // cannot come up at all. Both mean the caller stops waiting - see the declaration.
    const SurfaceState current = state();
    return current == SurfaceState::Presenting || current == SurfaceState::Failed;
}

String RenderControl::failureReason() const
{
    return dptr()->surface->failureReason();
}

inline auto RenderControl::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RenderControl::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
