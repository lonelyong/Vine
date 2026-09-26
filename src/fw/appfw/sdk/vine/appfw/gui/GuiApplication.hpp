#pragma once

#include <functional>

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
    /**
     * @brief Creates the application's windows: the startup frame (when enabled), then the main window.
     *
     * Creation and presentation are two steps: this method builds what the application is made of and puts nothing on
     * screen, run() shows it (see showUserInterface()) once the boot has built everything. A host that never runs the
     * loop presents its window the same way any host does - by ending the startup phase (see finishStartup()).
     */
    virtual void init() override;

  public:
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
     *
     * runStartup() calls it once the work it was handed returns, so a host that hands its startup work over does not
     * have to; one that wants to end the boot at a different moment still calls it itself.
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

    /**
     * @brief Shows the windows the boot put in front of the user: the startup frame, then the main window.
     *
     * Both are created by init() and shown here, and the order between them is load-bearing: the frame is what covers
     * the boot, so it goes up first, and the main window goes up next - not after the boot - because an embedded
     * render surface (RenderControl, the VSG backend) creates its swapchain from the native window of the top-level
     * widget, and a window that was never shown has none (the surface then fails to initialize instead of waiting for
     * the window). The loop paints both before the host's startup work - with it the plugin loading - is allowed to
     * start, which is what keeps them from being empty windows while the work blocks the thread.
     */
    void showUserInterface() override;

    /**
     * @brief Calls \a then once both boot windows have painted, at the latest once the first-paint deadline passes.
     *
     * A window is painted only once the event loop drives its queue (see Window::hasPainted()), so what this waits for
     * are the windows' first paints - the frame that covers the boot and the main window around it, because either of
     * them left empty is exactly what this hand-off exists to avoid. The deadline is the backstop for a window system
     * that never reports a window as visible: the framework moves on and says so.
     *
     * @param then What run() runs once the user interface is up; called exactly once, from the next event-loop turn.
     */
    void whenUserInterfaceIsUp(std::function<void()> then) override;

  private:
    void applyTheme(Theme theme);

    /// Calls the pending then of whenUserInterfaceIsUp() once no boot window is left unpainted, from the next event-loop
    /// turn: the first paint arrives while that window's paint event is being dispatched, and the host's startup work
    /// repaints the frame and finally takes it away - neither belongs inside a window's paint.
    ///
    /// @param deadline Whether the call comes from the first-paint backstop; a window still unpainted is then reported
    ///                 and the host's work starts anyway instead of being waited for.
    void startWhenUp(bool deadline);
};

VN_APPFWGUI_NS_END
