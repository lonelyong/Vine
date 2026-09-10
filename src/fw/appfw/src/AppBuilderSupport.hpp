#pragma once

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/appfw_global.hpp>

V_APPFW_NS_BEGIN

class Application;

/**
 * @brief Applies the process-wide identity and the configuration file.
 *
 * Shared by createApplication() and createGuiApplication() so both builders
 * produce the same layout:
 * 1. AppConfig::name becomes the Qt application name (when set).
 * 2. AppConfig::organization becomes the Qt organization name (when set);
 *    otherwise Application::defaultOrganizationName() stays in place.
 * 3. AppConfig::config_file is loaded as the configuration file; when it is
 *    empty, Application::defaultConfigFile() is used unless
 *    AppConfig::persist_config is false.
 *
 * @param app    Application to configure.
 * @param config Application configuration.
 */
void applyAppConfig(Application& app, const AppConfig& config);

V_APPFW_NS_END
