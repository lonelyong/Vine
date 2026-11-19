#include <vine/appfw/gui/GuiApplication.hpp>

#include <cassert>
#include <coroutine>
#include <memory>
#include <utility>

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

#include <vine/appfw/Translation.hpp>
#include <vine/appfw/gui/BootSplash.hpp>
#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/ProgressPresenter.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/appfw/gui/StatusBar.hpp>
#include <vine/appfw/gui/VisualUserIO.hpp>
#include <vine/appfw/gui/Window.hpp>

#include <vine/appfw/MainThreadDispatcher.hpp>
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

GuiApplicationData::~GuiApplicationData() = default;

namespace
{

/// Upper bound for the wait for a boot window's first paint; a window system that never shows a window must not keep
/// the host's startup work from running.
constexpr int kFirstPaintDeadlineMs = 300;

/// The window a boot puts on screen: the startup frame, when the configuration enables one. The main window is NOT
/// among them - it is shown by startupEnd(), so that the user never sees a window that is still growing its ribbon.
///
/// @param d Application data.
/// @return The startup frame, or nullptr when the boot shows nothing.
Window* startupWindow(GuiApplicationData* d)
{
    return static_cast<Window*>(d->boot_splash.get());
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

/**
 * @brief Waits for a window's first paint, or for the boot's deadline.
 *
 * Three sources end the wait - the first paint, the deadline, and "it had already painted before the wait armed" - and
 * all of them go through wake(), which fires once: the two live sources are dropped there, and the members that own
 * them say so. The resumption is posted rather than run inline, because the first paint signal arrives while that
 * window's paint event is being dispatched, and resuming there would run the rest of the boot inside somebody else's
 * paint - which repaints this frame and finally destroys it (measured: `QWidget::repaint: Recursive repaint detected`
 * followed by a segfault).
 *
 * Missing the deadline is not a failure: a window system may never report a window as visible, and the caller tells the
 * two cases apart with Window::hasPainted() and says so in the log.
 */
class AwaitStartupFrame {
  public:
    /**
     * @brief Arms a wait for @p frame's first paint.
     *
     * @param context Qt object the wait connects through; this class is not a QObject.
     * @param frame   The frame to wait for.
     */
    AwaitStartupFrame(QObject* context, Window* frame) noexcept
      : context_(context)
      , frame_(frame)
    {}

    /// Nothing to wait for when the window system painted it already (some platforms paint from show()).
    [[nodiscard]] bool await_ready() const noexcept
    {
        return frame_->hasPainted();
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
        handle_       = handle;
        subscription_ = frame_->first_paint.connect([this] { wake(); });

        // Not parented to the context: this object owns the timer, and a parent that died first would leave the
        // owner deleting a timer Qt had already destroyed. The connection names the context instead, so it dies with it.
        timer_ = std::make_unique<QTimer>();
        timer_->setSingleShot(true);
        QObject::connect(timer_.get(), &QTimer::timeout, context_, [this] { wake(); });
        timer_->start(kFirstPaintDeadlineMs);
    }

    void await_resume() const noexcept
    {}

  private:
    /// The one way out: whichever source arrives first ends the wait, and only here.
    void wake()
    {
        const std::coroutine_handle<> handle = std::exchange(handle_, {});
        if (handle == nullptr) {
            return;
        }

        subscription_ = {};
        timer_.reset();

        // Posted, not resumed here: see the class note.
        static_cast<void>(MainThreadDispatcher::postToMainThread([handle] { handle.resume(); }));
    }

    QObject*                context_ = nullptr;
    Window*                 frame_   = nullptr;
    std::unique_ptr<QTimer> timer_;
    vn::Connection          subscription_{};
    std::coroutine_handle<> handle_{};
};

} // namespace

vn::async::Task<void> GuiApplication::startupStart()
{
    auto*         d     = static_cast<GuiApplicationData*>(dptr());
    Window* const frame = startupWindow(d);

    // A boot without a frame shows nothing, so there is no window that could sit there empty.
    if (frame == nullptr) {
        co_return;
    }

    // Only the frame goes up while the boot runs. What the old shape was protecting - the main window having a native
    // handle before the plugins load - does not need the window to be on screen: the render surface is embedded in a Qt
    // window container, and the embedded QWindow gets its own native handle whether or not the top-level was ever shown
    // (measured on X11/WSLg 2026-09-26: the backend attached 1707 ms into the boot with the main window still hidden, and
    // the first frame landed right after startupEnd() showed it). Keeping the main window off screen is what the boot
    // looks like: one frame that reports, and no window that grows a ribbon in front of the user.
    frame->show();

    // Showing the frame is a phase of its own, and this phase waits for its first paint (the `co_await` below), so the
    // stage name goes up first: progress jumps per phase, and every stretch the reader sees should say what is
    // happening now rather than carry the previous phase's name.
    if (StartupProgress* progress = StartupProgress::current(); progress != nullptr) {
        progress->stage(vn::appfw::translate("appfw", "Showing the startup frame"));
    }

    // What is waited for is that frame's first paint: showing a window only asks the window system to map it, and the
    // notification that it is on screen is dispatched by the very loop this phase runs in - which is why the wait is a
    // suspension (`co_await`) and not a block: the loop has to keep turning for that notice to arrive at all. The
    // deadline inside the awaitable is the backstop for a window system that never reports a window as visible.
    co_await AwaitStartupFrame{ static_cast<QObject*>(d->app.get()), frame };

    // Whatever happened, this line is the evidence that the boot reached the point where its work may start, and it is
    // the one the design documents quote (`hasPainted()` tells the two cases apart).
    if (frame->hasPainted()) {
        VN_LOGI("startup work starting {} ms after the application was asked to run: the startup frame is on screen", d->startup_requested_at.elapsed());
    }
    else {
        VN_LOGW("the startup phase is moving on {} ms after the application was asked to run without the startup frame "
                "having painted within {} ms: it stays an empty window until the window system maps it",
                d->startup_requested_at.elapsed(),
                kFirstPaintDeadlineMs);
    }
}

GuiApplication::GuiApplication(const AppConfig& config, int argc, char** argv)
  : Application(new GuiApplicationData(), config, argc, argv)
{
#if defined(Q_OS_LINUX)
    selectX11UnderWslg();
#endif

    // A QApplication, not the QCoreApplication the headless constructor makes: the platform plugin is chosen and the
    // window system comes up here, and the process may hold only one Qt application object.
    //
    // QCoreApplication keeps a reference to argc; the data it refers to must stay valid for the whole application
    // lifetime, so pass the stored member instead of a local copy.
    dptr()->app = std::make_unique<QApplication>(dptr()->argc, dptr()->argv);
    initialize(config);

    createWindows(config.splash);
}

GuiApplication::GuiApplication(int argc, char** argv)
  : GuiApplication(AppConfig{}, argc, argv)
{}

GuiApplication::~GuiApplication()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());

    // The widgets belong to the data block and die with it, right after this destructor (in reverse declaration order:
    // the frame, then the document host, then the registry, then the main window the host hands its slot back to).
    // What is left to say is the boot that never reached its last phase: the frame is still up and the main window it
    // was hiding was never shown. Two ways there: a host that drives its own boot and never calls startupEnd(), and a
    // boot the framework cancelled or failed - which leaves the frame up on purpose (see Application::cancelStartup()),
    // so the line states the fact and does not blame the host.
    if (d->boot_splash != nullptr && !d->boot_ended) {
        VN_LOGW("Startup frame is still showing as the application is destroyed: the boot never reached its last "
                "phase (Application::startupEnd())");
    }
}

