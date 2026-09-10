#pragma once
#include "appfw_global.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

#include <vine/String.hpp>

#include <vine/appfw/Plugin.hpp>

V_APPFW_NS_BEGIN

struct CommandInfo;
class ConfigItem;

/**
 * @brief Where a plugin comes from; decides what a user may do with it.
 */
enum class PluginScope
{
    BuiltIn,  ///< Ships with the application (builtInPluginDirectory()); cannot be disabled or uninstalled.
    User,     ///< Registered for the current user only (per-user registration directory).
    AllUsers, ///< Registered for every user of the machine (system registration directory).
};

/**
 * @brief A plugin library found in one of the configured plugin locations.
 *
 * Discovery is independent of loading: a plugin that the user disabled, or that
 * the host skips, is still discovered and reported here (its metadata comes from
 * the library's query entry point), but it is never instantiated and its
 * lifecycle never runs. enabled, skipped and loaded are resolved by
 * pluginEntries() at query time.
 */
struct V_APPFW_API PluginEntry {
    PluginInfo            info;                             ///< Metadata declared by the plugin.
    std::filesystem::path path;                             ///< Library file the plugin was found in.
    PluginScope           scope{ PluginScope::BuiltIn };    ///< Location class the plugin was found in.
    String                framework_version;                ///< Framework version the library was built with (PluginAbi); may be empty.
    bool                  enabled{ true };                  ///< Effective preference; disabled plugins are not loaded.
    bool                  loaded{ false };                  ///< Whether an instance exists right now.
    bool                  skipped{ false };                 ///< Refused by the host skip list (setSkipList()); implies !enabled.
};

/**
 * @brief One installed-plugin registration, read from an installed.d/<id>.plugin file.
 *
 * A registration is a drop-in file: a plugin (or an installer) writes it into the
 * per-user or the system registration directory, and the next start loads the
 * plugin it points at. Writing a file needs no application support, which is how
 * a package manager or an administrator installs a plugin.
 */
struct V_APPFW_API PluginRegistration {
    String                id;                  ///< Registration file name without the extension (unique per scope).
    String                file;                ///< The registration file itself.
    String                path;                ///< Registered plugin library or directory; required.
    String                name;                ///< Plugin name recorded in the file; empty when unknown.
    Uuid                  uuid;                ///< Plugin identity recorded in the file; null when unknown.
    PluginScope           scope{ PluginScope::User }; ///< Directory the file was read from.
    bool                  enabled{ true };      ///< false: disabled for everyone (administrator policy).
};

/**
 * @brief Loads and manages plugins.
 *
 * Provides the plugin search directory configuration and loads plugins from
 * dynamic libraries. Plugins are found in three kinds of locations, in this
 * order: the application's own directory (PluginScope::BuiltIn), the per-user
 * registration directory and the system registration directory (see
 * installPlugin() and pluginRegistrations()). The first location that provides a
 * plugin name wins, so an application-provided plugin is never overridden.
 *
 * What a user may do depends on that scope: an application-provided plugin
 * cannot be disabled or uninstalled (the application decides what it ships),
 * while a registered plugin can. Disabling is a per-user preference that is
 * persisted and, like installing and uninstalling, takes effect at the next
 * start; a registration file may also disable a plugin for every user
 * (administrator policy).
 *
 * Independent of all that, the host itself can refuse a plugin with
 * setSkipList(): that switch is process-local and wins over every other input,
 * including an application-provided plugin. A skipped plugin is still discovered
 * and listed (PluginEntry::skipped) so it stays visible, but it is never
 * instantiated.
 *
 * Thread and re-entrancy contract: the manager is not thread-safe. load(),
 * loadAll() and unloadAll() run on the thread that owns the Application, never
 * from a plugin lifecycle callback (a nested loadAll() is refused and logged),
 * and the process-wide configuration (setBuiltInPluginDirectory(), setSkipList())
 * is set before the first load.
 *
 * Hard constraint: a plugin library can be instantiated only once per process,
 * because its commands register into a process-global, non-revocable type
 * registry. Instances are owned by their library and are never destroyed, so
 * unloadAll() followed by loadAll() runs the lifecycle again on the same
 * instances.
 */
