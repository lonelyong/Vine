#pragma once

#include "Window.hpp"

V_APPFW_NS_BEGIN
struct SplashConfig;
V_APPFW_NS_END

V_APPFWGUI_NS_BEGIN

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
 * repaints and pumps the event queue right away (user input excluded); without that the frame would be painted for the
 * first time after the boot it is reporting on had already finished.
 */
class V_APPFW_API BootSplash : public Window {
    V_OBJECT_META_DECL

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

V_APPFWGUI_NS_END