UserIO* GuiApplication::createUserIO()
{
    return new VisualUserIO;
}

void GuiApplication::createWindows(const SplashConfig& splash)
{
    auto* d      = static_cast<GuiApplicationData*>(dptr());
    auto* qt_app = static_cast<QApplication*>(d->app.get());

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // When following the system, listen for system theme changes and re-resolve.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qt_app, [this, d](Qt::ColorScheme) {
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

    if (splash.enabled) {
        // The frame comes first, so that the boot which follows - building the main window, loading the plugins - is
        // covered by something that reports what is happening instead of by a window still growing its ribbon.
        // It is the only window the boot shows (see startupStart()) and only a view of it: the startup progress
        // sink belongs to the framework (the startup phase creates it) and the frame follows it through ProgressHost,
        // so nothing is reported from here.
        d->boot_splash = std::make_unique<BootSplash>(splash);

        // Created, not shown: the startup phase shows the frame (see startupStart()) and waits for its first paint
        // before the plugins load. Showing it here would drive the queue before the loop exists - which itself would run
        // the timers of everything that is starting up, the very thing the frame's repaint() exists to avoid.
    }

    d->main_window = std::make_unique<MainWindow>();

    // Embed the automatic progress bar into the main window's status bar: the framework's startup progress follows that
    // presenter too, so what the boot reports stays visible for as long as the window is up.
    // Qt owns the native widget via the status bar; the presenter self-destructs with it (UIElement ownership model).
    if (auto* status = d->main_window->statusBar()->impl<QStatusBar>()) {
        auto* presenter = new ProgressPresenter();
        status->addPermanentWidget(static_cast<QWidget*>(presenter->impl()));
    }

    // The view layer. GUI-only by construction: a view is a UIElement, so a headless Application has none, and the
    // central host below is what makes "the current document is what the middle of the window shows" the framework's
    // default instead of every host's own three lines. The host installs its container lazily (see its class note), so
    // a shell that put its own content in the central area keeps it until the first document is presented.
    d->view_registry = std::make_unique<DocumentViewRegistry>();
    if (auto* documents = documentManager(); documents != nullptr) {
        if (auto* docks = d->main_window->dockPanelManager(); docks != nullptr) {
            d->central_host = std::make_unique<CentralDocumentHost>(*documents, *d->view_registry, *docks);
        }
    }

    // Created, not shown: the main window goes up in startupEnd(), at the end of the boot, once the whole boot has
    // been built (this window, its docks, its status bar) - so the user never sees a window that is still assembling
    // itself. Until then the render view inside it keeps its native surface off screen, so the area it occupies is the
    // window's own background rather than a hole.
}

