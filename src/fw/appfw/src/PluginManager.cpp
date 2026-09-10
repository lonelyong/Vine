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
 * @brief Applies the four inputs that decide whether a plugin may be loaded.
 *
 * A free function so that pluginEntries() can resolve a whole list from one
 * snapshot of the inputs instead of one config read per plugin; the order of the
 * checks is the one documented by PluginManager::isPluginEnabled(), which uses it
 * as well.
 *
 * @param name            Plugin name.
 * @param built_in        Whether the plugin ships with the application.
 * @param skipped         Whether the host's skip list refuses it.
 * @param policy_disabled Whether a registration file disables it for everyone.
 * @param disabled        Names disabled by the per-user preference.
 * @return true if the plugin may be loaded.
 */
bool resolveEnabled(const String& name, bool built_in, bool skipped, bool policy_disabled, const std::vector<String>& disabled)
{
    // The host's own switch wins over everything, including what ships with the
    // application: that is how a single run (headless, safe mode) keeps a plugin
    // out without touching the user's stored preference.
    if (skipped) {
        return false;
    }
    // What ships with the application is not the user's to disable, and neither is
    // the user's preference.
    if (built_in) {
        return true;
    }
    // An administrator policy is not user-togglable either.
    if (policy_disabled) {
        return false;
    }
    return std::find(disabled.begin(), disabled.end(), name) == disabled.end();
}

/**
 * @brief Records a discovered plugin in the discovery list.
 *
 * Discovery is independent of loading: the list holds every valid Vine plugin
 * found in any plugin location, including the disabled ones and the ones the host
 * skips. The enabled, skipped and loaded flags of the returned entries are filled
 * in by pluginEntries() at query time.
 *
 * @param entries Discovery list of the manager.
 * @param info    Plugin metadata.
 * @param path    Library the plugin was found in.
 * @param scope   Location class the plugin was found in.
 */
