#include <vine/appfw/PluginManager.hpp>

#ifdef VN_CC_MSVC
#    include <Windows.h>
#endif // VN_CC_MSVC

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/logging/Log.hpp>
#include <vine/runtime/DynamicLibraryLoader.hpp>

VN_APPFW_NS_BEGIN

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
        VN_LOGI("Plugin '{}' is skipped by the host; not loading it", toUtf8(name));
    }
    else if (policy_disables) {
        VN_LOGW("Plugin '{}' is disabled for all users by its registration file; not loading it", toUtf8(name));
    }
    else {
        VN_LOGW("Plugin '{}' is disabled; not loading it", toUtf8(name));
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
            VN_LOGW("Two different plugins are named '{}': keeping '{}', ignoring '{}'",
                   toUtf8(info.name), toUtf8(known->path), toUtf8(path));
        }
        return;
    }

    // The other way round: two different names claiming one identity. The uuid is hardcoded by the
    // plugin (VN_DECLARE_PLUGIN), so the only way this happens is a copy-paste that forgot to change
    // it - which means two libraries are really the same plugin under two names. Both stay in the
    // list (a human has to see them to fix it) and the log names both, because nothing else would
    // ever tell: the names differ, so every other check here passes.
    if (!info.uuid.isNull()) {
        const auto same_identity = std::find_if(discovered.begin(), discovered.end(),
            [&info](const PluginEntry& entry) { return entry.info.uuid == info.uuid; });
        if (same_identity != discovered.end()) {
            VN_LOGW("Two plugins declare the same identity: '{}' at '{}' and '{}' at '{}' both report uuid '{}'; "
                    "a plugin's uuid must be its own",
                   toUtf8(same_identity->info.name), toUtf8(same_identity->path), toUtf8(info.name), toUtf8(path),
                   toUtf8(info.uuid.toString()));
        }
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
constexpr const char* kRegistrationExtension = ".plugin";

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
        VN_LOGW("Cannot read plugin registration '{}'", toUtf8(file));
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
            VN_LOGW("Plugin registration '{}' has a malformed line: '{}'", toUtf8(file), unpadded);
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
            VN_LOGI("Plugin registration '{}' has an unknown key '{}'", toUtf8(file), key);
        }
    }

    if (registration.path.empty()) {
        VN_LOGW("Plugin registration '{}' has no 'path'; ignoring it", toUtf8(file));
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
        if (entry.is_regular_file(ec) && entry.path().extension() == kRegistrationExtension) {
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
            VN_LOGW("Cannot write the plugin registration '{}'", toUtf8(temporary));
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
        VN_LOGW("Cannot create the plugin registration '{}'", toUtf8(target));
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
    vn::runtime::DynamicLibrary* lib{};
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
    result.lib = vn::runtime::DynamicLibraryLoader::instance().load(String(library.u8string()));
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
        const bool newer = declared->abi_version > VN_APPFW_PLUGIN_ABI_VERSION;
        result.rejection = fromUtf8("Plugin library '" + toUtf8(library) + "' declares ABI revision " +
                                    std::to_string(declared->abi_version) + " (built with framework " +
                                    (declared->framework_version != nullptr ? declared->framework_version : "unknown") +
                                    "), which is " + (newer ? "newer" : "older") + " than this host's " +
                                    std::to_string(VN_APPFW_PLUGIN_ABI_VERSION) + " (framework " VN_APPFW_VERSION +
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
    vn::runtime::DynamicLibrary* lib;
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
        VN_LOGE("Plugin '{}' threw while unloading: {}", toUtf8(name), e.what());
    }
    catch (...) {
        VN_LOGE("Plugin '{}' threw an unknown exception while unloading", toUtf8(name));
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
            VN_LOGI("Plugin '{}' unloaded after a failed load", toUtf8(plugin.name));
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
    return abi.abi_version == VN_APPFW_PLUGIN_ABI_VERSION;
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
        VN_LOGW("{}", toUtf8(queried.rejection));
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
            VN_LOGW("Plugin '{}' declares dependency '{}', which is not loaded", toUtf8(info->name), toUtf8(dep));
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

        // The query entry (from VN_DECLARE_PLUGIN) is the single metadata source.
        plugin->setInfo(*info);

        // Register the plugin's commands (VN_DECLARE_COMMAND) inside its own module:
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
            // then postLoad() for cross-plugin wiring. The hooks are coroutines, and this is the synchronous door: it
            // drives each of them to completion on this thread, delivering what they post to the application thread
            // (MainThreadDispatcher::runToCompletion(), over vn::async::runToCompletion()) so a plugin that runs its
            // heavy half on the pool still loads here. This door does not turn the event loop otherwise - loadAllAsync()
            // is the one the boot uses.
            PluginLoadContext context(app, info->name);
            static_cast<void>(MainThreadDispatcher::runToCompletion(plugin->preLoad(&context)));
            static_cast<void>(MainThreadDispatcher::runToCompletion(plugin->load(&context)));
            static_cast<void>(MainThreadDispatcher::runToCompletion(plugin->postLoad(&context)));
        }
    }
    catch (const std::exception& e) {
        VN_LOGE("Plugin '{}' threw while loading: {}", toUtf8(info->name), e.what());
        unloadOnePlugin(plugin, info->name);
        return nullptr;
    }
    catch (...) {
        VN_LOGE("Plugin '{}' threw an unknown exception while loading", toUtf8(info->name));
        unloadOnePlugin(plugin, info->name);
        return nullptr;
    }

    d->plugins.push_back(LoadedPlugin{ info->name, plugin, path });
    VN_LOGI("Plugin '{}' loaded from '{}'", toUtf8(info->name), toUtf8(path));
    return plugin;
}

