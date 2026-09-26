#pragma once

#include "Window.hpp"

VN_APPFW_NS_BEGIN
struct SplashConfig;
VN_APPFW_NS_END

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Startup frame: the frameless window shown while an application boots.
 *
 * Draws the application's identity (logo, title, subtitle) and the boot itself: the status line says what is happening
 * right now ("正在加载插件 app_shell (2/3)") next to a small indicator that keeps turning. There is no bar and no
 * percentage: a boot has no known total (see StartupProgress), so the frame reports *what* is happening and *that* it is
 * happening, and nothing about how far along it is. The content comes from StartupProgress, so the frame renders
 * whatever the framework and the application report and needs no application-side presentation code: it subscribes to
 * ProgressHost::changed() and redraws on every notification.
 *
 * The frame is created and owned by GuiApplication when AppConfig::splash is enabled, and it is destroyed by the boot's
 * last phase (Application::startupEnd()) - it does not close itself, because taking the boot's face down is the
 * framework's move (a close request from the window system is ignored; see the frame's closeEvent).
 *
 * It is the only window up while the boot runs: the main window is built by the constructor but shown by startupEnd(),
 * which is also what takes the frame down - so what the user sees while the boot runs is one frame that reports, never
 * a window that is still growing its ribbon. Nothing in that window is half-built when it appears: a render view
 * attached during loading keeps its native surface off screen until a frame is in it (see RenderControl), so the area
 * it occupies shows the window's own background rather than a hole.
 *
 * A boot holds the application thread in stretches - the session attach, the warm-up frame and the content load all
 * run on the pool, but what builds widgets and graphics objects on the application thread cannot - and every report is
 * made from inside one of those stretches or at a phase boundary between them. So a notification repaints the frame
 * right there instead of posting an update: posting would show it only once the loop came back, and pumping the queue
 * to force it is what this codebase refuses (it would run the timers of everything else that is starting up).
 *
 * That synchronous repaint is not by itself enough to put the frame on screen: the very first paint waits for the
 * window system's "it is visible now" notice, which reaches the frame through the event queue (on X11, an expose
 * event). That notice arrives because the boot suspends while it waits for it - see GuiApplication::startupStart() and
 * AwaitStartupFrame - and until it has been dispatched once the frame is an empty window on screen (hasPainted(),
 * inherited from Window, is what tells the two apart).
 *
 * Closing is not offered: there is no cancel affordance on the frame, and a close request from the window system is
 * ignored (see the frame's closeEvent). The boot's face is the framework's to take down, and a frame the user dismissed
 * while the load goes on would leave the screen empty until the main window comes up.
 */
class VN_APPFW_API BootSplash : public Window {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Builds the frame from its configuration.
     *
     * @param config Appearance of the frame; title/subtitle/logo may be empty and the progress display follows the
     *        startup progress sink.
     */
    explicit BootSplash(const SplashConfig& config);

    ~BootSplash() override;

  public:
    /**
     * @brief Returns the status line as it was last reported, before any elision.
     *
     * @return The status text, or an empty string when the boot has reported nothing yet.
     */
    String statusText() const;

    /**
     * @brief Reports whether the frame's busy indicator is turning.
     *
     * The frame shows no percentage and no bar: how far a boot has come is not knowable (the stages that have not started
     * yet have no known length - see StartupProgress), so a bar could only lie. What it shows instead is motion - a small
     * indicator that keeps turning, next to the line that says what is happening - and this is the state of that motion.
     *
     * @return true while the indicator is running.
     */
    bool isBusyIndicatorRunning() const;

  private:
    /// Redraws the frame from the startup progress sink; application thread only.
    void refresh();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
