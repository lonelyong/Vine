#include <vine/appfw/gui/GuiAppBuilder.hpp>

#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

#include "AppBuilderSupport.hpp"

V_APPFWGUI_NS_BEGIN

std::unique_ptr<GuiApplication> createGuiApplication(const AppConfig& config, int argc, char** argv)
{
    if (!config.built_in_plugin_dir.empty()) {
        PluginManager::setBuiltInPluginDirectory(config.built_in_plugin_dir);
    }

    auto app = std::make_unique<GuiApplication>(argc, argv);
    app->init();
    applyAppConfig(*app, config);

    return app;
}

V_APPFWGUI_NS_END
