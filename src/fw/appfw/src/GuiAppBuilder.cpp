#include <vine/appfw/gui/GuiAppBuilder.hpp>

#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

VN_APPFWGUI_NS_BEGIN

std::unique_ptr<GuiApplication> createGuiApplication(const AppConfig& config, int argc, char** argv)
{
    if (!config.built_in_plugin_dir.empty()) {
        PluginManager::setBuiltInPluginDirectory(config.built_in_plugin_dir);
    }

    // The constructor does the rest: the identity, the QApplication, the user IO, the configuration file and the
    // windows. The startup frame comes from AppConfig::splash and resolves an empty title to the application name
    // itself, so nothing has to be patched in here.
    return std::make_unique<GuiApplication>(config, argc, argv);
}

VN_APPFWGUI_NS_END