class V_APPFW_API PluginManager {
  public:
    PluginManager();
    ~PluginManager();

  public:
    /**
     * @brief Sets the application's own plugin directory (PluginScope::BuiltIn).
     *
     * Global static configuration; call before loading plugins. Registered
     * locations (installed.d) are scanned in addition to it, and it always wins
     * over them for the same plugin name.
     *
     * @param dir Built-in plugin directory.
     */
    static void setBuiltInPluginDirectory(const std::filesystem::path& dir);

    /**
     * @brief Returns the application's own plugin directory (PluginScope::BuiltIn).
     *
     * Where the plugins that ship with the application live: <exe>/plugins/vine
     * on Windows and <appdir>/plugins/vine on Linux (appdir is the directory
     * containing the bin/ directory). Plugins found here cannot be disabled or
     * uninstalled; the application decides what it ships.
     *
     * @return The built-in plugin directory.
     */
    [[nodiscard]] static std::filesystem::path builtInPluginDirectory();

    /**
     * @brief Sets the host's skip list: plugin names that must not be loaded.
     *
     * The host's own, process-local switch: it is never persisted and takes
     * effect at the next load()/loadAll(), so it is how an application keeps a
     * plugin out of a specific run (headless mode, safe mode, a command-line
     * override) without touching the user's stored preference. It wins over every
     * other input, including a plugin that ships with the application, which a
     * user cannot disable.
     *
     * A skipped plugin is still discovered and listed by pluginEntries(), with
     * PluginEntry::skipped set, so a UI can report "skipped by the host" instead
     * of showing nothing. Skipping does not unload anything: an already-loaded
     * plugin keeps running until unloadAll().
     *
     * @param names Plugin names to skip.
     */
    static void setSkipList(const std::vector<String>& names);

    /**
     * @brief Returns the host's skip list.
     *
     * Returns a const view of the internal list; no copy is made. Copy it before
     * calling setSkipList() when the previous list has to be restored.
     *
     * @return The skip list.
     */
    [[nodiscard]] static const std::vector<String>& skipList();

    /**
     * @brief Adds a plugin name to the host's skip list.
     *
     * @param name Plugin name to skip.
     */
    static void addToSkipList(String name);

    /**
     * @brief Removes a plugin name from the host's skip list.
     *
     * Unlike disabling, the skip list is not persisted, so removing a name is
     * enough for the next load()/loadAll() to load the plugin again within the
     * same run.
     *
     * @param name Plugin name.
     */
    static void removeFromSkipList(const String& name);

    /**
     * @brief Returns whether the host skips a plugin name.
     *
     * @param name Plugin name.
     * @return true if skipped.
     */
    [[nodiscard]] static bool isSkipped(const String& name);

    /**
     * @brief Returns the ConfigManager key holding the disabled plugin names.
     *
     * The value is a string array; the key is a dotted path so it is written to
     * the nested "plugins" object of the configuration JSON.
     *
     * @return The config key.
     */
    static const String& disabledConfigKey();

    /**
     * @brief Reports whether a plugin's declared ABI revision fits this host.
     *
     * This is the rule the manager applies before it reads anything else a plugin
     * declares (see PluginAbi): a library built against a different SDK reports a
     * different revision and is refused, because reading its PluginInfo with this
     * SDK's layout would misinterpret it. Exposed so that the rule has exactly one
     * implementation and a host can check a library before handing it to load().
     *
     * @param abi Build description reported by the library's vinePluginAbi().
     * @return true when the manager may read the plugin's metadata.
     */
    [[nodiscard]] static bool isPluginAbiCompatible(const PluginAbi& abi) noexcept;


