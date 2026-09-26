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
 * right now ("正在加载插件 app_shell (2/3)") and the bar shows how far the current stage has come. The content comes
 * from StartupProgress, so the frame renders whatever the framework and the application report and needs no
 * application-side presentation code: it subscribes to ProgressHost::changed() and redraws on every notification.
 *
 * The frame is created and owned by GuiApplication when AppConfig::splash is enabled, and it stays on screen until the
 * the boot ends with Application::startupEnd() - it does not close itself, because that moment is the framework's to
 * is done.
 *
 * It is drawn on top of the main window rather than instead of it: the window is shown while the boot lasts, since an
 * embedded render surface creates its swapchain from the native window of the top-level widget and a window that was
 * never shown has none. The frame is a stay-on-top splash, so it covers that window while the boot is reported.
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
     * @brief Returns the fraction of the current counted stage shown by the bar.
     *
     * @return A value in [0, 1], or a negative value while the bar is shown as busy (isIndeterminate()).
     */
    double progressFraction() const;

    /**
     * @brief Returns whether the bar is shown as busy.
     *
     * @return true when the current stage reports no countable total.
     */
    bool isIndeterminate() const;

  private:
    /// Redraws the frame from the startup progress sink; application thread only.
    void refresh();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