namespace
{

/// What one load pass may load, and whether the plan covers everything it found.
struct LoadPlan {
    std::vector<const Candidate*> planned;           ///< In dependency order; points into the pass's candidate list.
    bool                          complete{ true };  ///< false: something was left out (its reason is already logged).
};

/**
 * @brief Finds the plugins a load pass may load: every configured location, scanned and filtered.
 *
 * Step 1 and 2 of a load (see loadAllAsync()): the application's own directory comes first, then the
 * per-user registrations and finally the machine-wide ones, so the first location that provides a
 * name wins and an application-provided plugin is never overridden. Every library that answers the
 * ABI handshake is remembered in @p discovered - loadable or not, because a UI reports the disabled
 * ones too - and the loadable ones come back as candidates.
 *
 * @param manager         Manager whose enable decision and registrations are used.
 * @param discovered      Discovery list; it is cleared and refilled here.
 * @param policy_disabled Names a registration disables for everyone; cleared and refilled here.
 * @param loaded          Plugins already loaded; they are no candidates again.
 * @return The candidates, in scan order.
 */
std::vector<Candidate> collectCandidates(const PluginManager& manager, std::vector<PluginEntry>& discovered,
                                         std::vector<String>& policy_disabled, const std::vector<LoadedPlugin>& loaded)
{
    discovered.clear();
    policy_disabled.clear();

    // Step 1: the library files of every configured location.
    struct Source {
        std::filesystem::path     path;
        PluginScope               scope;
        bool                      registration_enabled;  // false: a registration disables it for everyone.
        const PluginRegistration* registration{};        // the file this source came from; null for the application's directory.
    };

    std::vector<Source>         sources;
    sources.push_back(Source{ PluginManager::builtInPluginDirectory(), PluginScope::BuiltIn, true, nullptr });

    // Registrations are read on every scan, so installing or removing a plugin (or editing a
    // registration file by hand) only needs a restart.
    const std::vector<PluginRegistration> registrations = manager.pluginRegistrations();
    for (const auto& registration : registrations) {
        if (!registration.enabled) {
            VN_LOGI("Plugin registration '{}' disables its plugin for all users", toUtf8(registration.id));
        }
        sources.push_back(Source{ std::filesystem::path(std::u8string_view(registration.path.data(), registration.path.size())),
                                  registration.scope, registration.enabled, &registration });
    }

    // Step 2: the metadata of every unfiltered, valid Vine plugin. No instance is created yet; only
    // the library is loaded, so that vinePluginQuery() can be resolved.
    std::vector<Candidate> candidates;
    for (const auto& source : sources) {
        const auto found = pluginLibrariesIn(source.path);
        if (found.empty()) {
            if (source.scope != PluginScope::BuiltIn) {
                VN_LOGW("Registered plugin location '{}' does not exist or holds no plugin library", source.path.string());
            }
            continue;
        }

        for (const auto& path : found) {
            const QueriedLibrary queried = queryLibrary(path);
            if (!queried.rejection.empty()) {
                // The one case worth a warning per scan: the library is a plugin, but this host cannot read it.
                VN_LOGW("{}", toUtf8(queried.rejection));
                continue;
            }
            if (!queried.isUsable()) {
                continue;  // not a loadable Vine plugin
            }
            const PluginInfo* info = queried.info;

            // A registration that points at one library also records that plugin's name and identity.
            // The library is the authority (see the design doc), so a file that disagrees is reported
            // and the plugin still loads under what its own metadata says - the point of the warning is
            // to tell a human that the file (or the library) is wrong, not to refuse the plugin. Only
            // single-library registrations are compared: that is what installPlugin() writes name and
            // uuid for, and a directory registration says nothing about the plugins inside it.
            if (source.registration != nullptr && found.size() == 1) {
                if (!source.registration->name.empty() && source.registration->name != info->name) {
                    VN_LOGW("Plugin registration '{}' names plugin '{}', but the library at '{}' reports '{}'; the library wins",
                           toUtf8(source.registration->id), toUtf8(source.registration->name), toUtf8(path), toUtf8(info->name));
                }
                if (!source.registration->uuid.isNull() && !info->uuid.isNull() && source.registration->uuid != info->uuid) {
                    VN_LOGW("Plugin registration '{}' records uuid '{}', but the library at '{}' reports '{}'; the library wins",
                           toUtf8(source.registration->id), toUtf8(source.registration->uuid.toString()), toUtf8(path),
                           toUtf8(info->uuid.toString()));
                }
            }

            // Discovery is independent of loading: a disabled plugin, or one the host skips, is still
            // reported (metadata + library path) so a UI can show it and explain why it is not running.
            rememberDiscovered(discovered, *info, path, source.scope, queried.framework_version);

            // A registration that disables the plugin for every user is policy: record it before the
            // enabled check below, which reads it back.
            const bool policy_disables = !source.registration_enabled;
            if (policy_disables) {
                policy_disabled.push_back(info->name);
            }

            const bool already_loaded = std::any_of(loaded.begin(), loaded.end(),
                [&info](const LoadedPlugin& lp) { return lp.name == info->name; });
            if (already_loaded) {
                continue;
            }

            // The same plugin can be provided by several locations (the application directory, a
            // per-user registration, a machine-wide registration): load it once, from the first one.
            const bool already_candidate = std::any_of(candidates.begin(), candidates.end(),
                [&info](const Candidate& c) { return c.info->name == info->name; });
            if (already_candidate) {
                continue;
            }

            // Disabled plugins (by the user, by a registration, or by the host's skip list) are not
            // instantiated and do not take part in dependency resolution; a dependent is therefore
            // reported as unresolved instead of being loaded with a missing dependency.
            if (!manager.isPluginEnabled(info->name)) {
                logRefusal(info->name, policy_disables);
                continue;
            }

            candidates.push_back(Candidate{ path, queried.lib, info });
        }
    }
    return candidates;
}

/**
 * @brief Resolves which candidates may be loaded, in which order, and reports the rest.
 *
 * Step 3 of a load (see loadAllAsync()). "Can be loaded" is not a property of one plugin but of the
 * whole set, so it is computed as a fixpoint: a candidate joins the batch once every dependency of it
 * is already loaded or already in the batch. One computation answers three questions - the load order
 * falls out (a dependency is always in before its dependent, so no separate topological sort is
 * needed), the report can name *every* plugin that will not be loaded with the reason (its own, or
 * the dependency that blocks it, chain by chain), and everything else still loads: one third-party
 * plugin with a missing or disabled dependency must not cost the application its shell.
 *
 * @param manager    Manager whose enable and skip decisions are used.
 * @param candidates What collectCandidates() found.
 * @param loaded     Plugins already loaded: satisfied dependencies.
 * @param discovered Discovery list, to tell "missing" from "disabled" from "blocked".
 * @return The plan; the reason for everything left out is logged here.
 */
LoadPlan resolveLoadPlan(const PluginManager& manager, const std::vector<Candidate>& candidates,
                         const std::vector<LoadedPlugin>& loaded, const std::vector<PluginEntry>& discovered)
{
    std::vector<const Candidate*> planned;
    planned.reserve(candidates.size());
    std::vector<String> satisfiable;
    satisfiable.reserve(loaded.size() + candidates.size());
    for (const auto& lp : loaded) {
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

    // Everything outside the closure cannot be loaded. Each of those is reported with its own reason,
    // so the reader can tell "install it", "enable it" and "stop skipping it" apart - and can follow a
    // chain, because the dependency that blocks a plugin is reported on its own line as well.
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
            if (PluginManager::isSkipped(dep)) {
                problem.skipped.push_back(dep);
            }
            else if (const PluginEntry* entry = findDiscovered(discovered, dep);
                     entry != nullptr && !manager.isPluginEnabled(dep)) {
                problem.disabled.push_back(dep);
            }
            else if (findDiscovered(discovered, dep) != nullptr) {
                problem.blocked.push_back(dep);  // discovered and enabled, but not loadable
            }
            else {
                problem.missing.push_back(dep);
            }
        }
        problems.push_back(std::move(problem));
    }

