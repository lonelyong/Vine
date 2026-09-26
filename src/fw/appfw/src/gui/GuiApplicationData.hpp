#pragma once

#include <functional>
#include <vector>

#include "../ApplicationData.hpp"

#include <vine/Signal.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

class QApplication;

VN_APPFWGUI_NS_BEGIN

class BootSplash;
class MainWindow;

struct GuiApplicationData : public ApplicationData {
    QApplication* app         = nullptr;
    MainWindow*   main_window = nullptr;

    /// Startup frame configuration; set before init() decides whether to create the frame.
    SplashConfig splash;

    /// The startup frame, or nullptr when none is shown (disabled, or startup already finished).
    BootSplash* boot_splash = nullptr;

    /// Whether the boot has ended (finishStartup() ran). A frame that is still up after that is one the host never
    /// called for - not a host that forgot the call, which is what the warnings key on.
    bool boot_ended = false;

    /// Pending callback of whenUserInterfaceIsUp(): what the framework runs once the boot's windows are up. Empty once
    /// it has been called - every source that can arrive calls the same place, and the first one takes it here.
    std::function<void()> when_up;

    /// Subscriptions to the first paint of the boot windows (see GuiApplication::whenUserInterfaceIsUp()): one per window
    /// that has not painted yet, dropped again once when_up runs.
    std::vector<vn::Connection> startup_gates;

    Theme theme         = Theme::Light; // currently active theme
    bool  follow_system = true;         // whether to follow the system theme
};

VN_APPFWGUI_NS_END