  public:
    /**
     * @brief Loads a plugin by name or library path.
     *
     * A name is resolved against builtInPluginDirectory() with the platform library
     * extension. The library is queried for the Vine plugin entry points and
     * the DLL-global plugin instance is created (two-step load).
     *
     * A disabled plugin (see setPluginEnabled) and a plugin the host skips (see
     * setSkipList) are refused: neither is instantiated even when its library is
     * named explicitly. The library and the plugin's metadata are still
     * discovered and reported by pluginEntries().
     *
     * Unlike loadAll(), this entry point does not resolve dependencies: a plugin
     * whose declared dependency is not loaded is loaded anyway (with a warning),
     * and its position in the load list carries no dependency meaning. Unloading
     * does not rely on that position (see unloadOrder()).
     *
     * @param name_or_path Plugin name or library path.
     * @return The loaded plugin, or nullptr on failure.
     */
    [[nodiscard]] Plugin* load(const String& name_or_path);

    /**
     * @brief Loads every plugin library found in the configured locations.
     *
     * Scans builtInPluginDirectory() and every registered location
     * (pluginRegistrations(); each one a library file or a directory) and
     * collects the metadata of every valid Vine plugin without creating
     * instances yet. Dependencies are then resolved against the newly
     * discovered plugins and the already-loaded ones; when every dependency is
     * satisfiable, instances are created in dependency order and the three-phase
     * lifecycle runs: preLoad() for all plugins, then load() for all plugins, then
     * postLoad() for all plugins.
     *
     * A plugin provided by several locations is discovered once, from the first
     * location that provides it, so builtInPluginDirectory() wins over an installed copy
     * of the same plugin. A registered location that does not exist or holds no
     * plugin library is reported and skipped.
     *
     * Plugins disabled through setPluginEnabled() and plugins skipped through
     * setSkipList() are discovered and reported by pluginEntries(), but no
     * instance is created and no lifecycle phase runs.
     *
     * Dependencies are resolved as a closure: a plugin joins the batch only once
     * every dependency of it is already loaded or already in the batch. A plugin
     * whose dependencies cannot be satisfied - one is missing, disabled, skipped, or
     * blocked itself - is therefore left out together with everything that depends on
     * it, while the remaining plugins still load: one broken third-party plugin must
     * not cost the application its shell. Every plugin left out is reported with its
     * own reason, so the log tells "install it", "enable it" and "stop skipping it"
     * apart, and a declared cycle is called out as such. The return value reports that
     * something was left out.
     *
     * The load is atomic as seen from the manager: when a plugin cannot be instantiated,
     * or throws out of preLoad()/load()/postLoad(), the instances created by this call
     * are unloaded again (best effort, newest first) and none of them is kept. It stays
     * atomic there because a plugin that failed at run time may already have left state
     * behind, while an unsatisfiable dependency is a static property of the set that can
     * be isolated to the plugin and its dependents. A nested call from a plugin
     * lifecycle callback is refused and logged.
     *
     * @return false if plugins were left out because their dependencies cannot be
     *         satisfied (including a dependency cycle), or if a plugin failed to
     *         instantiate or threw out of its lifecycle.
     */
    [[nodiscard]] bool loadAll();

    /**
     * @brief Unloads every loaded plugin in reverse dependency order.
     *
     * Runs Plugin::unload() for each loaded plugin, always unloading a plugin
     * before the plugins it depends on, while the application, the command
     * manager and the event bus are still alive. The order is computed from the
     * declared dependencies (see unloadOrder()), not from the position in the
     * load list, so it stays correct even when a plugin was appended by an
     * explicit load() after its dependency.
     *
     * A plugin whose unload() throws is logged and skipped so that the remaining
     * plugins are still unloaded; the plugin is considered unloaded in both
     * cases, so a second call is a no-op.
     *
     * The host does not roll back the plugin's registrations here: an unload is
     * only performed while shutting down, and the host's registries are destroyed
     * right afterwards. unload() must therefore release plugin-owned resources
     * (threads, workers, sinks, subscriptions), not host registrations.
     *
     * A plugin that throws is reported through the return value, so a shutdown
     * path that cannot act on it should cast the result to void deliberately.
     *
     * @return true if every unload() completed without throwing.
     */
    [[nodiscard]] bool unloadAll();

