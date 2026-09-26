#include <vine/appfw/AppBuilder.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/PluginManager.hpp>

VN_APPFW_NS_BEGIN

std::unique_ptr<Application> createApplication(const AppConfig& config, int argc, char** argv)
{
    if (!config.built_in_plugin_dir.empty()) {
        PluginManager::setBuiltInPluginDirectory(config.built_in_plugin_dir);
    }

    // The constructor does the rest: the identity, the Qt application object, the user IO and the configuration file.
    return std::make_unique<Application>(config, argc, argv);
}

VN_APPFW_NS_END
