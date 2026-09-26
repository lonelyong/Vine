#pragma once

#include <functional>

#include <vine/appfw/Application.hpp>

#include <vine/raw_ptr.hpp>
#include <vine/Signal.hpp>

VN_APPFWGUI_NS_BEGIN

class MainWindow;
class BootSplash;
class Window;

/**
 * @brief Application theme.
 */
enum class Theme
{
    Light, ///< Light theme.
    Dark   ///< Dark theme.
};

class VN_APPFW_API GuiApplication : public Application {
    VN_OBJECT_META_DECL
  public:
    /**
     * @brief Builds the GUI application from its configuration.
     *
     * Same initialization as Application - identity, managers, the configuration file - with a QApplication instead of
     * a QCoreApplication, and then the windows: the startup frame when AppConfig::splash enables it, and the main
     * window. Creation and presentation are two steps: the constructor builds what the application is made of and puts
     * nothing on screen, run() shows it (see startupStart()) once the boot has built everything. A host that never
     * runs the loop presents its window the same way a leaf does - by ending the startup phase from its own class (see
     * startupEnd()).
     *
     * @param config Application configuration.
     * @param argc Command line argument count.
     * @param argv Command line arguments.
     */
    GuiApplication(const AppConfig& config, int argc, char** argv);

    /**
     * @brief Builds the GUI application with a default configuration (a test, a tool): no startup frame.
     *
     * @param argc Command line argument count.
     * @param argv Command line arguments.
     */
    GuiApplication(int argc, char** argv);

    ~GuiApplication() override;

  public:
    /**
     * @brief Set and apply the theme immediately.
     *
     * In follow-system mode the next system theme change takes precedence.
     */
    void setTheme(Theme theme);

    /**
     * @brief Get the currently effective theme.
     */
    Theme theme() const;

    /**
     * @brief Enable or disable following the system theme.
     * @param follow True to follow the system theme.
     */
    void setFollowSystemTheme(bool follow);

    /**
     * @brief Whether the system theme is being followed.
     */
    bool followSystemTheme() const;

    /**
     * @brief Get the main window created during construction.
     *
     * The main window is created when the application is constructed and
     * remains valid for the whole application lifetime.
     */
    raw_ptr<MainWindow> mainWindow() const;

    /**
     * @brief Returns the startup frame, or nullptr when none is shown.
     *
     * Non-null from construction to startupEnd() when the configured frame is enabled; a host can use it to update the
     * frame's content itself, though the framework and the application normally report through StartupProgress.
     *
     * @return The startup frame, or nullptr.
     */
    raw_ptr<BootSplash> bootSplash() const;

  public:
    /**
     * @brief Emitted whenever the effective theme changes.
     *
     * Handlers are invoked on the GUI thread only; the parameter is the
     * newly effective theme.
     */
    Signal<Theme> theme_changed;

  protected:
    UserIO* createUserIO() override;

    /**
     * @brief Phase three of the boot: takes the startup frame away and shows the main window for the first time.
     *
     * This is where the main window appears: while the boot runs it was created but not shown (see startupStart()),
     * so the user's first sight of it is the finished window. Nothing inside it is left unfinished: a render view
     * attached during loading has its device and pipelines up but no frame yet, and the first frame lands a moment
     * later, in the event loop run() owns. That gap is the render view's own business, not the boot's - the view keeps
     * its native surface off screen until a frame is in it (see RenderControl), so the area it occupies shows the plain
     * widget background until the picture arrives, and showing the window can never reveal an empty native window.
     *
     * The frame, when there is one, is drawn on top of that window and stays until this phase: the boot ends when its
     * work is done - the framework's plugin load and the host's own startup work - and neither the frame nor the
     * framework can guess that moment. The startup progress has already been ended by the framework here (its sink is
     * destroyed just before this runs). The main window is raised and activated as the frame goes, because a
     * stay-on-top frame is shown without activating the process and the window would otherwise stay under whatever was
     * in front when it appeared.
     */
    vn::async::Task<void> startupEnd() override;

    /**
     * @brief Phase one of the boot: shows the startup frame the configuration asked for, and returns once it has painted.
     *
     * The frame is the only window this puts up: the main window is created by the constructor but shown by
     * startupEnd(), at the end of the boot - so what the user sees while the boot runs is one frame that reports,
     * never a window that is still growing its ribbon. What the old shape was protecting (the main window having a
     * native handle before the plugins load) does not need the window on screen: the render surface is embedded in a Qt
     * window container whose embedded QWindow has a native handle of its own (measured on X11/WSLg 2026-09-26: the
     * backend attached with the main window still hidden, and the first frame landed right after startupEnd()
     * showed it).
     *
     * A boot without a frame shows nothing, so it has nothing to wait for and returns at once. A frame is waited for
     * because showing a window only asks the window system to map it: the notice that it is on screen reaches it
     * through the event loop (see Window::hasPainted()), and dispatching that is what this phase suspends for - the loop
     * keeps turning, which is the whole point of the async shape. A deadline is the backstop for a window system that
     * never reports a window as visible: the boot moves on and says so.
     */
    vn::async::Task<void> startupStart() override;

  private:
    /// Creates what the application is made of: the startup frame when \a splash enables it, then the main window with
    /// the framework's progress bar in its status bar. Called by the constructor once the Qt application object and the
    /// user IO are up; nothing here is shown - the frame goes up in startupStart(), the window in startupEnd().
    ///
    /// @param splash Startup frame configuration; a disabled frame creates none.
    void createWindows(const SplashConfig& splash);

    void applyTheme(Theme theme);
};

VN_APPFWGUI_NS_END
