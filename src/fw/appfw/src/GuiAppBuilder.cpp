#include <vine/appfw/gui/GuiAppBuilder.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

#include "AppBuilderSupport.hpp"

VN_APPFWGUI_NS_BEGIN

std::unique_ptr<GuiApplication> createGuiApplication(const AppConfig& config, int argc, char** argv)
{
    if (!config.built_in_plugin_dir.empty()) {
        PluginManager::setBuiltInPluginDirectory(config.built_in_plugin_dir);
    }

    auto app = std::make_unique<GuiApplication>(argc, argv);

    // The startup frame is configured before init(): that is where it is created, and the main window stays hidden until
    // finishStartup() because of it. The title the framework can resolve itself (an empty title means "the application
    // name") is resolved here, because the application name is applied after init() - QCoreApplication exists only then.
    SplashConfig splash = config.splash;
    if (splash.enabled && splash.title.empty()) {
        splash.title = config.name;
    }
    app->setSplashConfig(splash);

    app->init();
    applyAppConfig(*app, config);

    return app;
}

VN_APPFWGUI_NS_END