    /**
     * @brief Returns the order unloadAll() unloads the given plugins in.
     *
     * Reverse dependency order: a plugin appears before every plugin it depends
     * on (transitively, via the declared PluginInfo::dependencies), so a
     * dependent is always unloaded before its dependency. Entries that are not
     * loaded, and dependencies that are not in the set, are ignored; plugins that
     * depend on each other keep their entry order, so the result is well defined
     * even for a cycle that only an explicit load() can create.
     *
     * The order is a pure function of the entries, which is exactly what
     * unloadAll() uses, so a host can inspect (or test) the sequence before
     * shutting down.
     *
     * @param entries Plugin entries, as reported by pluginEntries().
     * @return The names of the loaded plugins, in unload order.
     */
    [[nodiscard]] static std::vector<String> unloadOrder(const std::vector<PluginEntry>& entries);

  public:
    /**
     * @brief Returns the loaded plugin with the given name.
     *
     * @param name Plugin name.
     * @return The plugin instance, or nullptr if not loaded.
     */
    [[nodiscard]] Plugin* plugin(const String& name) const;

    /**
     * @brief Returns whether a plugin with the given name is loaded.
     *
     * @param name Plugin name.
     * @return true if loaded.
     */
    [[nodiscard]] bool isLoaded(const String& name) const;

    /**
     * @brief Returns the number of loaded plugins.
     *
     * @return The plugin count.
     */
    [[nodiscard]] std::size_t count() const;

    /**
     * @brief Returns the library file path of the loaded plugin.
     *
     * Plugins that were discovered but not loaded (disabled or failed to
     * instantiate) are reported as well.
     *
     * @param name Plugin name.
     * @return The library path, or an empty path if the plugin is unknown.
     */
    [[nodiscard]] std::filesystem::path libraryPath(const String& name) const;

    /**
     * @brief Returns the metadata and runtime state of every discovered plugin.
     *
     * Includes plugins that are disabled, plugins the host skips and plugins that
     * failed to load, so a UI can list them without instantiating anything.
     * Entries are ordered by plugin name; a plugin that was never seen by a
     * directory scan but loaded from an explicit path (load()) is included as
     * well.
     *
     * The state flags are resolved here, at query time, so the list cannot go
     * stale: PluginEntry::skipped reports the host's skip list and
     * PluginEntry::enabled the user/policy preference.
     *
     * @return The plugin entries.
     */
    [[nodiscard]] std::vector<PluginEntry> pluginEntries() const;

    /**
     * @brief Returns the names of all loaded plugins in load order.
     *
     * @return The plugin names.
     */
    [[nodiscard]] std::vector<String> names() const;

    /**
     * @brief Returns the instances of all loaded plugins in load order.
     *
     * @return The plugin instances.
     */
    [[nodiscard]] std::vector<Plugin*> plugins() const;

    /**
     * @brief Reports the commands registered by the given plugin.
     *
     * Delegates to the host CommandManager; commands are attributed to a plugin
     * while it is being loaded (see CommandManager::setRegistrationOwner).
     *
     * @param name Plugin name.
     * @return The plugin's command metadata (may be empty).
     */
    [[nodiscard]] std::vector<CommandInfo> commandInfosForPlugin(const String& name) const;

    /**
     * @brief Reports the config items registered by the given plugin.
     *
     * Delegates to the host ConfigRegistry; items are recorded with their owner
     * when the plugin registers them through PluginLoadContext.
     *
     * @param name Plugin name.
     * @return The plugin's config items (may be empty).
     */
    [[nodiscard]] std::vector<const ConfigItem*> configItemsForPlugin(const String& name) const;

    /**
     * @brief Returns the effective enabled state of a plugin.
     *
     * The state is the result of four inputs, in this order: a plugin on the
     * host's skip list (setSkipList) is refused, because the host's own switch
     * wins over everything; an application-provided plugin (PluginScope::BuiltIn)
     * is always enabled; a registration file that disables the plugin wins over
     * the user (administrator policy); otherwise the per-user preference from
     * setPluginEnabled() applies.
     *
     * Reads the per-user preference from the host ConfigManager when one is
     * available and falls back to the process-local preference otherwise.
     *
     * @param name Plugin name.
     * @return true if the plugin may be loaded.
     */
    [[nodiscard]] bool isPluginEnabled(const String& name) const;

