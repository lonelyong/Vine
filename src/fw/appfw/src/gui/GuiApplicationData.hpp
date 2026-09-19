#pragma once

#include "../ApplicationData.hpp"

#include <QTimer>

#include <vine/Signal.hpp>
#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

class QApplication;

V_APPFWGUI_NS_BEGIN

class BootSplash;
class MainWindow;

struct GuiApplicationData : public ApplicationData {
    QApplication* app         = nullptr;
    MainWindow*   main_window = nullptr;

    /// Startup frame configuration; set before init() decides whether to create the frame.
    SplashConfig splash;

    /// The startup frame, or nullptr when none is shown (disabled, or startup already finished).
    BootSplash* boot_splash = nullptr;

    /// Whether the boot has ended (finishStartup() ran). A frame that is still up after that is waiting for the
    /// window to be able to show something - not a host that forgot the call, which is what the warnings key on.
    bool boot_ended = false;

    /// Closes the startup frame when the render view reports in; armed only while the frame waits for it.
    Signal<RenderControl::SurfaceState>::Subscription frame_close_subscription{};

    /// Deadline of that wait, so a surface that never speaks cannot keep the frame up forever.
    QTimer frame_close_deadline;

    Theme theme         = Theme::Light; // currently active theme
    bool  follow_system = true;         // whether to follow the system theme
};

V_APPFWGUI_NS_END
