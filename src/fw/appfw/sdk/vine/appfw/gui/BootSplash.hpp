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
 * host calls Application::finishStartup() - it does not close itself, because only the host knows when its startup work
 * is done.
 *
 * It is drawn on top of the main window rather than instead of it: the window is shown while the boot lasts, since an
 * embedded render surface creates its swapchain from the native window of the top-level widget and a window that was
 * never shown has none. The frame is a stay-on-top splash, so it covers that window while the boot is reported.
 *
 * A boot runs on the application thread, which never returns to the event loop while it works, so a notification
 * repaints the frame right there instead of posting an update: the frame would otherwise be painted for the first
 * time only after the boot it reports on had already finished.
 *
 * That synchronous repaint is not by itself enough to put the frame on screen: painting a window waits for the window
 * system's "it is visible now" notice, which reaches the frame through the event queue (on X11, an expose event), and
 * a boot never dispatches that queue. Until it has been dispatched once the frame is an empty window on screen, so
 * the host dispatches it once right after showing the frame (see hasPainted()).
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

    /**
     * @brief Returns whether the frame has painted at least once.
     *
     * A shown frame is not a painted one: the first paint follows the window system's "it is visible now" notice,
     * which arrives through the event queue. Until that has been dispatched the frame is an empty window, which is
     * what it looks like when the boot never gets it on screen - measured under WSLg, where the frame stayed fully
     * transparent for its whole life while sixteen updates were reported.
     *
     * @return true once the frame has painted, false while it has not.
     */
    bool hasPainted() const noexcept;

  private:
    /// Redraws the frame from the startup progress sink; application thread only.
    void refresh();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
