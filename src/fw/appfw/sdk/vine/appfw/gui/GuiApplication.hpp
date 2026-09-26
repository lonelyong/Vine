#pragma once

#include <string_view>

#include <vine/appfw/Application.hpp>

#include <vine/raw_ptr.hpp>
#include <vine/Signal.hpp>

VN_APPFW_NS_BEGIN
struct SplashConfig;
VN_APPFW_NS_END

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
     * The host calls it once its startup work is done, and that includes the work plugins do in load(). What it may
     * leave undone is the window's ability to show something: a render view attached during loading has its device and
     * pipelines up but no frame yet, and the first frame lands a moment later, in the event loop run() owns. That gap
     * is the render view's own business, not the frame's - the view keeps its native surface off screen until a frame
     * is in it (see RenderControl), so the area it occupies shows the plain widget background until the picture
     * arrives, and uncovering the window here can never reveal an empty native window.
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

    /**
     * @brief Shows a window and waits for it to paint.
     *
     * For the windows a boot puts on screen before run() owns the event loop: painting a window waits for the window
     * system's "it is visible now" notice, which reaches it through the event queue, so a window shown without driving
     * that queue stays empty on screen (black, or transparent when it is translucent) until the loop starts. The wait
     * is bounded, and both outcomes are reported - see the design notes for the measurements.
     *
     * Driving the queue runs the timers of everything that is starting up, so it is only done while no render surface
     * exists yet: that is asserted inside, and it is the reason this is not a general purpose "show" (a plugin showing
     * a window during a boot must not come through here).
     *
     * @param window  Window to show; it has not painted yet.
     * @param subject Window name for the diagnostics.
     */
    void showAndWaitForFirstPaint(Window& window, std::string_view subject);
};

VN_APPFWGUI_NS_END
