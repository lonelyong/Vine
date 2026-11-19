#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "../ApplicationData.hpp"

#include <vine/Signal.hpp>

#include <vine/appfw/AppConfig.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

VN_APPFWGUI_NS_BEGIN

class BootSplash;
class MainWindow;
class DocumentViewRegistry;
class CentralDocumentHost;

struct GuiApplicationData : public ApplicationData {
    GuiApplicationData()  = default;
    /// Defined in GuiApplication.cpp: it destroys the widgets below, whose types this header only forward-declares.
    ~GuiApplicationData() override;

    /// The main window: created by the constructor, shown by startupEnd(). Declared FIRST so it is destroyed LAST of
    /// the widgets below - the document host hands the central slot back through this window's dock manager.
    std::unique_ptr<MainWindow> main_window;

    /// Which view presents which document type. GUI-only: a view is a UIElement, so a headless run has no views at all.
    std::unique_ptr<DocumentViewRegistry> view_registry;

    /// Shows the current document in the central client area (see CentralDocumentHost). Created with the registry.
    std::unique_ptr<CentralDocumentHost> central_host;

    /// The startup frame, or nullptr when none is shown (disabled, or the boot has ended). Declared LAST so it dies
    /// first: it is the one window the boot puts up, and what it covered has to be gone before it is taken down.
    std::unique_ptr<BootSplash> boot_splash;

    /// Whether the boot has ended (startupEnd() ran). A frame that is still up after that is one the host never
    /// called for - not a host that forgot the call, which is what the warnings key on.
    bool boot_ended = false;

    Theme theme         = Theme::Light; // currently active theme
    bool  follow_system = true;         // whether to follow the system theme
};

VN_APPFWGUI_NS_END
