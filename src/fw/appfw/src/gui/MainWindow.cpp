#include <vine/appfw/gui/MainWindow.hpp>

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QDockWidget>
#include <QGuiApplication>
#include <QPalette>
#include <QScreen>
#include <QSize>
#include <QTimer>
#include <QWindow>
#include <SARibbon.h>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/Gui.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/StatusBar.hpp>

#include "MainWindowImpl.hpp"
#include "WindowData.hpp"

V_APPFWGUI_NS_BEGIN

V_OBJECT_META_IMPL(MainWindow, Window)

struct MainWindow::Impl : public WindowData {
    std::unique_ptr<RibbonBar>        ribbon_bar;
    std::unique_ptr<StatusBar>        status_bar;
    std::unique_ptr<DockPanelManager> dock_panel_mgr;
    RenderControl*                    primary_render_control = nullptr;

    ~Impl();
};

MainWindow::Impl::~Impl() = default;

namespace
{

using itype = MainWindowImpl;

MainWindow* s_current_main_window = nullptr;

} // namespace

MainWindowImpl::MainWindowImpl(QWidget* parent)
  : SARibbonMainWindow(parent)
{
    setScreen(qApp->primaryScreen());

    // Subscribe to the app theme: GuiApplication is the sole theme authority;
    // here we only map and apply it.
    if (auto* app = obj_cast<GuiApplication>(Application::current())) {
        theme_handler_ = app->theme_changed.subscribe([this](Theme) { QTimer::singleShot(0, this, [this] { applyAppTheme(); }); });
    }

    QTimer::singleShot(0, this, [this] { applyAppTheme(); });
}

MainWindowImpl::~MainWindowImpl() = default;

void MainWindowImpl::applyAppTheme()
{
    const auto* app = obj_cast<GuiApplication>(Application::current());
    if (app == nullptr) {
        return;
    }
    setRibbonTheme(app->theme() == Theme::Dark ? SARibbonTheme::RibbonThemeDark : SARibbonTheme::RibbonThemeOffice2021Blue);
}

MainWindow::MainWindow()
  : Window(new Impl(), new MainWindowImpl(nullptr))
{
    s_current_main_window = this;

    dptr()->ribbon_bar     = std::make_unique<RibbonBar>(this);
    dptr()->status_bar     = std::make_unique<StatusBar>(this);
    dptr()->dock_panel_mgr = std::make_unique<DockPanelManager>();
    dptr()->dock_panel_mgr->setWindow(this);

    impl<itype>()->setWindowTitle("Vine");
    impl<itype>()->setMinimumSize(QSize(800, 600));
    impl<itype>()->setCentralWidget(static_cast<QWidget*>(dptr()->dock_panel_mgr->root()->impl()));
    impl<itype>()->setStatusBar(dptr()->status_bar->impl<QStatusBar>());

    // QMainWindow::setStatusBar takes ownership of the status bar widget, so the
    // wrapper must not delete it (owns_impl=false avoids a double-free with Qt's
    // parent-child cleanup when the window is destroyed first).
    dptr()->status_bar->setOwnsImpl(false);

    auto ribbon = impl<itype>()->ribbonBar();
    ribbon->setContentsMargins(5, 0, 5, 0);
    ribbon->applicationButton()->setText("File");
}

MainWindow::~MainWindow()
{
    if (s_current_main_window == this) {
        s_current_main_window = nullptr;
    }
    // d (and its owned ribbon/status/dock members) is deleted by UIElement.
}

MainWindow* MainWindow::current()
{
    return s_current_main_window;
}

raw_ptr<RibbonBar> MainWindow::ribbonBar() const
{
    return dptr()->ribbon_bar.get();
}

raw_ptr<StatusBar> MainWindow::statusBar() const
{
    return dptr()->status_bar.get();
}

raw_ptr<DockPanelManager> MainWindow::dockPanelManager() const
{
    return dptr()->dock_panel_mgr.get();
}

void MainWindow::setPrimaryRenderControl(RenderControl* control)
{
    dptr()->primary_render_control = control;
}

RenderControl* MainWindow::primaryRenderControl() const
{
    return dptr()->primary_render_control;
}

inline auto MainWindow::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto MainWindow::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