    // A cluster whose members only depend on each other is a declared cycle; saying so beats leaving
    // the reader with mutual "not loadable" lines.
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
        VN_LOGE("{}", report);
    }

    return LoadPlan{ std::move(planned), problems.empty() };
}

/**
 * @brief Creates the instances of the planned plugins and registers each one's commands.
 *
 * Step 4 of a load (see loadAllAsync()). Nothing is registered in the manager here: the caller owns
 * @p created and decides what happens to the batch - a load that does not finish unloads it, because
 * a plugin library can be created only once per process and a retry would reuse the same,
 * half-initialized instance.
 *
 * @param plan    What to create, in dependency order.
 * @param startup Progress sink; may be null.
 * @param created Filled with the instances created here, in order (also when this returns false, so
 *                the caller can release them and tag them - Plugin::setInfo() is private to the
 *                manager, so handing the metadata over is the caller's step).
 * @return true when every planned plugin was instantiated; false when one could not be, or when a
 *         stop request arrived.
 */
bool instantiatePlugins(const LoadPlan& plan, StartupProgress* startup, std::vector<LoadedPlugin>& created)
{
    if (startup != nullptr) {
        // One unit per plugin, reported while the heaviest pass (the load() phase below) runs; the
        // phases around it only update the label, so the bar does not reach its end before the
        // plugins are really up.
        startup->stage("正在加载插件", static_cast<double>(plan.planned.size()));
    }

    for (const Candidate* candidate : plan.planned) {
        const String& name = candidate->info->name;

        // 取消：启动被请求停下就不再装下一个（已装的那些由调用方决定去留：应用级取消会把它们卸掉）。
        if (startup != nullptr && startup->stopToken().stop_requested()) {
            VN_LOGI("the plugin load was cancelled; '{}' and the plugins after it are not loaded", toUtf8(name));
            return false;
        }

        if (startup != nullptr) {
            startup->setLabel("正在创建插件 " + toUtf8(name));
        }
        using CreateFn = Plugin* ();
        const auto create = candidate->lib->resolveSymbol<CreateFn>(u8"vinePluginCreate");
        if (!create) {
            VN_LOGE("Plugin '{}' has no create entry point", toUtf8(name));
            return false;
        }
        Plugin* plugin = create();
        if (!plugin) {
            VN_LOGE("Plugin '{}' failed to create its instance", toUtf8(name));
            return false;
        }

        // Register the plugin's commands (VN_DECLARE_COMMAND) inside its own module. The owner scope
        // tags these module commands with the plugin name.
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

        VN_LOGI("Plugin '{}' loaded", toUtf8(name));
        created.push_back(LoadedPlugin{ name, plugin, candidate->path });
    }
    return true;
}

