#pragma once

#include "appfw_global.hpp"

#include <filesystem>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Startup frame ("splash screen") configuration.
 *
 * The frame belongs to the application, not to the framework: it is the application's identity that is shown, and only
 * the application knows whether a frame is wanted at all (a short-lived tool or a test harness does not). It reports the
 * boot itself - which plugins are loading, and what the application is doing around them - through StartupProgress, so
 * nothing here configures the progress display.
 *
 * An application that enables the frame has to end its startup phase (run() does it as the boot's last phase; a host
 * that drives its own boot calls Application::startupEnd() from its own class): the
 * frame stays until then, because the framework cannot know when the application is done starting up. An empty title is
 * resolved to the application name by the frame itself, which is why the name from AppConfig::name is applied before any
 * window is created.
 */
struct SplashConfig {
    /// Whether the application shows a startup frame while it boots.
    bool enabled = false;

    /// Title shown on the frame (UTF-8); empty uses the Qt application name (AppConfig::name).
    String title;

    /// Second line under the title (version, vendor, ...), UTF-8; empty hides the line.
    String subtitle;

    /// Logo image drawn next to the title (SVG or a raster format); empty hides the logo.
    std::filesystem::path logo;
};

/**
 * @brief Declarative configuration of an application.
 *
 * The application constructors apply the settings to the process-wide application: name becomes the Qt application name,
 * organization the Qt organization name, built_in_plugin_dir the application's plugin directory (through the builders),
 * and config_file the persisted configuration (defaulting to Application::defaultConfigFile() under the user's data
 * directory). language is a placeholder reserved for locale selection.
 */
struct AppConfig {
    /// Application name (UTF-8), applied as QCoreApplication::applicationName().
    String name;

    ///
    /// Organization name (UTF-8), applied as QCoreApplication::organizationName().
    /// Empty keeps Application::defaultOrganizationName().
    String organization;

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

    ///
    /// Whether the framework loads the plugins during the startup phase
    /// (PluginManager::loadAllAsync()), so a host does not write that out itself. The load happens once
    /// the startup interface is on screen and before the host's own startup work (see
    /// Application::startup()), which is what lets a host rely on the commands and services
    /// the plugins register. Set to false for a host that manages the plugin list itself: a test
    /// that loads specific plugins, a tool that loads none.
    bool load_plugins = true;

    /// Locale placeholder reserved for i18n (not wired yet).
    String language;

    /// Startup frame shown while the application boots; see SplashConfig.
    SplashConfig splash;
};

VN_APPFW_NS_END
