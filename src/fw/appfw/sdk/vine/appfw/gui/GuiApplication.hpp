#pragma once

#include <vine/appfw/Application.hpp>

#include <vine/raw_ptr.hpp>
#include <vine/Signal.hpp>

V_APPFW_NS_BEGIN
struct SplashConfig;
V_APPFW_NS_END

V_APPFWGUI_NS_BEGIN

class ConsolePanel;
class MainWindow;
class BootSplash;

/**
 * @brief Application theme.
 */
enum class Theme
{
    Light, ///< Light theme.
    Dark   ///< Dark theme.
};

class V_APPFW_API GuiApplication : public Application {
    V_OBJECT_META_DECL
  public:
    GuiApplication(int argc, char** argv);
    ~GuiApplication() override;

  public:
    virtual void init() override;

  public:
    virtual int run() override;

    /**
     * @brief Ends the startup phase: finishes the startup progress and takes the startup frame away.
     *
     * The frame is drawn on top of the main window, which is already shown while the boot runs, and it stays there until
     * this call: only the host knows when its startup work is done, so the frame neither closes itself nor guesses a
     * timeout. The startup progress reported through StartupProgress ends here too - its sink is destroyed, so the
     * progress presenters go back to following whatever runs next. The main window is raised and activated as the frame
     * goes, because a stay-on-top frame is shown without activating the process and the window would otherwise stay
     * under whatever was in front when it appeared.
     *
     * The host calls it once its startup work is done, and that includes the work plugins do in load(). What may not be
     * done is the window's ability to SHOW something: a render view attached during loading is up but still empty, and
     * the frame a native surface gets the size of its window from the window system, which needs the event loop - so the
     * first frame lands a moment after the boot work ends (~0.7-0.9 s measured, the device/swapchain/pipeline build
     * included). The frame is what hides exactly that, so when the window is not ready to be uncovered the close is
     * DEFERRED to the render view's own report and happens inside the event loop that produced it (run()'s), bounded by
     * a deadline. An application without a render view, or with one that has already drawn, takes the immediate path it
     * always did.
     *
     * The host does not observe the difference: `finishStartup()` still means "the boot work is over", and a frame that
     * outlives the call is the window still coming up, not a host that forgot it.
     */
    void finishStartup() override;

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
     * @brief Get the main window created during initialization.
     *
     * The main window is created and shown when the application initializes
     * and remains valid for the whole application lifetime.
     */
    raw_ptr<MainWindow> mainWindow() const;

    /**
     * @brief Sets the startup frame configuration.
     *
     * Effective before init(): that is where the frame is created. The application builders do the part the framework
     * cannot know (an empty title falls back to AppConfig::name), so a host that builds the application itself only has
     * to call this.
     *
     * @param config Appearance of the frame; AppConfig::splash.
     */
    void setSplashConfig(const SplashConfig& config);

    /**
     * @brief Returns the startup frame, or nullptr when none is shown.
     *
     * Non-null between init() and finishStartup() when the configured frame is enabled; a host can use it to update the
     * frame's content itself, though the framework and the application normally report through StartupProgress.
     *
     * @return The startup frame, or nullptr.
     */
    raw_ptr<BootSplash> bootSplash() const;

    /**
     * @brief Attaches the console panel used by the visual user I/O.
     *
     * The GUI application's UserIO routes its output and input prompts to
     * this panel. The caller (typically a plugin) creates and docks the
     * panel and then hands it over here.
     */
    void setConsolePanel(ConsolePanel* console);

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

  private:
    void applyTheme(Theme theme);

    /** @brief Whether the window has something to show: its render view has drawn a frame, or it has none. */
    bool windowCanBeSeen() const;

    /** @brief Closes the startup frame and brings the window it covered forward. Idempotent. */
    void closeStartupFrame();

    /** @brief Arms the one-shot close that follows the render view's own report (see finishStartup()). */
    void deferStartupFrameClose();
};

V_APPFWGUI_NS_END