    /**
     * @brief Enables or disables a plugin and persists the user's preference.
     *
     * The preference is written to the host ConfigManager (disabledConfigKey())
     * so it survives a restart; without a config manager it is kept in memory.
     * The running state is deliberately untouched: disabling does not unload an
     * already-loaded plugin, and enabling does not load one. Both take effect at
     * the next loadAll(), i.e. at the next start.
     *
     * An application-provided plugin (PluginScope::BuiltIn) is refused, because
     * the application decides what it ships; use setSkipList() to keep a plugin
     * out of the process instead. A plugin that a registration file disables for
     * every user can be enabled here, but the policy keeps it disabled; the same
     * holds for a plugin on the host's skip list, which is only released by
     * removeFromSkipList().
     *
     * @param name Plugin name.
     * @param enabled true to enable, false to disable.
     * @return true if the preference was accepted (and handed to the host
     *         ConfigManager when there is one), false when it was refused. A failing
     *         config write is not reported here, because
     *         ConfigManager::setStringArray() reports none.
     */
    [[nodiscard]] bool setPluginEnabled(const String& name, bool enabled);

    /**
     * @brief Registers a plugin location, so it is found after a restart.
     *
     * Writes an installed.d/<id>.plugin file into the registration directory of
     * the given scope and returns its id: PluginScope::User writes into the
     * per-user directory (no privileges needed), PluginScope::AllUsers into the
     * system directory (needs write access, so a normal user gets false and
     * should be offered the per-user scope instead). PluginScope::BuiltIn is
     * refused: what ships with the application is not installed by the user.
     *
     * The location may be a plugin library file or a directory of libraries,
     * anywhere on disk. When it holds exactly one plugin, the file is named after
     * that plugin, otherwise after the location. Registering does not load
     * anything: the plugin takes part in the next loadAll(). Use load() to also
     * run a plugin immediately, like the plugin manager dialog does for a trial.
     *
     * The id doubles as the file name inside the registration directory, so it must
     * be a plain file name: a name with a path separator, ":" or a control
     * character is refused instead of producing a file outside the directory.
     *
     * @param path Library file or directory to register.
     * @param scope Scope to register it in; defaults to the current user.
     * @return The registration id, or an empty string when the path is empty or
     *         missing, the scope is BuiltIn, the id is not usable as a file name,
     *         or the file could not be written.
     */
    [[nodiscard]] String installPlugin(const String& path, PluginScope scope = PluginScope::User);

    /**
     * @brief Removes a registration, so its plugin is no longer found.
     *
     * Deletes the installed.d/<id>.plugin file written by installPlugin(). The
     * counterpart of a registration: it takes effect at the next start, and a
     * plugin that is currently loaded keeps running until then (see unloadAll()
     * for the shutdown path).
     *
     * @param id Registration id as returned by installPlugin().
     * @param scope Scope the registration was written in.
     * @return true if the registration existed and is now removed.
     */
    [[nodiscard]] bool uninstallPlugin(const String& id, PluginScope scope);

    /**
     * @brief Returns every installed-plugin registration that is readable now.
     *
     * The per-user directory is listed before the system ones, so display order
     * and the precedence used when the same plugin name is registered twice both
     * have the user's own choice first. A malformed file is reported and skipped.
     *
     * @return The registrations, in precedence order.
     */
    [[nodiscard]] std::vector<PluginRegistration> pluginRegistrations() const;

  private:
    /// Reports whether a registration disables a plugin for every user.
    ///
    /// The scan in loadAll() records the policy of every plugin it discovers; this
    /// lookup falls back to reading the registration files when the fast path has
    /// nothing, so the policy holds even in a process that never ran loadAll() - an
    /// explicit load() must not resurrect a plugin an administrator disabled.
    ///
    /// @param name    Plugin name.
    /// @param library Library the plugin is loaded from, or an empty path when only
    ///                the name is known.
    /// @return true when a registration disables the plugin for all users.
    [[nodiscard]] bool isPolicyDisabled(const String& name, const std::filesystem::path& library) const;

    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
