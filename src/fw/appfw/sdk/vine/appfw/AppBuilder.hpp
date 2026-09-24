#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <memory>
#include <string>

VN_APPFW_NS_BEGIN

class Application;

/**
 * @brief Startup frame ("splash screen") configuration.
 *
 * The frame belongs to the application, not to the framework: it is the application's identity that is shown, and only
 * the application knows whether a frame is wanted at all (a short-lived tool or a test harness does not). It reports the
 * boot itself - which plugins are loading, and what the application is doing around them - through StartupProgress, so
 * nothing here configures the progress display.
 *
 * An application that enables the frame must end its startup phase explicitly with Application::finishStartup(): the
 * frame stays until then, because the framework cannot know when the application is done starting up.
 */
struct SplashConfig {
    /// Whether the application shows a startup frame while it boots.
    bool enabled = false;

    /// Title shown on the frame; empty uses AppConfig::name, which the GUI application builder resolves.
    std::string title;

    /// Second line under the title (version, vendor, ...); empty hides the line.
    std::string subtitle;

    /// Logo image drawn next to the title (SVG or a raster format); empty hides the logo.
    std::filesystem::path logo;
};

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

    /// Startup frame shown while the application boots; see SplashConfig.
    SplashConfig splash;
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
VN_APPFW_API std::unique_ptr<Application> createApplication(const AppConfig& config, int argc, char** argv);

VN_APPFW_NS_END
