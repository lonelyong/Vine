#include <vine/appfw/PluginManager.hpp>

#ifdef V_CC_MSVC
#    include <Windows.h>
#endif // V_CC_MSVC

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __linux__
#    include <unistd.h>
#endif // __linux__

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/logging/Log.hpp>
#include <vine/runtime/DynamicLibraryLoader.hpp>

V_APPFW_NS_BEGIN

namespace
{

/// Converts a String to a std::string for fmt-based logging.
std::string toUtf8(const String& s)
{
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

/// Returns a UTF-8 text view of a String, for file output.
std::string_view toUtf8View(const String& s)
{
    return std::string_view(reinterpret_cast<const char*>(s.data()), s.size());
}

/// Converts a filesystem path to UTF-8 text, for logging.
///
/// path::string() uses the native narrow encoding, which throws for characters
/// the ANSI code page cannot represent; the project is UTF-8 throughout, so paths
/// are converted through u8string().
std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// Converts UTF-8 bytes held in a std::string to a String.
String fromUtf8(const std::string& text)
{
    return String(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

/// Returns the text without surrounding spaces, tabs and newlines.
std::string trimmed(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/**
 * @brief Returns the directory containing the current executable.
 */
std::filesystem::path executableDir()
{
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH> buf{};
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        return std::filesystem::path(std::wstring(buf.data(), len)).parent_path();
    }
#elif defined(__linux__)
    std::array<char, 4096> buf{};
    const auto len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0) {
        return std::filesystem::path(std::string(buf.data(), static_cast<std::size_t>(len))).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

/**
 * @brief Returns the platform-specific default plugin directory.
 *
 * Windows: <exe>/plugins/vine. Linux: <appdir>/plugins/vine where appdir is
 * the directory containing the bin/ directory (the executable lives in bin).
 */
std::filesystem::path defaultBuiltInPluginDirectory()
{
#if defined(_WIN32)
    return executableDir() / "plugins" / "vine";
#elif defined(__linux__)
    return executableDir().parent_path() / "plugins" / "vine";
#else
    return executableDir() / "plugins" / "vine";
#endif
}

/**
 * @brief Plugin search directory storage, initialized on first access.
 */
std::filesystem::path& builtInPluginDirectoryStorage()
{
    static std::filesystem::path dir = defaultBuiltInPluginDirectory();
    return dir;
}

/// Skip-list storage: plugin names that load() must not load.
std::vector<String>& skipListStorage()
{
    static std::vector<String> list;
    return list;
}

/**
 * @brief Returns the host config manager, or nullptr when none is available.
 */
ConfigManager* hostConfigManager()
{
    Application* app = Application::current();
    return app ? app->configManager() : nullptr;
}

/**
 * @brief Reads the user's disabled plugin names from the host config.
 *
 * This is the per-user preference (PluginManager::disabledConfigKey()) and not
 * the administrator policy, which lives in the registration files. Returns an
 * empty list when no config manager is available, in which case the manager's
 * process-local list is authoritative.
 */
std::vector<String> userDisabledFromConfig()
{
    if (ConfigManager* cfg = hostConfigManager(); cfg != nullptr) {
        return cfg->getStringArray(PluginManager::disabledConfigKey());
    }
    return {};
}

/**
 * @brief Returns the names the user disabled: the stored preference when the host
 * has a ConfigManager, the process-local list otherwise.
 *
 * @param fallback Process-local preference of the manager; used without a config.
 * @return The disabled plugin names.
 */
std::vector<String> disabledNames(const std::vector<String>& fallback)
{
    return hostConfigManager() != nullptr ? userDisabledFromConfig() : fallback;
}

/**
 * @brief Applies the four inputs that decide whether a plugin may be loaded.
 *
 * A free function so that pluginEntries() can resolve a whole list from one
 * snapshot of the inputs instead of one config read per plugin; the order of the
 * checks is the one documented by PluginManager::isPluginEnabled(), which uses it
 * as well.
 *
 * @param name              Plugin name.
 * @param is_built_in       Whether the plugin ships with the application.
 * @param is_skipped        Whether the host's skip list refuses it.
 * @param policy_disables   Whether a registration file disables it for everyone.
 * @param user_disabled     Names disabled by the per-user preference.
 * @return true if the plugin may be loaded.
 */
bool resolveEnabled(const String& name, bool is_built_in, bool is_skipped, bool policy_disables, const std::vector<String>& user_disabled)
{
    // The host's own switch wins over everything, including what ships with the
    // application: that is how a single run (headless, safe mode) keeps a plugin
    // out without touching the user's stored preference.
    if (is_skipped) {
        return false;
    }
    // What ships with the application is not the user's to disable, and neither is
    // the user's preference.
    if (is_built_in) {
        return true;
    }
    // An administrator policy is not user-togglable either.
    if (policy_disables) {
        return false;
    }
    return std::find(user_disabled.begin(), user_disabled.end(), name) == user_disabled.end();
}

/**
 * @brief Logs why a plugin is not being loaded, keeping the three reasons apart.
 *
 * The reason decides what the operator has to change: the host's own skip list
 * (setSkipList), an administrator's registration file, or the user's preference -
 * which is why a skipped plugin is logged as a decision and a disabled one as a
 * warning.
 *
 * @param name              Plugin name.
 * @param policy_disables   Whether a registration file disables it for everyone.
 */
void logRefusal(const String& name, bool policy_disables)
{
    if (PluginManager::isSkipped(name)) {
        V_LOGI("Plugin '{}' is skipped by the host; not loading it", toUtf8(name));
    }
    else if (policy_disables) {
        V_LOGW("Plugin '{}' is disabled for all users by its registration file; not loading it", toUtf8(name));
    }
    else {
        V_LOGW("Plugin '{}' is disabled; not loading it", toUtf8(name));
    }
}

/**
 * @brief Records a discovered plugin in the discovery list.
 *
 * Discovery is independent of loading: the list holds every valid Vine plugin
 * found in any plugin location, including the disabled ones and the ones the host
 * skips. The enabled, skipped and loaded flags of the returned entries are filled
 * in by pluginEntries() at query time.
 *
 * @param discovered        Discovery list of the manager.
 * @param info              Plugin metadata.
 * @param path              Library the plugin was found in.
 * @param scope             Location class the plugin was found in.
 * @param framework_version Framework version the library was built with.
 */
void rememberDiscovered(std::vector<PluginEntry>& discovered, const PluginInfo& info, const std::filesystem::path& path, PluginScope scope,
                        const String& framework_version)
{
    const auto known = std::find_if(discovered.begin(), discovered.end(),
        [&info](const PluginEntry& entry) { return entry.info.name == info.name; });
    if (known != discovered.end()) {
        // The same name provided by a second location: the first one (higher
        // precedence) is the plugin, but a different identity is worth reporting
        // because it means two unrelated plugins picked the same name.
        if (!info.uuid.isNull() && !known->info.uuid.isNull() && info.uuid != known->info.uuid) {
            V_LOGW("Two different plugins are named '{}': keeping '{}', ignoring '{}'",
                   toUtf8(info.name), toUtf8(known->path), toUtf8(path));
        }
        return;
    }
    // The state flags keep their defaults (enabled, not loaded, not skipped);
    // pluginEntries() resolves them when the entry is reported.
    discovered.push_back(PluginEntry{ .info = info, .path = path, .scope = scope, .framework_version = framework_version });
}

/**
 * @brief Returns the discovery entry of a plugin, or nullptr when unknown.
 */
const PluginEntry* findDiscovered(const std::vector<PluginEntry>& discovered, const String& name)
{
    for (const auto& entry : discovered) {
        if (entry.info.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// Extension of a plugin registration file (installed.d/<id>.plugin).
constexpr const char* s_registration_extension = ".plugin";

/**
 * @brief Returns the per-user and system plugin registration directories.
 *
 * The per-user one comes first: registering for the current user always wins over
 * a machine-wide registration of the same plugin. Empty without an Application.
 */
std::vector<std::filesystem::path> registrationDirectories()
{
    std::vector<std::filesystem::path> directories;
    Application*                       app = Application::current();
    if (app == nullptr) {
        return directories;
    }

    directories.push_back(app->pluginRegistrationDirectory());
    const auto system = app->allUsersPluginRegistrationDirectories();
    directories.insert(directories.end(), system.begin(), system.end());
    return directories;
}

/**
 * @brief Parses one installed.d/<id>.plugin registration file.
 *
 * Format: one "key = value" per line, "#" or ";" starts a comment. The required
 * key is "path" (a plugin library or a directory of libraries); "name", "uuid"
 * and "enabled" are optional and only describe the registration ("enabled =
 * false" disables the plugin for every user).
 *
 * @param file  Registration file.
 * @param scope Directory the file was read from.
 * @param id    Registration id (file name without the extension).
 * @return The registration, or std::nullopt when the file is unreadable or has no
 *         usable path.
 */
std::optional<PluginRegistration> parseRegistration(const std::filesystem::path& file, PluginScope scope, const String& id)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        V_LOGW("Cannot read plugin registration '{}'", toUtf8(file));
        return std::nullopt;
    }

    PluginRegistration registration;
    registration.id    = id;
    registration.file  = String(file.u8string());
    registration.scope = scope;

    std::string line;
    while (std::getline(stream, line)) {
        const std::string unpadded = trimmed(line);
        if (unpadded.empty() || unpadded[0] == '#' || unpadded[0] == ';') {
            continue;
        }
        const auto separator = unpadded.find('=');
        if (separator == std::string::npos) {
            V_LOGW("Plugin registration '{}' has a malformed line: '{}'", toUtf8(file), unpadded);
            continue;
        }

        const std::string key   = trimmed(unpadded.substr(0, separator));
        const std::string value = trimmed(unpadded.substr(separator + 1));
        if (key == "path") {
            registration.path = fromUtf8(value);
        }
        else if (key == "name") {
            registration.name = fromUtf8(value);
        }
        else if (key == "uuid") {
            registration.uuid = Uuid::parse(fromUtf8(value));
        }
        else if (key == "enabled") {
            registration.enabled = !(value == "false" || value == "0" || value == "no");
        }
        else {
            // Unknown keys are kept harmless: an installer may add its own
            // bookkeeping next to the keys the manager understands.
            V_LOGI("Plugin registration '{}' has an unknown key '{}'", toUtf8(file), key);
        }
    }

    if (registration.path.empty()) {
        V_LOGW("Plugin registration '{}' has no 'path'; ignoring it", toUtf8(file));
        return std::nullopt;
    }
    return registration;
}

/**
 * @brief Reads every installed.d/<id>.plugin file of the given directory.
 *
 * @param directory Registration directory.
 * @param scope     Scope to tag the registrations with.
 * @return The registrations, ordered by id.
 */
std::vector<PluginRegistration> registrationsIn(const std::filesystem::path& directory, PluginScope scope)
{
    std::vector<PluginRegistration> registrations;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return registrations;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == s_registration_extension) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        const String id = String(file.stem().u8string());
        if (auto registration = parseRegistration(file, scope, id); registration.has_value()) {
            registrations.push_back(std::move(*registration));
        }
    }
    return registrations;
}

/**
 * @brief Returns a path without "." and ".." components, never throwing.
 *
 * weakly_canonical() (the non-throwing overload) needs the filesystem to answer;
 * when that fails (permission, symlink loop, path too long) the lexical form is
 * still good enough for the containment test below.
 *
 * @param path Path to normalize.
 * @return The normalized path, or the lexically normal form on failure.
 */
std::filesystem::path normalizedPath(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        result = path.lexically_normal();
    }
    return result;
}

/**
 * @brief Returns whether a path is provided by the application itself.
 *
 * @param path Plugin library path.
 * @return true for a library inside PluginManager::builtInPluginDirectory().
 */
bool isBuiltInPath(const std::filesystem::path& path)
{
    const std::filesystem::path dir  = normalizedPath(PluginManager::builtInPluginDirectory());
    const std::filesystem::path file = normalizedPath(path);
    if (dir.empty() || file.empty()) {
        return false;
    }
    const auto relative = file.lexically_relative(dir);
    if (relative.empty()) {
        return false;
    }
    return *relative.begin() != std::filesystem::path("..");
}

/**
 * @brief Writes a registration file, replacing it atomically.
 *
 * A temporary file is written next to the target and renamed over it, so a
 * concurrent reader (or another installer) never sees a half-written file.
 *
 * @param target       Registration file to write.
 * @param registration Registration to store.
 * @return true on success.
 */
bool writeRegistrationFile(const std::filesystem::path& target, const PluginRegistration& registration)
{
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);

    std::filesystem::path temporary = target;
    temporary += ".tmp";

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            V_LOGW("Cannot write the plugin registration '{}'", toUtf8(temporary));
            return false;
        }
        stream << "# Vine plugin registration; written by PluginManager::installPlugin().\n";
        stream << "path = " << toUtf8View(registration.path) << "\n";
        if (!registration.name.empty()) {
            stream << "name = " << toUtf8View(registration.name) << "\n";
        }
        if (!registration.uuid.isNull()) {
            stream << "uuid = " << toUtf8View(registration.uuid.toString()) << "\n";
        }
        if (!registration.enabled) {
            stream << "enabled = false\n";
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        V_LOGW("Cannot create the plugin registration '{}'", toUtf8(target));
        return false;
    }
    return true;
}

/**
 * @brief Returns whether a registration id can be used as a file name.
 *
 * The id becomes installed.d/<id>.plugin, so it must not be able to leave that
 * directory: path separators, the reserved names "." and ".." and control
 * characters are rejected. Everything else is kept, UTF-8 included, because the
 * file is opened through the wide-character API on Windows.
 *
 * @param id Registration id candidate.
 * @return true if the id is usable as a file name.
 */
bool isUsableRegistrationId(const String& id)
{
    if (id.empty() || id == u8"." || id == u8"..") {
        return false;
    }
    for (auto it = id.cbegin(); it != id.cend(); ++it) {
        const auto c = static_cast<unsigned>(*it);
        // ':' is rejected as well: Windows reads it as a drive or stream separator.
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Returns whether a plugin declares the given plugin as a dependency.
 */
bool dependsOn(const PluginEntry& entry, const String& name)
{
    const auto& deps = entry.info.dependencies;
    return std::find(deps.begin(), deps.end(), name) != deps.end();
}

/// Returns the platform plugin library file extension.
std::filesystem::path pluginExtension()
{
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

/**
 * @brief A plugin library that was loaded and vetted for this host.
 *
 * The library stays mapped for the process lifetime (see DynamicLibraryLoader), so
 * loading one before any decision to install or load it has no lifetime consequence.
 */
struct QueriedLibrary {
    vine::runtime::DynamicLibrary* lib{};
    const PluginInfo*              info{};
    String                         framework_version;  ///< Version the library reported as built with.
    String                         rejection;          ///< Non-empty: why this host refuses the library.

    /// true when the host may read the plugin's metadata.
    bool isUsable() const noexcept { return lib != nullptr && info != nullptr; }
};

/**
 * @brief Loads a plugin library, checks its ABI handshake and reads its metadata.
 *
 * The single place that knows how a Vine plugin is recognized, and the only place
 * that decides whether the metadata may be read at all: the handshake comes first,
 * because PluginInfo is a struct whose layout follows the SDK and misreading it is
 * silent. A file that is not a Vine plugin yields an unusable result without a
 * message (it is skipped like any unrelated library); a library that *looks* like a
 * plugin but cannot be trusted yields a rejection explaining why, with the versions
 * both sides speak, so the fix (rebuild the plugin) is obvious from the log.
 *
 * @param library Plugin library file.
 * @return The library handle, its metadata and the rejection, if any.
 */
QueriedLibrary queryLibrary(const std::filesystem::path& library)
{
    QueriedLibrary result;
    result.lib = vine::runtime::DynamicLibraryLoader::instance().load(String(library.u8string()));
    if (result.lib == nullptr) {
        return result;  // not loadable at all: not a plugin location the host can use
    }

    using AbiFn = const PluginAbi* ();
    const auto      abi      = result.lib->resolveSymbol<AbiFn>(u8"vinePluginAbi");
    const PluginAbi* declared = abi != nullptr ? abi() : nullptr;
    if (declared == nullptr) {
        result.rejection = fromUtf8("Plugin library '" + toUtf8(library) +
                                    "' declares no ABI handshake (vinePluginAbi), so it was built against a framework "
                                    "older than this one. Rebuild the plugin against this framework.");
        return result;
    }
    if (!PluginManager::isPluginAbiCompatible(*declared)) {
        const bool newer = declared->abi_version > V_APPFW_PLUGIN_ABI_VERSION;
        result.rejection = fromUtf8("Plugin library '" + toUtf8(library) + "' declares ABI revision " +
                                    std::to_string(declared->abi_version) + " (built with framework " +
                                    (declared->framework_version != nullptr ? declared->framework_version : "unknown") +
                                    "), which is " + (newer ? "newer" : "older") + " than this host's " +
                                    std::to_string(V_APPFW_PLUGIN_ABI_VERSION) + " (framework " V_APPFW_VERSION +
                                    "). Rebuild the plugin against this framework.");
        return result;
    }
    result.framework_version =
        declared->framework_version != nullptr ? fromUtf8(declared->framework_version) : String{};

    using QueryFn = const PluginInfo* ();
    const auto query = result.lib->resolveSymbol<QueryFn>(u8"vinePluginQuery");
    if (query == nullptr) {
        result.lib = nullptr;  // Not a Vine plugin: no metadata to report, no complaint either.
        return result;
    }
    result.info = query();
    return result;
}

/**
 * @brief Returns the plugin libraries provided by one location.
 *
 * The location may be a library file or a directory, in which case it is scanned
 * one level deep (a per-plugin subdirectory layout is not supported yet). The
 * result is sorted so that the scan order - and therefore which copy of a plugin
 * is discovered first - does not depend on the filesystem iteration order.
 *
 * @param location Library file or directory to scan.
 * @return The library files, in a deterministic order.
 */
std::vector<std::filesystem::path> pluginLibrariesIn(const std::filesystem::path& location)
{
    std::vector<std::filesystem::path> libraries;
    std::error_code                    ec;

    if (std::filesystem::is_regular_file(location, ec)) {
        if (location.extension() == pluginExtension()) {
            libraries.push_back(location);
        }
        return libraries;
    }
    if (!std::filesystem::is_directory(location, ec)) {
        return libraries;
    }

    for (const auto& entry : std::filesystem::directory_iterator(location, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == pluginExtension()) {
            libraries.push_back(entry.path());
        }
    }
    std::sort(libraries.begin(), libraries.end());
    return libraries;
}

/// Resolves a plugin name or library path to a library file.
std::filesystem::path resolvePluginPath(const String& name_or_path)
{
    const std::filesystem::path given(std::u8string_view(name_or_path.data(), name_or_path.size()));
    std::error_code             ec;
    if (given.has_extension() && std::filesystem::exists(given, ec)) {
        return given;
    }
    std::filesystem::path file = given;
    file += pluginExtension();
    return PluginManager::builtInPluginDirectory() / file;
}

/**
 * @brief Tags every command registered while alive with the given plugin name.
 *
 * RAII: restores the previous owner on destruction so the plugin's commands
 * (vinePluginRegisterCommands and the load lifecycle) are attributed to it.
 */
class RegistrationOwnerScope
{
  public:
    RegistrationOwnerScope(CommandManager* manager, const String& owner)
      : manager_(manager)
    {
        if (manager_ != nullptr) {
            manager_->setRegistrationOwner(owner);
        }
    }

    ~RegistrationOwnerScope()
    {
        if (manager_ != nullptr) {
            manager_->setRegistrationOwner({});
        }
    }

    RegistrationOwnerScope(const RegistrationOwnerScope&)            = delete;
    RegistrationOwnerScope& operator=(const RegistrationOwnerScope&) = delete;

  private:
    CommandManager* manager_;
};

/**
 * @brief RAII: keeps a flag set for the duration of an operation.
 *
 * loadAll() uses it to refuse re-entrant calls: a plugin that calls back into the
 * manager from its own lifecycle would interleave with the local batch of
 * instances the outer call is creating, which its rollback path cannot untangle.
 */
class ScopedFlag
{
  public:
    /**
     * @brief Sets the flag; it is cleared when the scope ends.
     *
     * @param flag Flag to hold for the object's lifetime.
     */
    explicit ScopedFlag(bool& flag)
      : flag_(flag)
    {
        flag_ = true;
    }

    ~ScopedFlag() { flag_ = false; }

    ScopedFlag(const ScopedFlag&)            = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;

  private:
    bool& flag_;
};

} // namespace

/**
 * @brief A loaded plugin, its instance and the library it came from.
 */
struct LoadedPlugin {
    String                name;
    Plugin*               plugin;
    std::filesystem::path path;  // Library file the plugin was loaded from.
};

/**
 * @brief A discovered plugin library whose metadata has been queried.
 *
 * The instance is not created yet; only the library is loaded so that
 * vinePluginQuery() can be resolved for dependency resolution.
 */
struct Candidate {
    std::filesystem::path          path;
    vine::runtime::DynamicLibrary* lib;
    const PluginInfo*              info;
};

/// Runs the unload lifecycle of one plugin, swallowing and logging exceptions.
///
/// @param plugin Plugin instance; ignored when nullptr.
/// @param name   Plugin name, for logging.
/// @return true if unload() completed without throwing.
bool unloadOnePlugin(Plugin* plugin, const String& name)
{
    if (plugin == nullptr) {
        return true;
    }

    Application* app = Application::current();
    RegistrationOwnerScope owner_scope(app ? app->commandManager() : nullptr, name);
    PluginLoadContext      context(app, name);
    try {
        plugin->unload(&context);
        return true;
    }
    catch (const std::exception& e) {
        V_LOGE("Plugin '{}' threw while unloading: {}", toUtf8(name), e.what());
    }
    catch (...) {
        V_LOGE("Plugin '{}' threw an unknown exception while unloading", toUtf8(name));
    }
    return false;
}

/// Unloads a batch of plugins, newest first, so a dependent goes before its dependency.
///
/// @param loaded Plugins created by a failed loadAll(); emptied on return.
/// @return true if every unload() completed without throwing.
bool unloadLoadedPlugins(std::vector<LoadedPlugin>& loaded)
{
    bool ok = true;
    while (!loaded.empty()) {
        LoadedPlugin plugin = std::move(loaded.back());
        loaded.pop_back();
        if (unloadOnePlugin(plugin.plugin, plugin.name)) {
            V_LOGI("Plugin '{}' unloaded after a failed load", toUtf8(plugin.name));
        }
        else {
            ok = false;
        }
    }
    return ok;
}

struct PluginManager::Impl {
    /// Loaded plugins, in load order (dependencies before their dependents).
    std::vector<LoadedPlugin> plugins;

    /// Discovered plugins, in scan order of the last loadAll()/load(); not all loaded.
    std::vector<PluginEntry> discovered;

    /// Disabled plugin names, used only when no host config manager exists.
    std::vector<String> disabled_fallback;

    /// Names a registration file disables for everyone (administrator policy).
    /// Filled while scanning, because the policy is a property of the location a
    /// plugin came from.
    std::vector<String> policy_disabled;

    /// Set while loadAll() runs, so a nested call from a plugin lifecycle callback
    /// is refused instead of interleaving with the batch being created.
    bool loading{ false };
};

PluginManager::PluginManager()
  : d(new Impl())
{}

PluginManager::~PluginManager() = default;

void PluginManager::setBuiltInPluginDirectory(const std::filesystem::path& dir)
{
    builtInPluginDirectoryStorage() = dir;
}

std::filesystem::path PluginManager::builtInPluginDirectory()
{
    return builtInPluginDirectoryStorage();
}

void PluginManager::setSkipList(const std::vector<String>& names)
{
    skipListStorage() = names;
}

const std::vector<String>& PluginManager::skipList()
{
    return skipListStorage();
}

void PluginManager::addToSkipList(String name)
{
    skipListStorage().push_back(std::move(name));
}

void PluginManager::removeFromSkipList(const String& name)
{
    auto& list = skipListStorage();
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
}

bool PluginManager::isSkipped(const String& name)
{
    const auto& list = skipListStorage();
    return std::find(list.begin(), list.end(), name) != list.end();
}

const String& PluginManager::disabledConfigKey()
{
    static const String s_key{ u8"plugins.disabled" };
    return s_key;
}

bool PluginManager::isPluginAbiCompatible(const PluginAbi& abi) noexcept
{
    // One rule, one place: anything else would let a path read a layout it was not
    // told it may read. The framework version is diagnostic and deliberately not
    // compared - two builds of the same revision are compatible by construction.
    return abi.abi_version == V_APPFW_PLUGIN_ABI_VERSION;
}

Plugin* PluginManager::load(const String& name_or_path)
{
    const std::filesystem::path path = resolvePluginPath(name_or_path);
    if (path.empty()) {
        return nullptr;
    }

    // Step 1 and 2: load the plugin library (cached by the shared loader), check its
    // ABI handshake and ask it for its metadata.
    const QueriedLibrary queried = queryLibrary(path);
    if (!queried.rejection.empty()) {
        V_LOGW("{}", toUtf8(queried.rejection));
        return nullptr;
    }
    if (!queried.isUsable()) {
        return nullptr;
    }
    const PluginInfo* info = queried.info;

    // The metadata of a plugin that is not loaded is still reported, so a
    // disabled or skipped plugin stays visible in the plugin list.
    rememberDiscovered(d->discovered, *info, path, isBuiltInPath(path) ? PluginScope::BuiltIn : PluginScope::User,
                       queried.framework_version);

    // Reuse an already-loaded plugin with the same name.
    for (const auto& lp : d->plugins) {
        if (lp.name == info->name) {
            return lp.plugin;
        }
    }

    // A disabled plugin, or one the host skips, is never instantiated, not even
    // when its library is named explicitly: the decision applies to every load. The
    // discovery above happened first, so the plugin stays visible in
    // pluginEntries() together with the reason it was refused.
    if (!isPluginEnabled(info->name)) {
        logRefusal(info->name, isPolicyDisabled(info->name, path));
        return nullptr;
    }

    // Unlike loadAll(), an explicit load does not resolve dependencies: load the
    // plugin anyway, but say so, because it may assume its dependencies are
    // already initialized.
    for (const auto& dep : info->dependencies) {
        const bool dep_loaded = std::any_of(d->plugins.begin(), d->plugins.end(),
            [&dep](const LoadedPlugin& lp) { return lp.name == dep; });
        if (!dep_loaded) {
            V_LOGW("Plugin '{}' declares dependency '{}', which is not loaded", toUtf8(info->name), toUtf8(dep));
        }
    }

    // Step 3: create the DLL-global plugin instance and run its lifecycle. The
    // whole block is guarded: a plugin that throws, or that returns no instance,
    // must not be left half-initialized, because a plugin library can be created
    // only once per process and a retry would reuse that same instance. The
    // instance itself belongs to the library (it is never destroyed), so the only
    // way back to a consistent state is running the unload lifecycle.
    using CreateFn = Plugin* ();
    const auto create = queried.lib->resolveSymbol<CreateFn>(u8"vinePluginCreate");
    if (!create) {
        return nullptr;
    }

    Plugin* plugin = nullptr;
    try {
        plugin = create();
        if (plugin == nullptr) {
            return nullptr;
        }

        // The query entry (from V_DECLARE_PLUGIN) is the single metadata source.
        plugin->setInfo(*info);

        // Register the plugin's commands (V_DECLARE_COMMAND) inside its own module:
        // the DLL exports vinePluginRegisterCommands, which runs in the plugin's
        // code and flushes its per-module queue into the CommandManager. The owner
        // scope attributes every command registered during the plugin's load
        // (module commands + lifecycle) to this plugin.
        using RegisterFn = void(CommandManager*);
        const auto register_cmds = queried.lib->resolveSymbol<RegisterFn>(u8"vinePluginRegisterCommands");
        Application* app = Application::current();
        CommandManager* cm = app ? app->commandManager() : nullptr;
        {
            RegistrationOwnerScope owner_scope(cm, info->name);
            if (register_cmds) {
                register_cmds(cm);
            }

            // Three-phase lifecycle, aligned with loadAll(): preLoad(), then load(),
            // then postLoad() for cross-plugin wiring.
            PluginLoadContext context(app, info->name);
            plugin->preLoad(&context);
            plugin->load(&context);
            plugin->postLoad(&context);
        }
    }
    catch (const std::exception& e) {
        V_LOGE("Plugin '{}' threw while loading: {}", toUtf8(info->name), e.what());
        unloadOnePlugin(plugin, info->name);
        return nullptr;
    }
    catch (...) {
        V_LOGE("Plugin '{}' threw an unknown exception while loading", toUtf8(info->name));
        unloadOnePlugin(plugin, info->name);
        return nullptr;
    }

    d->plugins.push_back(LoadedPlugin{ info->name, plugin, path });
    V_LOGI("Plugin '{}' loaded from '{}'", toUtf8(info->name), toUtf8(path));
    return plugin;
}

bool PluginManager::loadAll()
{
    // A plugin that calls loadAll() again from its own preLoad()/load()/postLoad()
    // would interleave with the batch created below, which is local to this call;
    // refuse the nested call instead of leaving half-initialized instances behind.
    if (d->loading) {
        V_LOGW("loadAll() was called from a plugin lifecycle callback; ignoring the nested call");
        return false;
    }
    ScopedFlag loading_scope(d->loading);

    // Discovery starts over on every scan so that plugins added or removed on
    // disk are reflected in pluginEntries().
    d->discovered.clear();

    // Step 1: collect the library files of every configured location. The
    // application's own directory comes first, then the per-user registrations
    // and finally the machine-wide ones, so the first location that provides a
    // plugin name wins and an application-provided plugin is never overridden.
    struct Source {
        std::filesystem::path path;
        PluginScope           scope;
        bool                  registration_enabled; // false: a registration disables it for everyone.
    };

    std::vector<Source> sources;
    sources.push_back(Source{ builtInPluginDirectory(), PluginScope::BuiltIn, true });
    d->policy_disabled.clear();

    // Registrations are read on every scan, so installing or removing a plugin
    // (or editing a registration file by hand) only needs a restart.
    for (const auto& registration : pluginRegistrations()) {
        if (!registration.enabled) {
            V_LOGI("Plugin registration '{}' disables its plugin for all users", toUtf8(registration.id));
        }
        sources.push_back(Source{ std::filesystem::path(std::u8string_view(registration.path.data(), registration.path.size())),
                                  registration.scope, registration.enabled });
    }

    // Step 2: query the metadata of every unfiltered, valid Vine plugin. No
    // instance is created yet; only the library is loaded so that
    // vinePluginQuery() can be resolved. A plugin provided by several locations
    // is discovered once, from the first one.
    std::vector<Candidate> candidates;
    for (const auto& source : sources) {
        const auto found = pluginLibrariesIn(source.path);
        if (found.empty()) {
            if (source.scope != PluginScope::BuiltIn) {
                V_LOGW("Registered plugin location '{}' does not exist or holds no plugin library", source.path.string());
            }
            continue;
        }

        for (const auto& path : found) {
            const QueriedLibrary queried = queryLibrary(path);
            if (!queried.rejection.empty()) {
                // The one case worth a warning per scan: the library is a plugin, but
                // this host cannot read it. Everything else here is silence.
                V_LOGW("{}", toUtf8(queried.rejection));
                continue;
            }
            if (!queried.isUsable()) {
                continue;  // not a loadable Vine plugin
            }
            const PluginInfo* info = queried.info;

            // Discovery is independent of loading: a disabled plugin, or one the
            // host skips, is still reported (metadata + library path) so a UI can
            // show it and explain why it is not running.
            rememberDiscovered(d->discovered, *info, path, source.scope, queried.framework_version);

            // A registration that disables the plugin for every user is policy:
            // record it before the enabled check below, which reads it back.
            const bool policy_disables = !source.registration_enabled;
            if (policy_disables) {
                d->policy_disabled.push_back(info->name);
            }

            const bool already_loaded = std::any_of(d->plugins.begin(), d->plugins.end(),
                [&info](const LoadedPlugin& lp) { return lp.name == info->name; });
            if (already_loaded) {
                continue;
            }

            // The same plugin can be provided by several locations (the
            // application directory, a per-user registration, a machine-wide
            // registration): load it once, from the first location.
            const bool already_candidate = std::any_of(candidates.begin(), candidates.end(),
                [&info](const Candidate& c) { return c.info->name == info->name; });
            if (already_candidate) {
                continue;
            }

            // Disabled plugins (by the user, by a registration that disables them
            // for every user, or by the host's skip list) are not instantiated and
            // do not take part in dependency resolution; a dependent is therefore
            // reported as unresolved instead of being loaded with a missing
            // dependency.
            if (!isPluginEnabled(info->name)) {
                logRefusal(info->name, policy_disables);
                continue;
            }

            candidates.push_back(Candidate{ path, queried.lib, info });
        }
    }

    // Step 3: dependency resolution, computed as a fixpoint: "can be loaded" is not a
    // property of one plugin but of the whole set, because a plugin whose dependency
    // cannot be loaded cannot be loaded either - and neither can anything below it.
    //
    // Doing it that way answers three questions with one computation:
    //
    // - the load order falls out of it: a candidate joins the set only once all its
    //   dependencies are already in it, so no separate topological sort is needed;
    // - the report can name *every* plugin that will not be loaded, with the reason
    //   (its own, or the dependency that blocks it) instead of only the first level;
    // - everything else still loads. One third-party plugin with a missing or
    //   disabled dependency must not cost the whole application its shell; the
    //   caller is told what was left out through the return value.
    std::vector<const Candidate*> planned;
    planned.reserve(candidates.size());
    std::vector<String> satisfiable;
    satisfiable.reserve(d->plugins.size() + candidates.size());
    for (const auto& lp : d->plugins) {
        satisfiable.push_back(lp.name);  // already loaded: a satisfied dependency
    }
    const auto isSatisfiable = [&satisfiable](const String& name) {
        return std::find(satisfiable.begin(), satisfiable.end(), name) != satisfiable.end();
    };
    for (bool progressed = true; progressed;) {
        progressed = false;
        for (const auto& c : candidates) {
            if (isSatisfiable(c.info->name)) {
                continue;
            }
            if (std::all_of(c.info->dependencies.begin(), c.info->dependencies.end(), isSatisfiable)) {
                planned.push_back(&c);  // its dependencies are in the set: it may join it
                satisfiable.push_back(c.info->name);
                progressed = true;
            }
        }
    }

    // Everything outside the closure cannot be loaded. Each of those is reported with
    // its own reason, so the reader can tell "install it", "enable it" and "stop
    // skipping it" apart - and can follow a chain, because the dependency that blocks
    // a plugin is itself reported on its own line.
    struct LoadProblem {
        String              plugin;
        std::vector<String> missing;
        std::vector<String> disabled;
        std::vector<String> skipped;
        std::vector<String> blocked;
    };
    std::vector<LoadProblem> problems;
    for (const auto& c : candidates) {
        if (isSatisfiable(c.info->name)) {
            continue;  // planned
        }
        LoadProblem problem{ c.info->name };
        for (const auto& dep : c.info->dependencies) {
            if (isSatisfiable(dep)) {
                continue;
            }
            if (isSkipped(dep)) {
                problem.skipped.push_back(dep);
            }
            else if (const PluginEntry* entry = findDiscovered(d->discovered, dep);
                     entry != nullptr && !isPluginEnabled(dep)) {
                problem.disabled.push_back(dep);
            }
            else if (findDiscovered(d->discovered, dep) != nullptr) {
                problem.blocked.push_back(dep);  // discovered and enabled, but not loadable
            }
            else {
                problem.missing.push_back(dep);
            }
        }
        problems.push_back(std::move(problem));
    }

    // A cluster whose members only depend on each other is a declared cycle; saying so
    // beats leaving the reader with mutual "not loadable" lines.
    const bool cyclic = !problems.empty() && std::all_of(problems.begin(), problems.end(), [&problems](const LoadProblem& problem) {
        return std::all_of(problem.blocked.begin(), problem.blocked.end(), [&problems](const String& blocked) {
            return std::any_of(problems.begin(), problems.end(), [&blocked](const LoadProblem& other) { return other.plugin == blocked; });
        }) && problem.missing.empty() && problem.disabled.empty() && problem.skipped.empty();
    });

    if (!problems.empty()) {
        std::string report = "Plugin dependency resolution: " + std::to_string(problems.size()) +
                             " plugin(s) will not be loaded (the rest still loads):";
        for (const auto& problem : problems) {
            report += "\n  Plugin '";
            report += toUtf8(problem.plugin);
            report += "':";
            for (const auto& dep : problem.missing) {
                report += "\n    - missing dependency: ";
                report += toUtf8(dep);
            }
            for (const auto& dep : problem.disabled) {
                report += "\n    - disabled dependency: ";
                report += toUtf8(dep);
            }
            for (const auto& dep : problem.skipped) {
                report += "\n    - skipped by the host: ";
                report += toUtf8(dep);
            }
            for (const auto& dep : problem.blocked) {
                report += "\n    - dependency not loadable: ";
                report += toUtf8(dep);
            }
        }
        if (cyclic) {
            report += "\n  These plugins depend on each other in a cycle.";
        }
        V_LOGE("{}", report);
    }

    // Step 4: create the instances in the order the closure produced, and step 5: run
    // the lifecycle. Both are wrapped so that a throwing plugin, or a library without
    // a usable create entry point, cannot leave instances behind that the manager
    // does not know about: a plugin library can be created only once per process,
    // so a retry would reuse the same, half-initialized instance.
    std::vector<LoadedPlugin> created;
    created.reserve(planned.size());
    try {
        for (const Candidate* candidate : planned) {
            const String& name = candidate->info->name;
            using CreateFn = Plugin* ();
            const auto create = candidate->lib->resolveSymbol<CreateFn>(u8"vinePluginCreate");
            if (!create) {
                V_LOGE("Plugin '{}' has no create entry point", toUtf8(name));
                unloadLoadedPlugins(created);
                return false;
            }
            Plugin* plugin = create();
            if (!plugin) {
                V_LOGE("Plugin '{}' failed to create its instance", toUtf8(name));
                unloadLoadedPlugins(created);
                return false;
            }

            // The query entry (from V_DECLARE_PLUGIN) is the single metadata source.
            plugin->setInfo(*candidate->info);

            // Register the plugin's commands (V_DECLARE_COMMAND) inside its own module.
            // The owner scope tags these module commands with the plugin name.
            using RegisterFn = void(CommandManager*);
            const auto register_cmds = candidate->lib->resolveSymbol<RegisterFn>(u8"vinePluginRegisterCommands");
            Application* app = Application::current();
            CommandManager* cm = app ? app->commandManager() : nullptr;
            {
                RegistrationOwnerScope owner_scope(cm, name);
                if (register_cmds) {
                    register_cmds(cm);
                }
            }

            V_LOGI("Plugin '{}' loaded", toUtf8(name));
            created.push_back(LoadedPlugin{ name, plugin, candidate->path });
        }

        // Step 5: three-phase lifecycle - preLoad() for every plugin (each one
        // registers its own commands), then load() for every plugin, then
        // postLoad() for every plugin. Each plugin gets its own context so it can
        // query pluginName() and its own registered configs. Every phase runs
        // inside a per-plugin owner scope so lifecycle-registered commands are
        // attributed to the plugin.
        for (const auto& lp : created) {
            RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
            PluginLoadContext context(Application::current(), lp.name);
            lp.plugin->preLoad(&context);
        }
        for (const auto& lp : created) {
            RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
            PluginLoadContext context(Application::current(), lp.name);
            lp.plugin->load(&context);
        }
        for (const auto& lp : created) {
            RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
            PluginLoadContext context(Application::current(), lp.name);
            lp.plugin->postLoad(&context);
        }
    }
    catch (const std::exception& e) {
        V_LOGE("Plugin loading failed, releasing the instances created by this call: {}", e.what());
        unloadLoadedPlugins(created);
        return false;
    }
    catch (...) {
        V_LOGE("Plugin loading failed with an unknown exception, releasing the instances created by this call");
        unloadLoadedPlugins(created);
        return false;
    }

    // Register the created plugins (in dependency order).
    for (auto& lp : created) {
        d->plugins.push_back(std::move(lp));
    }
    // The batch is loaded; plugins left out because their dependencies cannot be
    // satisfied make the call report false, without having cost the rest anything.
    return problems.empty();
}

bool PluginManager::unloadAll()
{
    if (d->plugins.empty()) {
        return true;
    }

    // The unload order is derived from the declared dependencies, not from the
    // position in the load list: load() appends a single plugin without
    // resolving dependencies, so a dependency can sit behind its dependent.
    std::vector<PluginEntry> entries;
    entries.reserve(d->plugins.size());
    for (const auto& lp : d->plugins) {
        // unloadOrder() only reads the metadata and the loaded flag; the scope and
        // the remaining state flags are placeholders.
        entries.push_back(PluginEntry{ .info  = lp.plugin->info(),
                                       .path  = lp.path,
                                       .scope = PluginScope::BuiltIn,
                                       .loaded = true });
    }

    bool ok = true;
    for (const auto& name : unloadOrder(entries)) {
        const auto it = std::find_if(d->plugins.begin(), d->plugins.end(),
            [&name](const LoadedPlugin& lp) { return lp.name == name; });
        if (it == d->plugins.end()) {
            continue; // Already gone (a plugin unloaded a sibling re-entrantly).
        }

        // The entry is dropped before unload() so that a plugin calling back into
        // the manager while unloading already sees itself as unloaded, and a
        // re-entrant unloadAll() cannot invoke unload() twice on one instance.
        LoadedPlugin loaded = std::move(*it);
        d->plugins.erase(it);

        if (unloadOnePlugin(loaded.plugin, loaded.name)) {
            V_LOGI("Plugin '{}' unloaded", toUtf8(loaded.name));
        }
        else {
            // A throwing plugin must not stop the others from unloading.
            ok = false;
        }
    }
    return ok;
}

std::vector<String> PluginManager::unloadOrder(const std::vector<PluginEntry>& entries)
{
    // Only loaded plugins are unloaded; keep the caller's (discovery) order as
    // the tie-breaker so the result is deterministic.
    std::vector<const PluginEntry*> remaining;
    remaining.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.loaded) {
            remaining.push_back(&entry);
        }
    }

    std::vector<String> order;
    order.reserve(remaining.size());
    while (!remaining.empty()) {
        // The first plugin no other remaining plugin depends on: unload it now,
        // because nothing that is still loaded needs it before going away.
        const auto it = std::find_if(remaining.begin(), remaining.end(), [&remaining](const PluginEntry* candidate) {
            return std::none_of(remaining.begin(), remaining.end(), [candidate](const PluginEntry* other) {
                return other != candidate && dependsOn(*other, candidate->info.name);
            });
        });
        if (it == remaining.end()) {
            // Declared cycle: reachable only through explicit load() calls (loadAll()
            // rejects cycles), and 'dependents first' has no solution for it. Fall
            // back to the entry order so the set is still unloaded completely.
            for (const auto* entry : remaining) {
                order.push_back(entry->info.name);
            }
            break;
        }
        order.push_back((*it)->info.name);
        remaining.erase(it);
    }
    return order;
}

Plugin* PluginManager::plugin(const String& name) const
{
    for (const auto& lp : d->plugins) {
        if (lp.name == name) {
            return lp.plugin;
        }
    }
    return nullptr;
}

bool PluginManager::isLoaded(const String& name) const
{
    return plugin(name) != nullptr;
}

std::size_t PluginManager::count() const
{
    return d->plugins.size();
}

std::filesystem::path PluginManager::libraryPath(const String& name) const
{
    for (const auto& lp : d->plugins) {
        if (lp.name == name) {
            return lp.path;
        }
    }
    if (const PluginEntry* entry = findDiscovered(d->discovered, name); entry != nullptr) {
        return entry->path;
    }
    return {};
}

std::vector<PluginEntry> PluginManager::pluginEntries() const
{
    // The four state inputs are snapshotted once, so a list of n plugins costs one
    // config read instead of n (a UI rebuilds this list on every refresh and every
    // selection change) and the reported state is consistent across the list.
    const std::vector<String> disabled = disabledNames(d->disabled_fallback);

    std::vector<String> loaded;
    loaded.reserve(d->plugins.size());
    for (const auto& lp : d->plugins) {
        loaded.push_back(lp.name);
    }

    std::vector<PluginEntry> entries = d->discovered;
    for (auto& entry : entries) {
        const String& name    = entry.info.name;
        const bool    skipped = isSkipped(name);
        const bool    policy  = std::find(d->policy_disabled.begin(), d->policy_disabled.end(), name) != d->policy_disabled.end();

        // State is resolved at query time so the entries cannot go stale: the skip
        // list lives outside the manager, the preference in the config, the
        // instance in the loaded list. skipped is reported next to enabled so a UI
        // can tell "the host refuses it" from "the user turned it off".
        entry.skipped = skipped;
        entry.enabled = resolveEnabled(name, entry.scope == PluginScope::BuiltIn, skipped, policy, disabled);
        entry.loaded  = std::find(loaded.begin(), loaded.end(), name) != loaded.end();
    }

    // The directory iteration order is unspecified; sort by name so the plugin
    // list a UI builds from this is stable across platforms.
    std::sort(entries.begin(), entries.end(), [](const PluginEntry& lhs, const PluginEntry& rhs) {
        return lhs.info.name < rhs.info.name;
    });
    return entries;
}

bool PluginManager::isPluginEnabled(const String& name) const
{
    // The same four inputs pluginEntries() snapshots for a whole list; here they
    // are read for one name, so the answer is always current.
    const PluginEntry* entry    = findDiscovered(d->discovered, name);
    const bool         built_in = entry != nullptr && entry->scope == PluginScope::BuiltIn;
    const bool         skipped  = isSkipped(name);
    const bool         policy   = isPolicyDisabled(name, {});
    // The per-user preference comes from the host ConfigManager when there is one,
    // and from the process-local list otherwise.
    const std::vector<String> disabled = disabledNames(d->disabled_fallback);

    return resolveEnabled(name, built_in, skipped, policy, disabled);
}

bool PluginManager::isPolicyDisabled(const String& name, const std::filesystem::path& library) const
{
    if (std::find(d->policy_disabled.begin(), d->policy_disabled.end(), name) != d->policy_disabled.end()) {
        return true;  // recorded by the scan that discovered the plugin
    }

    // Not known yet: no scan has run, or the plugin was named explicitly. Reading the
    // registrations here costs one directory listing and keeps the policy absolute.
    const std::filesystem::path loaded = library.empty() ? std::filesystem::path{} : normalizedPath(library);
    for (const auto& registration : pluginRegistrations()) {
        if (registration.enabled) {
            continue;
        }
        if (!registration.name.empty() && registration.name == name) {
            return true;
        }
        if (loaded.empty() || registration.path.empty()) {
            continue;
        }
        // A registration names the library itself or the directory holding it.
        const std::filesystem::path registered =
            normalizedPath(std::filesystem::path(std::u8string_view(registration.path.data(), registration.path.size())));
        if (registered == loaded || registered == loaded.parent_path()) {
            return true;
        }
    }
    return false;
}

bool PluginManager::setPluginEnabled(const String& name, bool enabled)
{
    if (name.empty()) {
        V_LOGW("Cannot enable or disable a plugin without a name");
        return false;
    }

    if (const PluginEntry* entry = findDiscovered(d->discovered, name);
        entry != nullptr && entry->scope == PluginScope::BuiltIn) {
        V_LOGW("Plugin '{}' is provided by the application and cannot be disabled; ask the application to stop shipping it", toUtf8(name));
        return false;
    }

    if (isPolicyDisabled(name, {}) && enabled) {
        // Storing the preference is fine, but the policy keeps winning; saying so
        // avoids a UI that looks broken.
        V_LOGW("Plugin '{}' is disabled for all users by its registration file; enabling it here has no effect", toUtf8(name));
    }

    if (isSkipped(name) && enabled) {
        // The host's skip list wins the same way; the preference is still stored,
        // so it applies as soon as the host stops skipping the plugin.
        V_LOGW("Plugin '{}' is skipped by the host; enabling it here has no effect until the host stops skipping it", toUtf8(name));
    }

    if (ConfigManager* cfg = hostConfigManager(); cfg != nullptr) {
        std::vector<String> disabled = userDisabledFromConfig();
        const auto          it       = std::find(disabled.begin(), disabled.end(), name);
        if (enabled) {
            if (it == disabled.end()) {
                return true; // Already enabled.
            }
            disabled.erase(it);
        }
        else {
            if (it != disabled.end()) {
                return true; // Already disabled.
            }
            disabled.push_back(name);
        }
        cfg->setStringArray(disabledConfigKey(), disabled);
        V_LOGI("Plugin '{}' is now {}; the change takes effect at the next start", toUtf8(name),
               enabled ? "enabled" : "disabled");
        return true;
    }

    // No config manager: the preference only lives for this process.
    const auto it = std::find(d->disabled_fallback.begin(), d->disabled_fallback.end(), name);
    if (enabled) {
        if (it != d->disabled_fallback.end()) {
            d->disabled_fallback.erase(it);
        }
    }
    else if (it == d->disabled_fallback.end()) {
        d->disabled_fallback.push_back(name);
    }
    return true;
}

std::vector<PluginRegistration> PluginManager::pluginRegistrations() const
{
    std::vector<PluginRegistration> registrations;
    const auto                      directories = registrationDirectories();
    for (std::size_t i = 0; i < directories.size(); ++i) {
        // The per-user directory comes first, so its registrations win when the
        // same plugin is registered for one user and for everyone.
        const PluginScope scope = i == 0 ? PluginScope::User : PluginScope::AllUsers;
        auto              found = registrationsIn(directories[i], scope);
        registrations.insert(registrations.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return registrations;
}

String PluginManager::installPlugin(const String& path, PluginScope scope)
{
    if (path.empty()) {
        V_LOGW("Cannot register an empty plugin location");
        return {};
    }
    if (scope == PluginScope::BuiltIn) {
        V_LOGW("Refusing to register '{}' as an application-provided plugin; what ships with the application is not installed by the user",
               toUtf8(path));
        return {};
    }

    // An absolute, lexically normal path is stored: the registration is read at the
    // next start, from a different working directory, so a relative path would stop
    // resolving even though it is valid right now. Symlinks are deliberately kept as
    // given - the registration records where the user pointed, not what it resolves to.
    std::error_code             ec;
    const std::filesystem::path location =
        std::filesystem::absolute(std::filesystem::path(std::u8string_view(path.data(), path.size())), ec).lexically_normal();
    if (!std::filesystem::exists(location, ec)) {
        // The registration is a path, so registering a missing one would only
        // produce a warning on every later start.
        V_LOGW("Cannot register plugin location '{}': it does not exist", toUtf8(location));
        return {};
    }

    // The registration directory to write into: the per-user one, or the first
    // system directory that exists (writing there needs administrator rights).
    const auto directories = registrationDirectories();
    if (directories.empty()) {
        V_LOGW("Cannot register plugin location '{}': the application has no registration directory", toUtf8(location));
        return {};
    }

    std::filesystem::path directory = directories.front();
    if (scope == PluginScope::AllUsers) {
        // The first system directory that can be written to wins; a normal user
        // has none, which is the signal to fall back to the per-user scope.
        bool                          found = false;
        const std::vector<std::filesystem::path> system =
            Application::current() ? Application::current()->allUsersPluginRegistrationDirectories()
                                   : std::vector<std::filesystem::path>{};
        for (const auto& candidate : system) {
            if (std::filesystem::is_directory(candidate, ec) || std::filesystem::create_directories(candidate, ec)) {
                directory = candidate;
                found     = true;
                break;
            }
        }
        if (!found) {
            V_LOGW("Cannot register plugin location '{}' for all users: no writable system registration directory", toUtf8(location));
            return {};
        }
    }

    // Name the file after the plugin when the location holds exactly one, so the
    // registration is readable; otherwise after the location. The id is only an
    // identifier: the plugin's identity is the UUID and name in its library.
    PluginRegistration registration;
    registration.path    = String(location.u8string());
    registration.enabled = true;

    auto libraries = pluginLibrariesIn(location);
    if (libraries.size() == 1) {
        const QueriedLibrary queried = queryLibrary(*libraries.begin());
        if (!queried.rejection.empty()) {
            // Registering it would only produce that warning on every later start.
            V_LOGW("{}", toUtf8(queried.rejection));
            return {};
        }
        if (queried.info != nullptr) {
            registration.name = queried.info->name;
            registration.uuid = queried.info->uuid;
        }
    }

    // The id doubles as the file name inside the registration directory, so it has
    // to be a plain file name: a library file contributes its stem, a directory its
    // own name, and a path that ends with a separator is reduced to its parent.
    std::filesystem::path named = location;
    if (named.filename().empty()) {
        named = named.parent_path();
    }
    const std::filesystem::path base = named.extension() == pluginExtension() ? named.stem() : named.filename();
    const String                id   = !registration.name.empty() ? registration.name : String(base.u8string());
    if (!isUsableRegistrationId(id)) {
        V_LOGW("Cannot register plugin location '{}': '{}' cannot be used as a registration file name", toUtf8(location), toUtf8(id));
        return {};
    }

    std::filesystem::path target = directory / std::filesystem::path(std::u8string_view(id.data(), id.size()));
    target += s_registration_extension;

    if (!writeRegistrationFile(target, registration)) {
        return {};
    }

    V_LOGI("Plugin location '{}' registered as '{}' in '{}'; it is scanned at the next start", toUtf8(location), toUtf8(id), toUtf8(directory));
    return id;
}

bool PluginManager::uninstallPlugin(const String& id, PluginScope scope)
{
    // The id is a file name inside the registration directory; refuse anything that
    // could reach outside it (see installPlugin()).
    if (!isUsableRegistrationId(id)) {
        V_LOGW("Cannot uninstall plugin registration '{}': it is not a usable registration id", toUtf8(id));
        return false;
    }

    const auto directories = registrationDirectories();
    if (directories.empty()) {
        return false;
    }

    // The per-user directory is the first one; the system directories follow it.
    std::vector<std::filesystem::path> candidates;
    if (scope == PluginScope::AllUsers) {
        // An AllUsers registration lives in a system directory, so only those are
        // searched: a per-user file with the same id is a different registration.
        candidates.assign(directories.begin() + 1, directories.end());
    }
    else {
        candidates.push_back(directories.front());
    }

    for (const auto& directory : candidates) {
        std::filesystem::path file = directory / std::filesystem::path(std::u8string_view(id.data(), id.size()));
        file += s_registration_extension;

        std::error_code ec;
        if (std::filesystem::remove(file, ec)) {
            V_LOGI("Plugin registration '{}' removed; it is no longer scanned at the next start", toUtf8(file));
            return true;
        }
    }
    return false;
}

std::vector<String> PluginManager::names() const
{
    std::vector<String> result;
    result.reserve(d->plugins.size());
    for (const auto& lp : d->plugins) {
        result.push_back(lp.name);
    }
    return result;
}

std::vector<Plugin*> PluginManager::plugins() const
{
    std::vector<Plugin*> result;
    result.reserve(d->plugins.size());
    for (const auto& lp : d->plugins) {
        result.push_back(lp.plugin);
    }
    return result;
}

std::vector<CommandInfo> PluginManager::commandInfosForPlugin(const String& name) const
{
    auto* instance = plugin(name);
    return instance ? instance->commandInfos() : std::vector<CommandInfo>{};
}

std::vector<const ConfigItem*> PluginManager::configItemsForPlugin(const String& name) const
{
    auto* instance = plugin(name);
    return instance ? instance->configItems() : std::vector<const ConfigItem*>{};
}

V_APPFW_NS_END
