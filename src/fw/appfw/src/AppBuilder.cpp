#include <vine/appfw/AppBuilder.hpp>

#include <QCoreApplication>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/PluginManager.hpp>

#include "AppBuilderSupport.hpp"

V_APPFW_NS_BEGIN

void applyAppConfig(Application& app, const AppConfig& config)
{
    if (!config.name.empty()) {
        QCoreApplication::setApplicationName(QString::fromStdString(config.name));
    }
    if (!config.organization.empty()) {
        QCoreApplication::setOrganizationName(QString::fromStdString(config.organization));
    }

    // An explicit file always wins; otherwise the framework layout under the
    // user's data directory is used unless the caller opted out (tests, tools).
    if (!config.config_file.empty()) {
        app.setConfigFile(config.config_file);
        return;
    }
    if (config.persist_config) {
        app.setConfigFile(app.defaultConfigFile());
    }
}

std::unique_ptr<Application> createApplication(const AppConfig& config, int argc, char** argv)
{
    if (!config.built_in_plugin_dir.empty()) {
        PluginManager::setBuiltInPluginDirectory(config.built_in_plugin_dir);
    }

    auto app = std::make_unique<Application>(argc, argv);
    app->init();
    applyAppConfig(*app, config);

    return app;
}

V_APPFW_NS_END