/**
 * @brief Runs the three lifecycle phases of a batch of plugins.
 *
 * Step 5 of a load (see loadAllAsync()): preLoad() for every plugin (each one registers its own
 * commands), then load() for every plugin, then postLoad() for every plugin for cross-plugin wiring.
 * Each plugin gets its own context so it can query pluginName() and its own registered configs, and
 * every phase runs inside a per-plugin owner scope so lifecycle-registered commands are attributed
 * to the plugin.
 *
 * This is the only part of a load that has to be a coroutine: the hooks are (`Plugin::preLoad()` and
 * friends return a task), so a plugin can send heavy, UI-free work to the pool and come back with
 * resumeOnMainThread() before touching UI again.
 *
 * @param created Instances to run the lifecycle on, in dependency order; the caller owns (and
 *                releases) them.
 * @param startup Progress sink; may be null.
 * @return true when every phase ran; false when a stop request arrived.
 * @throws Whatever a hook throws; the caller releases the batch and fails the pass.
 */
vn::async::Task<bool> runLifecycle(const std::vector<LoadedPlugin>& created, StartupProgress* startup)
{
    // 每拍都是一次 `co_await`，原因有两个：钩子是协程（插件自己要分段、要跳池都在里面），而且回调返回后要让循环
    // 转一圈。owner scope 刻意写在**花括号里、不跨 await**：回转期间别的排队调用也会注册命令，owner 还挂着就会
    // 把它们记到这个插件名下。
    for (const auto& lp : created) {
        RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
        PluginLoadContext      context(Application::current(), lp.name);
        co_await lp.plugin->preLoad(&context);
    }

    std::size_t loaded_units = 0;  // plugins whose load() has returned: one unit of the progress each
    for (const auto& lp : created) {
        RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
        PluginLoadContext      context(Application::current(), lp.name);
        if (startup != nullptr) {
            startup->setLabel("正在加载插件 " + toUtf8(lp.name) + " (" + std::to_string(loaded_units + 1) + "/" + std::to_string(created.size()) + ")");
        }
        if (startup != nullptr && startup->stopToken().stop_requested()) {
            VN_LOGI("the plugin load was cancelled; '{}' and the plugins after it are not loaded", toUtf8(lp.name));
            co_return false;
        }
        co_await lp.plugin->load(&context);
        if (startup != nullptr) {
            ++loaded_units;
            startup->advance(static_cast<double>(loaded_units));
        }
    }

    for (const auto& lp : created) {
        RegistrationOwnerScope owner_scope(Application::current() ? Application::current()->commandManager() : nullptr, lp.name);
        PluginLoadContext      context(Application::current(), lp.name);
        if (startup != nullptr) {
            startup->setLabel("正在收尾插件 " + toUtf8(lp.name));
        }
        co_await lp.plugin->postLoad(&context);
    }
    co_return true;
}

} // namespace


