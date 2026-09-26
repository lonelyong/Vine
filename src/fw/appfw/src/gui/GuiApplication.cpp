#include <vine/appfw/gui/GuiApplication.hpp>

#include <array>
#include <cassert>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPalette>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QWidget>

#if defined(Q_OS_WIN) && QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#    include <QSettings>
#endif

#include <vine/appfw/gui/BootSplash.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/ProgressPresenter.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/appfw/gui/StatusBar.hpp>
#include <vine/appfw/gui/VisualUserIO.hpp>
#include <vine/appfw/gui/Window.hpp>

#include <vine/appfw/StartupProgress.hpp>
#include <vine/logging/Log.hpp>

#include "GuiApplicationData.hpp"

#if defined(Q_OS_WIN) && QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
namespace
{

// Qt < 6.5 does not read the Windows colour scheme, so query it directly.
bool isSystemDarkMode()
{
    QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat);

    return settings.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
}

} // namespace
#endif

#if defined(Q_OS_LINUX)
namespace
{

// DockingPanes moves top-level windows and reads the global cursor position,
// neither of which the Wayland platform plugin supports. WSLg exposes both
// DISPLAY and WAYLAND_DISPLAY and this Qt build prefers Wayland, so force
// X11/XWayland when running under WSL unless the user chose a platform.
void selectX11UnderWslg()
{
    if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        return;
    }

    if (qEnvironmentVariableIsSet("WSL_DISTRO_NAME") && qEnvironmentVariableIsSet("DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
}

} // namespace
#endif

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(GuiApplication, Application)

namespace
{

/// Upper bound for the wait for a boot window's first paint; a window system that never shows a window must not keep
/// the host's startup work from running.
constexpr int kFirstPaintDeadlineMs = 300;

/// The windows a boot puts on screen, in the order they appear: the startup frame when there is one, then the main
/// window. Either entry can be null - a boot without a frame has no first one.
///
/// @param d Application data.
/// @return The two windows, in that order.
std::array<Window*, 2> bootWindows(GuiApplicationData* d)
{
    return {static_cast<Window*>(d->boot_splash), static_cast<Window*>(d->main_window)};
}

/// Returns whether every window of \a windows has painted; a window that is not there is not owed one.
///
/// @param windows Windows to ask, as bootWindows() returns them.
/// @return true when nothing is left unpainted.
bool allPainted(const std::array<Window*, 2>& windows)
{
    for (const Window* const window : windows) {
        if (window != nullptr && !window->hasPainted()) {
            return false;
        }
    }
    return true;
}

// Classic Fusion dark palette (matches the Qt >= 6.5 Fusion dark palette).
QPalette createDarkPalette()
{
    QPalette pal;

    // ------------------------ Active / Inactive (normal state) ------------------------
    // *Window*: Generic window/panel background (e.g., main window, dialogs)
    pal.setColor(QPalette::Window, QColor(43, 45, 48)); // very dark gray (bg)
    // *WindowText*: Text on Window backgrounds (e.g., window title, labels)
    pal.setColor(QPalette::WindowText, QColor(230, 230, 230)); // off-white

    // *Base*: Background for text entry and item views (tables, lists, etc.)
    pal.setColor(QPalette::Base, QColor(30, 31, 33)); // darker charcoals (very dark gray)
    // *AlternateBase*: Alternate background for item views (odd/even rows)
    pal.setColor(QPalette::AlternateBase, QColor(37, 39, 42)); // slightly lighter dark gray
    // *Text*: Text on Base/AlternateBase (e.g., QTableWidgetItem text, QLineEdit text)
    pal.setColor(QPalette::Text, QColor(225, 225, 225)); // almost white

    // *Button*: Button and default control background (push buttons, checkboxes, etc.)
    pal.setColor(QPalette::Button, QColor(55, 57, 61)); // dark gray
    // *ButtonText*: Text on buttons/controls
    pal.setColor(QPalette::ButtonText, QColor(230, 230, 230)); // off-white

    // *Highlight*: Background for selected items or selected text
    pal.setColor(QPalette::Highlight, QColor(45, 115, 190)); // steel blue (consistent across themes)
    // *HighlightedText*: Text on Highlight (e.g., selected item text)
    pal.setColor(QPalette::HighlightedText, Qt::white); // white for contrast on blue

    // *ToolTipBase*: Tooltip background
    pal.setColor(QPalette::ToolTipBase, QColor(52, 54, 58)); // dark gray
    // *ToolTipText*: Tooltip text color
    pal.setColor(QPalette::ToolTipText, QColor(240, 240, 240)); // nearly white

    // *BrightText*: High-emphasis text (often error/warning text)
    pal.setColor(QPalette::BrightText, QColor(255, 80, 80)); // bright red
    // *Link*: Hyperlink text color
    pal.setColor(QPalette::Link, QColor(80, 160, 225)); // bright blue-ish

    // *Mid*: Mid-tone color, used for 3D elements (borders, separators, etc.)
    pal.setColor(QPalette::Mid, QColor(90, 92, 96)); // mid gray

    // ------------------------ Disabled state ------------------------
    // Explicitly set the Disabled group for widgets enabled=false.
    // *WindowText* when disabled (text on Window background)
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(125, 127, 130));
    // *Text* when disabled (text on Base/AlternateBase)
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(125, 127, 130));
    // *ButtonText* when disabled
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(135, 137, 140));

    // *Base* when disabled (bg for disabled text controls or table cells)
    pal.setColor(QPalette::Disabled, QPalette::Base, QColor(42, 44, 47));
    // *AlternateBase* when disabled (usually same as Base for disabled)
    pal.setColor(QPalette::Disabled, QPalette::AlternateBase, QColor(42, 44, 47));
    // *Button* when disabled (bg for disabled buttons)
    pal.setColor(QPalette::Disabled, QPalette::Button, QColor(48, 50, 53));
    // *Highlight* when disabled (selection bg in disabled state)
    pal.setColor(QPalette::Disabled, QPalette::Highlight, QColor(65, 80, 95));
    // *HighlightedText* when disabled (text on disabled highlight)
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(160, 160, 160));

    return pal;
}

