#include <vine/appfw/gui/GuiApplication.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPalette>
#include <QStatusBar>
#include <QStyleHints>
#include <QThread>
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

/// Upper bound for the startup frame's first paint; a window system that never shows the frame must not hold the boot.
constexpr int kFirstPaintDeadlineMs = 300;

/// Pace of the dispatch loop below; a window paints within milliseconds on a display that shows windows promptly.
constexpr int kDispatchPollMs = 5;

/// Reports whether the widget it watches has painted anything at all.
///
/// A window that is up but unpainted is a black rectangle on screen until the event loop runs (measured under WSLg:
/// the main window stayed 0% painted for the whole boot), so this is the signal that it has stopped being one.
class FirstPaintSpy : public QObject
{
  public:
    explicit FirstPaintSpy(QObject* parent = nullptr)
      : QObject(parent)
    {}

    /**
     * @brief Returns whether a paint event has been seen.
     *
     * @return true once any part of the watched window has painted.
     */
    bool painted() const noexcept
    {
        return painted_;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Paint) {
            painted_ = true;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    bool painted_{ false };
};

/// Makes a filter see the paint of a window and of everything inside it.
///
/// A window paints its own area and hands every child its own paint event, and which of the two comes first is the
/// layout's business, so watching the top level alone would miss the first paint whenever a child covers it.
///
/// @param window Window whose paints are of interest.
/// @param spy Filter to install on the window and its children.
void watchPaints(QWidget& window, QObject& spy)
{
    window.installEventFilter(&spy);
    for (QWidget* child : window.findChildren<QWidget*>()) {
        child->installEventFilter(&spy);
    }
}

/**
 * Dispatches the event queue until a window has painted, or until the deadline passes.
 *
 * Painting a window waits for the window system's "it is visible now" notice, which arrives through the event queue
 * (on X11, an expose event). A synchronous repaint() paints the widget, but nothing of it reaches the window before
 * that notice has been dispatched - measured under WSLg, where the startup frame spent its whole boot as a fully
 * transparent window while sixteen updates were reported to it, and where the main window was a black rectangle for
 * just as long.
 *
 * The queue is dispatched at the two moments where it is safe: right after the startup frame is shown, and right after
 * the main window is shown - both before any plugin has loaded, so no render surface exists yet and this cannot wake a
 * component that is still starting up. That is also why neither window pumps for itself (see BootSplash).
 *
 * @param painted Predicate that becomes true once the window has painted.
 * @param subject Window being waited for, named in the diagnostics.
 * @return true if the window painted before the deadline, false otherwise.
 */
template <typename Painted>
bool dispatchUntilPainted(Painted painted, const char* subject)
{
    QElapsedTimer timer;
    timer.start();

    while (!painted()) {
        if (timer.elapsed() >= kFirstPaintDeadlineMs) {
            VN_LOGW("{} was shown but has not painted within {} ms: it stays an empty window until the event loop runs",
                    subject,
                    kFirstPaintDeadlineMs);
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QThread::msleep(kDispatchPollMs);
    }

    VN_LOGI("{} painted after {} ms", subject, timer.elapsed());
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
        d->boot_splash->show();

        // The frame is only on screen once it has painted, and that waits for the window system's notice on the event
        // queue (see dispatchUntilPainted): give it that, so the boot is covered by a picture rather than by an empty
        // window. Nothing else is alive yet, so dispatching here is free of the hazard the frame's own comments warn
        // about; the deadline keeps a window system that never shows the frame from holding the boot.
        dispatchUntilPainted([d] { return d->boot_splash->hasPainted(); }, "startup frame");
    }

    d->main_window = new MainWindow();

    // The window is shown while the frame reports the boot, not after it: an embedded render surface (RenderControl, the
    // VSG backend) creates its swapchain from the native window of the top-level widget, and a window that was never
    // shown has none - the surface then fails to initialize instead of waiting for the window. The frame is a
    // stay-on-top splash, so it covers the window while the boot lasts.
    d->main_window->show();

    // Embed the automatic progress bar into the main window's status bar.
    // Qt owns the native widget via the status bar; the presenter
    // self-destructs with it (UIElement ownership model).
    if (auto* status = d->main_window->statusBar()->impl<QStatusBar>()) {
        auto* presenter = new ProgressPresenter();
        status->addPermanentWidget(static_cast<QWidget*>(presenter->impl()));
    }

    if (auto* window = d->main_window->impl<QWidget>()) {
        // The window is on screen from here on, and an unpainted window is a black rectangle: measured under WSLg, it
        // stayed unpainted for the whole boot and the user saw exactly that. Give it the dispatch its first paint
        // waits for, after everything the framework itself puts into it is there. Same reasoning and same safe moment
        // as the frame above: no plugin has loaded yet, so no render surface exists.
        FirstPaintSpy spy;
        watchPaints(*window, spy);
        dispatchUntilPainted([&spy] { return spy.painted(); }, "main window");
    }
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

int GuiApplication::run()
{
    const auto* d = static_cast<GuiApplicationData*>(dptr());

    if (d->boot_splash != nullptr && !d->boot_ended) {
        // The host never ended the startup phase: the frame keeps covering a main window that is still hidden, so the
        // user has nothing to close and the application would sit in its main loop forever. The window is not shown here
        // on purpose - the host asked for an explicit end of the startup phase - but it must not go unsaid. (A frame
        // that outlives finishStartup() is not this: it is waiting for the window, and closes itself in this loop.)
        VN_LOGW("Startup frame is still showing and the main window is still hidden: the host must call "
               "Application::finishStartup() before run()");
    }

    const int code = d->app->exec();
    // Same shutdown sequence as Application::run(): plugins unload, the bus
    // delivers what is still parked (bounded) and stops, and the configuration
    // is persisted - all before the UI is torn down.
    shutdown();
    return code;
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