bool PluginManager::loadAll()
{
    // The synchronous door: a tool, a test, or a host that runs no loop drives the same load to completion on its own
    // thread. What that costs is the event loop: on the application thread this blocks it for as long as the plugins'
    // own stretches take, which is exactly why the boot uses loadAllAsync() instead. It dispatches while it waits (see
    // MainThreadDispatcher::runToCompletion()) because a plugin that hops to the pool comes back through the application
    // thread, which is the thread this call is holding.
    return MainThreadDispatcher::runToCompletion(loadAllAsync());
}

vn::async::Task<bool> PluginManager::loadAllAsync()
{
    // The load is four steps, and only the last one is a coroutine: the hooks are coroutines (a plugin
    // can send heavy, UI-free startup work to the pool with `co_await vn::async::run(...)` and come back
    // with `resumeOnMainThread()`), while scanning, dependency resolution and instantiation are plain
    // synchronous code (see collectCandidates(), resolveLoadPlan() and instantiatePlugins()).
    //
    // The manager itself does NOT turn the loop between steps. The startup frame repaints synchronously
    // whenever a report changes it (see BootSplash::onStartupChanged()), and pumping the queue during a
    // boot is something this codebase rejects on purpose: it would also run the timers of everything
    // else that is starting up - an embedded render surface drives its own attach backoff that way, and
    // letting it run before the host has laid its window out makes it build a swapchain on a window that
    // has no native handle yet. What the animation needs is a repaint source of its own, not a loop turn
    // here. A caller that has to block while driving this (loadAll()) gets the same load with the
    // dispatcher as its pump - see MainThreadDispatcher::runToCompletion().

    // A plugin that calls loadAll() again from its own preLoad()/load()/postLoad() would interleave with
    // the batch this call owns; refuse the nested call instead of leaving half-initialized instances.
    if (d->loading) {
        VN_LOGW("loadAll() was called from a plugin lifecycle callback; ignoring the nested call");
        co_return false;
    }
    ScopedFlag loading_scope(d->loading);

    // Startup progress is optional: without a startup frame (or a host that reports a headless boot)
    // there is no sink and every report below is a no-op.
    StartupProgress* const startup = StartupProgress::current();
    if (startup != nullptr) {
        startup->stage("正在查找插件");
    }

    const std::vector<Candidate> candidates = collectCandidates(*this, d->discovered, d->policy_disabled, d->plugins);
    const LoadPlan               plan       = resolveLoadPlan(*this, candidates, d->plugins, d->discovered);

    // Created here, registered only at the end: until the whole batch has run its lifecycle it belongs
    // to this call, so a pass that does not finish can release it (a plugin library can be created only
    // once per process, and a retry would reuse the same, half-initialized instance).
    std::vector<LoadedPlugin> created;
    created.reserve(plan.planned.size());

    bool loaded = false;
    try {
        bool batch_ready = instantiatePlugins(plan, startup, created);
        if (batch_ready) {
            // The query entry (from VN_DECLARE_PLUGIN) is the single metadata source, and handing it to
            // the instance is the manager's own step (Plugin::setInfo() is private; the file-local
            // creation step above cannot reach it).
            for (std::size_t i = 0; i < created.size(); ++i) {
                created[i].plugin->setInfo(*plan.planned[i]->info);
            }
            batch_ready = co_await runLifecycle(created, startup);
        }

        if (batch_ready) {
            for (auto& lp : created) {
                d->plugins.push_back(std::move(lp));
            }
            loaded = plan.complete;
        }
        else {
            // A plugin could not be created, or the load was cancelled: neither keeps the batch (the
            // instances are library-level singletons, so a retry would reuse the same half-initialized
            // objects), and the caller reports a cancelled boot as such.
            unloadLoadedPlugins(created);
        }
    }
    catch (const std::exception& e) {
        VN_LOGE("Plugin loading failed, releasing the instances created by this call: {}", e.what());
        unloadLoadedPlugins(created);
    }
    catch (...) {
        VN_LOGE("Plugin loading failed with an unknown exception, releasing the instances created by this call");
        unloadLoadedPlugins(created);
    }

    co_return loaded;
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
            VN_LOGI("Plugin '{}' unloaded", toUtf8(loaded.name));
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
        VN_LOGW("Cannot enable or disable a plugin without a name");
        return false;
    }

    if (const PluginEntry* entry = findDiscovered(d->discovered, name);
        entry != nullptr && entry->scope == PluginScope::BuiltIn) {
        VN_LOGW("Plugin '{}' is provided by the application and cannot be disabled; ask the application to stop shipping it", toUtf8(name));
        return false;
    }

    if (isPolicyDisabled(name, {}) && enabled) {
        // Storing the preference is fine, but the policy keeps winning; saying so
        // avoids a UI that looks broken.
        VN_LOGW("Plugin '{}' is disabled for all users by its registration file; enabling it here has no effect", toUtf8(name));
    }

    if (isSkipped(name) && enabled) {
        // The host's skip list wins the same way; the preference is still stored,
        // so it applies as soon as the host stops skipping the plugin.
        VN_LOGW("Plugin '{}' is skipped by the host; enabling it here has no effect until the host stops skipping it", toUtf8(name));
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
        VN_LOGI("Plugin '{}' is now {}; the change takes effect at the next start", toUtf8(name),
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
        VN_LOGW("Cannot register an empty plugin location");
        return {};
    }
    if (scope == PluginScope::BuiltIn) {
        VN_LOGW("Refusing to register '{}' as an application-provided plugin; what ships with the application is not installed by the user",
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
        VN_LOGW("Cannot register plugin location '{}': it does not exist", toUtf8(location));
        return {};
    }

    // The registration directory to write into: the per-user one, or the first
    // system directory that exists (writing there needs administrator rights).
    const auto directories = registrationDirectories();
    if (directories.empty()) {
        VN_LOGW("Cannot register plugin location '{}': the application has no registration directory", toUtf8(location));
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
            VN_LOGW("Cannot register plugin location '{}' for all users: no writable system registration directory", toUtf8(location));
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
            VN_LOGW("{}", toUtf8(queried.rejection));
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
        VN_LOGW("Cannot register plugin location '{}': '{}' cannot be used as a registration file name", toUtf8(location), toUtf8(id));
        return {};
    }

    std::filesystem::path target = directory / std::filesystem::path(std::u8string_view(id.data(), id.size()));
    target += kRegistrationExtension;

    if (!writeRegistrationFile(target, registration)) {
        return {};
    }

    VN_LOGI("Plugin location '{}' registered as '{}' in '{}'; it is scanned at the next start", toUtf8(location), toUtf8(id), toUtf8(directory));
    return id;
}

bool PluginManager::uninstallPlugin(const String& id, PluginScope scope)
{
    // The id is a file name inside the registration directory; refuse anything that
    // could reach outside it (see installPlugin()).
    if (!isUsableRegistrationId(id)) {
        VN_LOGW("Cannot uninstall plugin registration '{}': it is not a usable registration id", toUtf8(id));
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
        file += kRegistrationExtension;

        std::error_code ec;
        if (std::filesystem::remove(file, ec)) {
            VN_LOGI("Plugin registration '{}' removed; it is no longer scanned at the next start", toUtf8(file));
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

VN_APPFW_NS_END