// Classic Fusion dark palette (matches the Qt >= 6.5 Fusion dark palette).
QPalette createLightPalette()
{
    QPalette pal;

    // ------------------------ Active / Inactive ------------------------
    // *Window*: Generic window/panel background
    pal.setColor(QPalette::Window, QColor(245, 245, 245)); // very light gray
    // *WindowText*: Text on Window backgrounds
    pal.setColor(QPalette::WindowText, QColor(35, 35, 35)); // near-black gray

    // *Base*: Background for text entry and item views
    pal.setColor(QPalette::Base, QColor(255, 255, 255)); // white
    // *AlternateBase*: Alternate background (e.g., odd rows)
    pal.setColor(QPalette::AlternateBase, QColor(248, 248, 248)); // very light gray
    // *Text*: Text on Base/AlternateBase
    pal.setColor(QPalette::Text, QColor(35, 35, 35)); // near-black gray

    // *Button*: Button and control background
    pal.setColor(QPalette::Button, QColor(238, 238, 238)); // light gray
    // *ButtonText*: Text on buttons/controls
    pal.setColor(QPalette::ButtonText, QColor(35, 35, 35)); // near-black

    // *Highlight*: Background for selections
    pal.setColor(QPalette::Highlight, QColor(45, 115, 190)); // same steel blue
    // *HighlightedText*: Text on Highlight
    pal.setColor(QPalette::HighlightedText, Qt::white); // white

    // *ToolTipBase*: Tooltip background
    pal.setColor(QPalette::ToolTipBase, QColor(255, 255, 225)); // pale yellow
    // *ToolTipText*: Tooltip text
    pal.setColor(QPalette::ToolTipText, QColor(35, 35, 35)); // near-black

    // *BrightText*: High-emphasis text
    pal.setColor(QPalette::BrightText, QColor(200, 40, 40)); // dark red
    // *Link*: Hyperlink text color
    pal.setColor(QPalette::Link, QColor(0, 100, 190)); // blue

    // *Mid*: Mid-tone (borders/separators)
    pal.setColor(QPalette::Mid, QColor(170, 170, 170)); // medium gray

    // ------------------------ Disabled state ------------------------
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(155, 155, 155));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(155, 155, 155));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(150, 150, 150));

    pal.setColor(QPalette::Disabled, QPalette::Base, QColor(238, 238, 238));
    pal.setColor(QPalette::Disabled, QPalette::AlternateBase, QColor(238, 238, 238));
    pal.setColor(QPalette::Disabled, QPalette::Button, QColor(232, 232, 232));

    pal.setColor(QPalette::Disabled, QPalette::Highlight, QColor(190, 200, 210));
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(120, 120, 120));

    return pal;
}

// Resolves the "system current theme" into a Theme enum value.
Theme resolveSystemTheme()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? Theme::Dark : Theme::Light;
#elif defined(Q_OS_WIN)
    return isSystemDarkMode() ? Theme::Dark : Theme::Light;
#else
    return Theme::Light;
#endif
}

} // namespace

