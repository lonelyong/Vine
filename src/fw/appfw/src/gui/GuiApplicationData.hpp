#pragma once

#include <functional>
#include <vector>

#include "../ApplicationData.hpp"

#include <vine/Signal.hpp>

#include <vine/appfw/AppConfig.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

VN_APPFWGUI_NS_BEGIN

class BootSplash;
class MainWindow;

struct GuiApplicationData : public ApplicationData {
    MainWindow* main_window = nullptr;

    /// The startup frame, or nullptr when none is shown (disabled, or startup already finished).
    BootSplash* boot_splash = nullptr;

    /// Whether the boot has ended (startupEnd() ran). A frame that is still up after that is one the host never
    /// called for - not a host that forgot the call, which is what the warnings key on.
    bool boot_ended = false;

    Theme theme         = Theme::Light; // currently active theme
    bool  follow_system = true;         // whether to follow the system theme
};

VN_APPFWGUI_NS_END
