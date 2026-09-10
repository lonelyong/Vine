#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <memory>
#include <string>

V_APPFW_NS_BEGIN

class Application;

/**
 * @brief Declarative configuration used to build an application.
 *
 * The factory functions apply the settings to the process-wide application: name
 * becomes the Qt application name, organization the Qt organization name,
 * built_in_plugin_dir the application's plugin directory, and config_file the persisted
 * configuration (defaulting to Application::defaultConfigFile() under the
 * user's data directory). language is a placeholder reserved for locale
 * selection.
 */
struct AppConfig {
    /// Application name, applied as QCoreApplication::applicationName().
    std::string name;

    ///
    /// Organization name, applied as QCoreApplication::organizationName().
    /// Empty keeps Application::defaultOrganizationName().
    std::string organization;

    ///
    /// Directory of the plugins that ship with the application
    /// (PluginScope::BuiltIn); empty keeps the default.
    std::filesystem::path built_in_plugin_dir;

    ///
    /// Configuration file to load and save; empty uses
    /// Application::defaultConfigFile().
    std::filesystem::path config_file;

    ///
    /// Whether the default configuration file is read and saved. Set to false
    /// to keep a process out of the user's configuration (tests, short-lived
    /// tools); an explicit config_file is always used regardless.
    bool persist_config = true;

    /// Locale placeholder reserved for i18n (not wired yet).
    std::string language;
};

/**
 * @brief Builds a headless application from config.
 *
 * Applies the plugin directory, initializes the application, then applies the
 * application identity (name, organization) and enables configuration
 * persistence.
 *
 * @param config Application configuration.
 * @param argc Command line argument count.
 * @param argv Command line arguments.
 * @return The initialized application.
 */
V_APPFW_API std::unique_ptr<Application> createApplication(const AppConfig& config, int argc, char** argv);

V_APPFW_NS_END