void GuiApplication::showUserInterface()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());

    // The order is load-bearing: the frame is what covers the boot, so it goes up first, and the main window follows -
    // not after the boot - because an embedded render surface (RenderControl, the VSG backend) creates its swapchain from
    // the native window of the top-level widget, and a window that was never shown has none (the surface then fails to
    // initialize instead of waiting for the window).
    if (d->boot_splash != nullptr) {
        d->boot_splash->show();
    }
    if (d->main_window != nullptr) {
        d->main_window->show();
    }
}

void GuiApplication::whenUserInterfaceIsUp(std::function<void()> then)
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    d->when_up = std::move(then);

    // What is waited for is every window the boot puts on screen, not the first one: the frame covers the boot and the
    // main window is what is around it, and either of them left unpainted is exactly the empty window this hand-off
    // exists to avoid. Nothing has painted yet - the loop has not run - so this waits for the events.
    for (Window* const window : bootWindows(d)) {
        if (window != nullptr && !window->hasPainted()) {
            d->startup_gates.push_back(window->first_paint.connect([this] { startWhenUp(false); }));
        }
    }

    // The deadline is the backstop for a window system that never reports a window as visible: the framework moves on
    // anyway, and says so. The QApplication is the context because this class is not a QObject.
    QTimer::singleShot(kFirstPaintDeadlineMs, d->app, [this] { startWhenUp(true); });

    startWhenUp(false); // Nothing may be owed: no windows at all, or they painted while this was being armed.
}

void GuiApplication::startWhenUp(bool deadline)
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    if (!d->when_up) {
        return; // Already handed over: every source arrives here, and the first one takes the pending callback.
    }

    const bool painted = allPainted(bootWindows(d));
    if (!painted && !deadline) {
        return; // Still owed a window: its own first paint brings us back here.
    }

    const bool        framed = d->boot_splash != nullptr;
    const char* const shown  = framed ? "the startup frame and the main window" : "the main window";
    const long long   waited = d->startup_handed_over.isValid() ? d->startup_handed_over.elapsed() : 0;

    if (painted) {
        if (d->startup_work != nullptr) {
            VN_LOGI("startup work starting {} ms after the application was asked to run: {} {} on screen",
                    waited,
                    shown,
                    framed ? "are" : "is");
        }
    }
    else {
        VN_LOGW("the startup phase is moving on {} ms after the application was asked to run without {} having painted "
                "within {} ms: it stays an empty window until the event loop runs",
                waited,
                shown,
                kFirstPaintDeadlineMs);
    }

    // Handed to the next turn rather than run from here: this can run while a paint event is being dispatched (that is
    // what the signal reports), and the host's startup work repaints the frame on every progress report and finally
    // takes the frame away - neither can happen inside the paint of the window being taken away. Being posted more than
    // once does not matter: the base takes the callback once.
    auto then = std::move(d->when_up);
    d->startup_gates.clear();
    QTimer::singleShot(0, d->app, [then = std::move(then)] { then(); });
}

GuiApplication::GuiApplication(int argc, char** argv)
  : Application(new GuiApplicationData(), argc, argv)
{}

GuiApplication::~GuiApplication()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());

    if (d->boot_splash != nullptr) {
        // The frame dies with the application either way, but a boot the host never ended also leaves the main window
        // it was hiding unshown, which is exactly what the host has to be told about.
        if (!d->boot_ended) {
            VN_LOGW("Startup frame is still showing as the application is destroyed: the host never called "
                   "Application::finishStartup()");
        }
        delete d->boot_splash;
        d->boot_splash = nullptr;
    }

    delete d->main_window;
    // d is deleted by Application::~Application()
}

UserIO* GuiApplication::createUserIO()
{
    return new VisualUserIO;
}

void GuiApplication::init()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    if (d->app != nullptr) {
        return;
    }

#if defined(Q_OS_LINUX)
    selectX11UnderWslg();
#endif

    // QCoreApplication keeps a reference to argc; the data it refers to
    // must stay valid for the whole application lifetime, so pass the
    // stored member instead of a local copy.
    d->app = new QApplication(d->argc, d->argv);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // When following the system, listen for system theme changes and re-resolve.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, d->app, [this, d](Qt::ColorScheme) {
        if (d->follow_system) {
            setTheme(resolveSystemTheme());
        }
    });
