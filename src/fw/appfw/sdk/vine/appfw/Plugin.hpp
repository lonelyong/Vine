#pragma once
#include "appfw_global.hpp"

#include <vector>

#include <vine/Object.hpp>
#include <vine/Uuid.hpp>


V_APPFW_NS_BEGIN

class PluginLoadContext;
class ConfigItem;
struct CommandInfo;

/**
 * @brief Static metadata declared by a plugin.
 */
struct V_APPFW_API PluginInfo {
    ///
    /// Stable plugin identity, hardcoded by the plugin (V_DECLARE_PLUGIN) and
    /// independent of its name and of where its library is installed. The null
    /// UUID means "not declared": identity then falls back to name.
    Uuid                uuid;
    String              name;         // Unique plugin name (identifier).
    String              display_name; // Human-friendly name shown in UI; falls back to name when empty.
    String              version;      // Plugin version, e.g. "1.2.0".
    String              description;  // Human-readable description.
    String              vendor;       // Author or vendor.
    String              email;        // Contact address of the vendor; optional.
    String              repo;         // Source repository or issue tracker URL; optional.
    ///
    /// Inline SVG source used as the plugin's icon in the UI. Empty means "no
    /// icon": the plugin manager then draws its own default one. Keep it to plain
    /// shapes - the host renders it with QSvgRenderer, which supports a static SVG
    /// subset - and a source that cannot be parsed falls back to the default icon
    /// as well.
    String              icon;
    std::vector<String> dependencies; // Plugin names required before this one.
};

/**
 * @brief Base class of a loadable plugin.
 *
 * A plugin is an Object loaded by the PluginManager. The lifecycle is: preLoad()
 * for every plugin, then load() for every plugin, then postLoad() for every
 * plugin once all have loaded (cross-plugin wiring); on shutdown unload() runs
 * for every loaded plugin in reverse dependency order (dependents first).
 *
 * A throw out of preLoad(), load() or postLoad() is caught by the manager, which
 * unloads the instance again (best effort) and reports the load as failed: a
 * plugin library can be created only once per process, so unload() must tolerate
 * being called on a plugin whose load() did not complete.
 */
class V_APPFW_API Plugin : public Object {
    V_OBJECT_META_DECL;

  public:
    /**
     * @brief Returns the plugin's static metadata.
     *
     * Populated by the PluginManager from the plugin's vinePluginQuery() entry
     * when the plugin is created; V_DECLARE_PLUGIN is the single source of the
     * metadata.
     *
     * @return The plugin metadata.
     */
    const PluginInfo& info() const;

    /**
     * @brief Returns the plugin name (convenience for info().name).
     *
     * @return The plugin name.
     */
    String name() const;

    /**
     * @brief Reports the commands registered by this plugin.
     *
     * Queries the host CommandManager for commands attributed to this plugin
     * (see CommandManager::setRegistrationOwner).
     *
     * @return The plugin's command metadata (may be empty).
     */
    std::vector<CommandInfo> commandInfos() const;

    /**
     * @brief Reports the config items registered by this plugin.
     *
     * Queries the host ConfigRegistry for items owned by this plugin.
     *
     * @return The plugin's config items (may be empty).
     */
    std::vector<const ConfigItem*> configItems() const;

  public:
    /**
     * @brief Called before load(); used for dependency checks and early setup.
     *
     * @param context Load context exposing host capabilities.
     */
    virtual void preLoad(PluginLoadContext* context);

    /**
     * @brief Registers the plugin's contributions (services, configs, ...).
     *
     * @param context Load context exposing host capabilities.
     */
    virtual void load(PluginLoadContext* context);

    /**
     * @brief Called after every plugin has loaded; used for cross-plugin wiring.
     *
     * @param context Load context exposing host capabilities.
     */
    virtual void postLoad(PluginLoadContext* context);

    /**
     * @brief Releases the plugin's resources on shutdown.
     *
     * Called by the PluginManager for every loaded plugin, in reverse dependency
     * order (a plugin is unloaded before the plugins it depends on), after the
     * main loop has stopped but while the application, the command manager, the
     * event bus and the UI are still alive, so the plugin can still publish on
     * the bus or remove its own subscriptions.
     *
     * A disabled plugin was never loaded and is therefore never unloaded. The host
     * does not revoke the plugin's registrations before calling this (its managers
     * are destroyed right afterwards), so unload() releases plugin-owned resources
     * (workers, threads, sinks, subscriptions), not host registrations. Throwing
     * from unload() is caught and logged; the remaining plugins are still
     * unloaded.
     *
     * unload() is also called after a failed load() (see the class comment), so it
     * must not assume that preLoad()/load()/postLoad() all ran; an implementation
     * that registers resources in load() must release them defensively here.
     *
     * @param context Load context exposing host capabilities.
     */
    virtual void unload(PluginLoadContext* context);

  private:
    friend class PluginManager;

    /**
     * @brief Sets the plugin's static metadata.
     *
     * Called by the PluginManager when the plugin is created (from the plugin's
     * vinePluginQuery() entry); plugin authors do not call this.
     *
     * @param info Plugin metadata.
     */
    void setInfo(PluginInfo info);

    /// Static metadata set by the PluginManager at load time.
    PluginInfo info_;
};

V_APPFW_NS_END