void rememberDiscovered(std::vector<PluginEntry>& entries, const PluginInfo& info, const std::filesystem::path& path, PluginScope scope)
{
    const auto known = std::find_if(entries.begin(), entries.end(),
        [&info](const PluginEntry& entry) { return entry.info.name == info.name; });
    if (known != entries.end()) {
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
    entries.push_back(PluginEntry{ .info = info, .path = path, .scope = scope });
}

/**
 * @brief Returns the discovery entry of a plugin, or nullptr when unknown.
 */
const PluginEntry* findDiscovered(const std::vector<PluginEntry>& entries, const String& name)
{
    for (const auto& entry : entries) {
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
    return !relative.empty() && relative.native().rfind("..", 0) != 0;
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
 * @brief Queries a plugin library for its metadata.
 *
 * The library stays mapped for the process lifetime (see DynamicLibraryLoader),
 * so querying before an install decision has no lifetime consequence.
 *
 * @param library Plugin library file.
 * @return The metadata, or nullptr when the file is not a loadable Vine plugin.
 */
const PluginInfo* queryPlugin(const std::filesystem::path& library)
{
    vine::runtime::DynamicLibrary* lib =
        vine::runtime::DynamicLibraryLoader::instance().load(String(library.u8string()));
    if (lib == nullptr) {
        return nullptr;
    }
    using QueryFn = const PluginInfo* ();
    const auto query = lib->resolveSymbol<QueryFn>(u8"vinePluginQuery");
    return query != nullptr ? query() : nullptr;
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

/// Resolves a plugin name or path to a library file.
std::filesystem::path resolvePluginPath(const String& str)
{
    const std::filesystem::path given(std::u8string_view(str.data(), str.size()));
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

    /// Discovered plugins, in scan order of the last loadAll(); not all loaded.
    std::vector<PluginEntry> entries;

    /// Disabled plugin names, used only when no host config manager exists.
    std::vector<String> disabled;

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

Plugin* PluginManager::load(const String& str)
{
    // Skip by input name (bare plugin name case); avoids loading the library.
    // Nothing can be discovered here: without the library there is no metadata.
    if (isSkipped(str)) {
        V_LOGI("Plugin '{}' is skipped by the host; not loading it", toUtf8(str));
        return nullptr;
    }

    const std::filesystem::path path = resolvePluginPath(str);
    if (path.empty()) {
        return nullptr;
    }

    // Step 1: load the plugin library (cached by the shared loader).
    vine::runtime::DynamicLibrary* lib = vine::runtime::DynamicLibraryLoader::instance().load(String(path.u8string()));
    if (!lib) {
        return nullptr;
    }

    // Step 2: query — is this a Vine plugin, and what is its metadata?
    using QueryFn = const PluginInfo* ();
    const auto query = lib->resolveSymbol<QueryFn>(u8"vinePluginQuery");
    if (!query) {
        return nullptr;
    }
    const PluginInfo* info = query();
    if (!info) {
        return nullptr;
    }

    // The metadata of a plugin that is not loaded is still reported, so a
    // disabled or skipped plugin stays visible in the plugin list.
    rememberDiscovered(d->entries, *info, path, isBuiltInPath(path) ? PluginScope::BuiltIn : PluginScope::User);

    // Reuse an already-loaded plugin with the same name.
    for (const auto& lp : d->plugins) {
        if (lp.name == info->name) {
            return lp.plugin;
        }
    }

    // A disabled plugin, or one the host skips, is never instantiated, not even
    // when its library is named explicitly: the decision applies to every load.
    if (!isPluginEnabled(info->name)) {
        if (isSkipped(info->name)) {
            V_LOGI("Plugin '{}' is skipped by the host; not loading it", toUtf8(info->name));
        }
        else {
            V_LOGW("Plugin '{}' is disabled; not loading it", toUtf8(info->name));
        }
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
    const auto create = lib->resolveSymbol<CreateFn>(u8"vinePluginCreate");
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
        const auto register_cmds = lib->resolveSymbol<RegisterFn>(u8"vinePluginRegisterCommands");
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
    d->entries.clear();

    // Step 1: collect the library files of every configured location. The
    // application's own directory comes first, then the per-user registrations
    // and finally the machine-wide ones, so the first location that provides a
    // plugin name wins and an application-provided plugin is never overridden.
    struct Source {
        std::filesystem::path path;
        PluginScope           scope;
        bool                  policy_enabled; // false: a registration disables it for everyone.
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
            vine::runtime::DynamicLibrary* lib =
                vine::runtime::DynamicLibraryLoader::instance().load(String(path.u8string()));
            if (!lib) {
                continue;
            }
            using QueryFn = const PluginInfo* ();
            const auto query = lib->resolveSymbol<QueryFn>(u8"vinePluginQuery");
            if (!query) {
                continue; // Not a Vine plugin.
            }
            const PluginInfo* info = query();
            if (!info) {
                continue;
            }

            // Discovery is independent of loading: a disabled plugin, or one the
            // host skips, is still reported (metadata + library path) so a UI can
            // show it and explain why it is not running.
            rememberDiscovered(d->entries, *info, path, source.scope);

            // A registration that disables the plugin for every user is policy:
            // record it before the enabled check below, which reads it back.
            if (!source.policy_enabled) {
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
                if (isSkipped(info->name)) {
                    V_LOGI("Plugin '{}' is skipped by the host; not loading it", toUtf8(info->name));
                }
                else {
                    V_LOGI("Plugin '{}' is disabled; skipped", toUtf8(info->name));
                }
                continue;
            }

            candidates.push_back(Candidate{ path, lib, info });
        }
    }

    // Step 3: dependency resolution. The satisfiable name set is the union of
    // the already-loaded plugins and the newly discovered candidates.
    std::vector<String> available;
    available.reserve(d->plugins.size() + candidates.size());
    for (const auto& lp : d->plugins) {
        available.push_back(lp.name);
    }
    for (const auto& c : candidates) {
        available.push_back(c.info->name);
    }

    // Collect every unsatisfied dependency so that they are reported as one
    // readable block instead of one log line per missing dependency. A dependency
    // that exists but is disabled - or that the host skips - is reported
    // separately: the fix is to enable it or to stop skipping it, not to install
    // it.
    struct UnresolvedDependency {
        String              plugin;
        std::vector<String> missing;
        std::vector<String> disabled;
        std::vector<String> skipped;
    };
    std::vector<UnresolvedDependency> unresolved;
    for (const auto& c : candidates) {
        UnresolvedDependency problem{ c.info->name, {}, {}, {} };
        for (const auto& dep : c.info->dependencies) {
            if (std::find(available.begin(), available.end(), dep) != available.end()) {
                continue;
            }
            if (isSkipped(dep)) {
                problem.skipped.push_back(dep);
            }
            else if (const PluginEntry* entry = findDiscovered(d->entries, dep);
                     entry != nullptr && !isPluginEnabled(dep)) {
                problem.disabled.push_back(dep);
            }
            else {
                problem.missing.push_back(dep);
            }
        }
        if (!problem.missing.empty() || !problem.disabled.empty() || !problem.skipped.empty()) {
            unresolved.push_back(std::move(problem));
        }
    }
    if (!unresolved.empty()) {
        std::string report = "Plugin dependency resolution failed for " + std::to_string(unresolved.size()) + " plugin(s):";
        for (const auto& problem : unresolved) {
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
        }
        V_LOGE("{}", report);
        return false;
    }

    // Step 3: topological sort of the candidates so that every plugin comes
    // after its dependencies. Already-loaded plugins are satisfied roots and do
    // not appear in the order.
    std::map<String, std::size_t>         indegree;
    std::map<String, std::vector<String>> dependents;
    for (const auto& c : candidates) {
        indegree[c.info->name] = 0;
        for (const auto& dep : c.info->dependencies) {
            const bool is_candidate = std::any_of(candidates.begin(), candidates.end(),
                [&dep](const Candidate& other) { return other.info->name == dep; });
            if (is_candidate) {
                dependents[dep].push_back(c.info->name);
                ++indegree[c.info->name];
            }
        }
    }
    std::vector<String> order;
    std::vector<String> ready;
    for (const auto& pair : indegree) {
        if (pair.second == 0) {
            ready.push_back(pair.first);
        }
    }
    while (!ready.empty()) {
        const String name = ready.back();
        ready.pop_back();
        order.push_back(name);
        for (const auto& dependent : dependents[name]) {
            if (--indegree[dependent] == 0) {
                ready.push_back(dependent);
            }
        }
    }
    if (order.size() != candidates.size()) {
        // Plugins that could not be ordered are involved in a cycle; list them.
        std::string report = "Plugin dependency cycle detected among:";
        for (const auto& c : candidates) {
            if (std::find(order.begin(), order.end(), c.info->name) == order.end()) {
                report += "\n  - ";
                report += toUtf8(c.info->name);
            }
        }
        V_LOGE("{}", report);
        return false;
    }

    // Step 4: create the instances in dependency order, and step 5: run the
    // lifecycle. Both are wrapped so that a throwing plugin, or a library without
    // a usable create entry point, cannot leave instances behind that the manager
    // does not know about: a plugin library can be created only once per process,
    // so a retry would reuse the same, half-initialized instance.
    std::vector<LoadedPlugin> created;
    created.reserve(order.size());
    try {
        for (const auto& name : order) {
            const auto it = std::find_if(candidates.begin(), candidates.end(),
                [&name](const Candidate& c) { return c.info->name == name; });
            using CreateFn = Plugin* ();
            const auto create = it->lib->resolveSymbol<CreateFn>(u8"vinePluginCreate");
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
            plugin->setInfo(*it->info);

            // Register the plugin's commands (V_DECLARE_COMMAND) inside its own module.
            // The owner scope tags these module commands with the plugin name.
            using RegisterFn = void(CommandManager*);
            const auto register_cmds = it->lib->resolveSymbol<RegisterFn>(u8"vinePluginRegisterCommands");
            Application* app = Application::current();
            CommandManager* cm = app ? app->commandManager() : nullptr;
            {
                RegistrationOwnerScope owner_scope(cm, name);
                if (register_cmds) {
                    register_cmds(cm);
                }
            }

            V_LOGI("Plugin '{}' loaded", toUtf8(name));
            created.push_back(LoadedPlugin{ name, plugin, it->path });
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
    return true;
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
    if (const PluginEntry* entry = findDiscovered(d->entries, name); entry != nullptr) {
        return entry->path;
    }
    return {};
}

std::vector<PluginEntry> PluginManager::pluginEntries() const
{
    // The four state inputs are snapshotted once, so a list of n plugins costs one
    // config read instead of n (a UI rebuilds this list on every refresh and every
    // selection change) and the reported state is consistent across the list.
    const bool                has_config = hostConfigManager() != nullptr;
    const std::vector<String> disabled   = has_config ? userDisabledFromConfig() : d->disabled;

    std::vector<String> loaded;
    loaded.reserve(d->plugins.size());
    for (const auto& lp : d->plugins) {
        loaded.push_back(lp.name);
    }

    std::vector<PluginEntry> entries = d->entries;
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
    const PluginEntry* entry    = findDiscovered(d->entries, name);
    const bool         built_in = entry != nullptr && entry->scope == PluginScope::BuiltIn;
    const bool         skipped  = isSkipped(name);
    const bool         policy   = std::find(d->policy_disabled.begin(), d->policy_disabled.end(), name) != d->policy_disabled.end();
    // Reads the per-user preference from the host ConfigManager when one is
    // available and falls back to the process-local preference otherwise.
    const std::vector<String> disabled = hostConfigManager() != nullptr ? userDisabledFromConfig() : d->disabled;

    return resolveEnabled(name, built_in, skipped, policy, disabled);
}

bool PluginManager::setPluginEnabled(const String& name, bool enabled)
{
    if (name.empty()) {
        V_LOGW("Cannot enable or disable a plugin without a name");
        return false;
    }

    if (const PluginEntry* entry = findDiscovered(d->entries, name);
        entry != nullptr && entry->scope == PluginScope::BuiltIn) {
        V_LOGW("Plugin '{}' is provided by the application and cannot be disabled; ask the application to stop shipping it", toUtf8(name));
        return false;
    }

    if (std::find(d->policy_disabled.begin(), d->policy_disabled.end(), name) != d->policy_disabled.end() && enabled) {
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
    const auto it = std::find(d->disabled.begin(), d->disabled.end(), name);
    if (enabled) {
        if (it != d->disabled.end()) {
            d->disabled.erase(it);
        }
    }
    else if (it == d->disabled.end()) {
        d->disabled.push_back(name);
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

    const std::filesystem::path location(std::u8string_view(path.data(), path.size()));
    std::error_code             ec;
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
    registration.path    = path;
    registration.enabled = true;

    auto libraries = pluginLibrariesIn(location);
    if (libraries.size() == 1) {
        if (const PluginInfo* info = queryPlugin(*libraries.begin()); info != nullptr) {
            registration.name = info->name;
            registration.uuid = info->uuid;
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
        candidates.assign(directories.begin() + 1, directories.end());
        // Uninstalling for every user may legitimately target the per-user file
        // when it is the only registration; that is what the caller asked for, so
        // only the system directories are considered here.
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