#endif

    // Apply the initial theme (follows the system or fixed).
    if (d->follow_system) {
        d->theme = resolveSystemTheme();
    }
    applyTheme(d->theme);

    setupUserIO();

    if (d->main_window != nullptr) {
        return;
    }

    if (d->splash.enabled) {
        // The frame comes first, so that the boot which follows - building the main window, loading the plugins - is
        // covered by something that reports what is happening instead of by a window still growing its ribbon.
        auto* boot = beginStartupProgress();
        boot->stage("正在初始化界面");

        d->boot_splash = new BootSplash(d->splash);

        // Created, not shown: run() shows the windows the boot puts in front of the user (see showUserInterface()) and
        // the loop paints them before the host's startup work may start. Showing here would drive the queue before the
        // loop exists - which itself would run the timers of everything that is starting up, the very thing the frame's
        // repaint() exists to avoid.
    }

    d->main_window = new MainWindow();

    // Embed the automatic progress bar into the main window's status bar: what the framework puts into the window is
    // painted by its first paint, and the paint is what run() waits for before the host's startup work may start.
    // Qt owns the native widget via the status bar; the presenter self-destructs with it (UIElement ownership model).
    if (auto* status = d->main_window->statusBar()->impl<QStatusBar>()) {
        auto* presenter = new ProgressPresenter();
        status->addPermanentWidget(static_cast<QWidget*>(presenter->impl()));
    }

    // Created, not shown: showUserInterface() is where the window goes up, once the whole boot has been built (this
    // window, its docks, its status bar) - and the loop paints it before the host's startup work, with it the plugin
    // loading, is allowed to start (see runStartup()).
}

void GuiApplication::finishStartup()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());

    if (d->boot_ended) {
        return; // Idempotent: the boot ends with the first call, whatever the frame does afterwards.
    }
    d->boot_ended = true;

    // Whether a frame is going away decides whether the window has to be brought forward below: the frame is a
    // stay-on-top window that was shown without activating the process, so the main window was shown underneath it and,
    // on Windows, never became the foreground window - without an explicit raise it stays under whatever was in front at
    // the time (the terminal the application was started from), which looks exactly like a window that never appeared.
    const bool frame_was_showing = d->boot_splash != nullptr;

    // The boot work is over here, and the startup progress ends with it: the sink goes back to following whatever runs
    // next, whether or not the frame is still up.
    Application::finishStartup();

    if (!frame_was_showing) {
        // Nothing to uncover: the window is what it is, and it only has to be up (a host that never showed it, or a
        // boot that ran without a frame, ends here).
        if (d->main_window != nullptr && !d->main_window->visible()) {
            d->main_window->show();
        }
        return;
    }

    // The frame goes now: the window is what can be uncovered, because a render view in it keeps its area hidden
    // until a frame is in the surface (see RenderControl) - what shows in the meantime is the window's own
    // background. There is nothing to wait for here.
    delete d->boot_splash;
    d->boot_splash = nullptr;

    if (d->main_window != nullptr) {
        if (!d->main_window->visible()) {
            d->main_window->show();
        }

        // Diagnostic: tells "the window sat behind something" (visible but not active) apart from "the window was
        // never shown", which are the two ways a frame that closes too early can look like a window that never
        // appeared.
        VN_LOGI("Startup frame going away: main window visible={}, active={}", d->main_window->visible(), d->main_window->isActive());

        if (auto* native = d->main_window->impl<QWidget>()) {
            native->raise();
        }
        d->main_window->activate();
    }
}

void GuiApplication::setSplashConfig(const SplashConfig& config)
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    if (d->app != nullptr) {
        // init() already ran, so whether a frame is shown has been decided; re-deciding it here would do nothing.
        VN_LOGW("GuiApplication::setSplashConfig() after init() is ignored: the startup frame is created during init()");
        return;
    }

    d->splash = config;
}

raw_ptr<BootSplash> GuiApplication::bootSplash() const
{
    return static_cast<const GuiApplicationData*>(dptr())->boot_splash;
}

void GuiApplication::setTheme(Theme theme)
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    if (d->theme == theme) {
        return;
    }
    d->theme = theme;
    applyTheme(theme);
    theme_changed.trigger(theme);
}

Theme GuiApplication::theme() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->theme;
}

void GuiApplication::setFollowSystemTheme(bool follow)
{
    auto* d = static_cast<GuiApplicationData*>(dptr());
    if (d->follow_system == follow) {
        return;
    }
    d->follow_system = follow;
    if (follow) {
        setTheme(resolveSystemTheme());
    }
}

bool GuiApplication::followSystemTheme() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->follow_system;
}

raw_ptr<MainWindow> GuiApplication::mainWindow() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->main_window;
}

void GuiApplication::applyTheme(Theme theme)
{
    const auto* d = static_cast<GuiApplicationData*>(dptr());
    if (d->app == nullptr) {
        return;
    }

    if (theme == Theme::Dark) {
        d->app->setPalette(createDarkPalette());
    }
    else {
        d->app->setPalette(createLightPalette());
    }
}

VN_APPFWGUI_NS_END