vn::async::Task<void> GuiApplication::startupEnd()
{
    auto* d = static_cast<GuiApplicationData*>(dptr());

    if (d->boot_ended) {
        co_return; // Idempotent: the boot ends with the first call, whatever the frame does afterwards.
    }
    d->boot_ended = true;

    // The last phase reports as well: the main window going up and the frame going away both happen here, and the
    // reporting door closes right after this phase returns (see Application::startupSequence()).
    if (StartupProgress* progress = StartupProgress::current(); progress != nullptr) {
        progress->stage(vn::appfw::translate("appfw", "Preparing the main window"));
    }

    // The startup progress is still alive here (it belongs to the whole boot, this last phase included), so what is
    // done here is the boot's own face: the main window goes up and the frame goes away.
    //
    // The window goes up now, and this is the only place it does: createWindows() always built it and the boot kept it
    // off screen, so the user's first sight of it is the finished window. What the render area shows until its first
    // frame is the window's own background, not a hole (see RenderControl).
    auto* const window = d->main_window.get();
    if (!window->isVisible()) {
        window->show();
    }

    if (d->boot_splash == nullptr) {
        co_return; // Nothing was covering the boot: nothing to take away, nothing to bring forward.
    }

    // The frame goes now that what it covered is up. The window has to be brought forward with it: a stay-on-top frame
    // is shown without activating the process, so on Windows the window never became the foreground window and, without
    // an explicit raise, stays under whatever was in front when the frame appeared (the terminal the application was
    // started from) - which looks exactly like a window that never showed up.
    d->boot_splash.reset();

    // Diagnostic: tells "the window sat behind something" (visible but not active) apart from "the window was never
    // shown", the two ways a frame that closes too early can look like a window that never appeared.
    VN_LOGI("Startup frame going away: main window visible={}, active={}", window->isVisible(), window->isActive());

    if (auto* native = window->impl<QWidget>()) {
        native->raise();
    }
    window->activate();
}

raw_ptr<BootSplash> GuiApplication::bootSplash() const
{
    return static_cast<const GuiApplicationData*>(dptr())->boot_splash.get();
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

raw_ptr<DocumentViewRegistry> GuiApplication::viewRegistry() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->view_registry.get();
}

raw_ptr<CentralDocumentHost> GuiApplication::centralDocumentHost() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->central_host.get();
}

bool GuiApplication::followSystemTheme() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->follow_system;
}

raw_ptr<MainWindow> GuiApplication::mainWindow() const
{
    const auto* d = static_cast<const GuiApplicationData*>(dptr());
    return d->main_window.get();
}

void GuiApplication::applyTheme(Theme theme)
{
    // The Qt application object is owned by the base's application data and is the last member standing there, so it is
    // alive for as long as this call can happen: applyTheme() runs from the constructor and from a theme change, both of
    // which need a live application.
    auto* qt_app = static_cast<QApplication*>(static_cast<const GuiApplicationData*>(dptr())->app.get());

    if (theme == Theme::Dark) {
        qt_app->setPalette(createDarkPalette());
    }
    else {
        qt_app->setPalette(createLightPalette());
    }
}

VN_APPFWGUI_NS_END
