// dlinfo(RTLD_DI_LINKMAP) is a GNU extension; the macro must precede every include.
#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

#include <vine/runtime/DynamicLibraryLoader.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#    if defined(__linux__)
#        include <link.h>
#    endif
#endif

VN_RUNTIME_NS_BEGIN

namespace
{

bool isDirectPath(const String& name)
{
    if (name.find(u8'/') != String::npos || name.find(u8'\\') != String::npos) {
        return true;
    }
#ifdef _WIN32
    // `C:lib.dll` names a drive but has no separator; it is still a direct path.
    return name.find(u8':') != String::npos;
#else
    return false;
#endif
}

String joinPath(const String& dir, const String& name)
{
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == u8'/' || dir.back() == u8'\\') {
        return dir + name;
    }
    return dir + u8"/" + name;
}

std::vector<String> resolveCandidates(const std::vector<String>& search_paths, const String& name)
{
    if (isDirectPath(name)) {
        return { name };
    }
    std::vector<String> candidates;
    candidates.reserve(search_paths.size() + 1);
    for (const auto& dir : search_paths) {
        candidates.push_back(joinPath(dir, name));
    }
    candidates.push_back(name); // Fall back to the system default search.
    return candidates;
}

std::filesystem::path pathOf(const String& text)
{
#ifdef _WIN32
    return std::filesystem::path(text.toUtf16());
#else
    return std::filesystem::path(std::string(reinterpret_cast<const char*>(text.c_str())));
#endif
}

String canonicalFileName(const std::filesystem::path& file)
{
    std::error_code error;
    const auto      canonical = std::filesystem::weakly_canonical(file, error);
    return String((error ? file : canonical).u8string());
}

std::filesystem::path loadedFileOf(void* handle)
{
#ifdef _WIN32
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(handle), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2); // truncated: grow and ask again
    }
#elif defined(__linux__)
    // The handle is a link_map; this is the file the dynamic linker actually opened,
    // which a bare name resolved by the system loader has no other way to report.
    struct link_map* map = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr && map->l_name != nullptr && map->l_name[0] != '\0') {
        return std::filesystem::path(std::string(map->l_name));
    }
    return {};
#else
    (void)handle;
    return {};
#endif
}

String describeFailure(const String& candidate, const String& reason)
{
#ifdef _WIN32
    return candidate + String(u8": ") + reason;
#else
    (void)candidate; // dlerror() already names the library it could not open
    return reason;
#endif
}

void* loadLibraryHandle(const String& file_path, bool search_user_dirs, String& reason)
{
#ifdef _WIN32
    const auto wide_path = file_path.toUtf16();
    DWORD      flags     = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
    if (search_user_dirs) {
        flags |= LOAD_LIBRARY_SEARCH_USER_DIRS;
    }
    const auto handle = LoadLibraryExW(reinterpret_cast<const wchar_t*>(wide_path.data()), nullptr, flags);
    if (handle == nullptr) {
        reason = String::fromUtf8(std::system_category().message(static_cast<int>(GetLastError())));
        return nullptr;
    }
    return reinterpret_cast<void*>(handle);
#else
    (void)search_user_dirs;
    dlerror(); // clear any stale reason before the call
    void* handle = dlopen(reinterpret_cast<const char*>(file_path.c_str()), RTLD_LAZY);
    if (handle == nullptr) {
        const char* error = dlerror();
        reason            = String::fromUtf8(error != nullptr ? error : "dlopen failed");
    }
    return handle;
#endif
}

} // namespace

struct DynamicLibraryLoader::Impl {
    explicit Impl(Retention retention_)
      : retention(retention_)
    {}

    std::map<String, std::unique_ptr<DynamicLibrary>> libs;
    std::vector<String>                               search_paths;     // Library-name search directories.
    std::vector<String>                               dependency_paths; // Dependency dll search directories.
    Retention                                         retention;
    String                                            last_error; // Why the most recent load() failed.
#ifdef _WIN32
    std::vector<void*> dependency_cookies; // Handles returned by AddDllDirectory.
#endif
};

DynamicLibraryLoader::DynamicLibraryLoader()
  : DynamicLibraryLoader(Retention::UnloadOnDestruction)
{}

DynamicLibraryLoader::DynamicLibraryLoader(Retention retention)
  : d(new Impl(retention))
{}

DynamicLibraryLoader::~DynamicLibraryLoader()
{
    if (d->retention != Retention::KeepMapped) {
        return; // the Impl's unique_ptrs destroy each library, which unloads its handle
    }

    // Plugins are process-lifetime: PluginManager loads them once and never
    // unloads them, so their libraries must stay mapped for the whole process.
    // Unloading them here — at process exit, in a static-destruction order
    // relative to the Application and the process-wide registries
    // (CommandManager, RenderBackendRegistry) that still hold command
    // callables / factory pointers into the plugin code — crashes with SIGSEGV
    // because the plugin code is unmapped before those objects are destroyed.
    // Deliberately leak the loaded libraries instead: the OS reclaims them
    // when the process exits, so every registry teardown runs while the plugin
    // code is still mapped. When real plugin unload support is added, this
    // destructor must release the libraries only AFTER the registries that
    // reference their code have been cleared.
    d.release(); // never dlclose: keep the process-lifetime plugin code mapped
}

DynamicLibraryLoader& DynamicLibraryLoader::instance()
{
    static DynamicLibraryLoader loader(Retention::KeepMapped);
    return loader;
}

void DynamicLibraryLoader::addSearchPath(const String& dir_path)
{
    d->search_paths.push_back(dir_path);
}

void DynamicLibraryLoader::removeSearchPath(const String& dir_path)
{
    auto it = std::find(d->search_paths.begin(), d->search_paths.end(), dir_path);
    if (it != d->search_paths.end()) {
        d->search_paths.erase(it);
    }
}

void DynamicLibraryLoader::clearSearchPaths()
{
    d->search_paths.clear();
}

const std::vector<String>& DynamicLibraryLoader::searchPaths() const
{
    return d->search_paths;
}

void DynamicLibraryLoader::addDependencyPath(const String& dir_path)
{
#ifdef _WIN32
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto wide_path = dir_path.toUtf16();
    void*      cookie    = AddDllDirectory(reinterpret_cast<const wchar_t*>(wide_path.data()));
    d->dependency_paths.push_back(dir_path);
    d->dependency_cookies.push_back(cookie);
#else
    d->dependency_paths.push_back(dir_path);
#endif
}

void DynamicLibraryLoader::removeDependencyPath(const String& dir_path)
{
    auto it = std::find(d->dependency_paths.begin(), d->dependency_paths.end(), dir_path);
    if (it == d->dependency_paths.end()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(it - d->dependency_paths.begin());
#ifdef _WIN32
    if (index < d->dependency_cookies.size() && d->dependency_cookies[index] != nullptr) {
        RemoveDllDirectory(d->dependency_cookies[index]);
    }
    d->dependency_cookies.erase(d->dependency_cookies.begin() + static_cast<std::ptrdiff_t>(index));
#endif
    d->dependency_paths.erase(it);
}

void DynamicLibraryLoader::clearDependencyPaths()
{
#ifdef _WIN32
    for (void* cookie : d->dependency_cookies) {
        if (cookie != nullptr) {
            RemoveDllDirectory(cookie);
        }
    }
    d->dependency_cookies.clear();
#endif
    d->dependency_paths.clear();
}

const std::vector<String>& DynamicLibraryLoader::dependencyPaths() const
{
    return d->dependency_paths;
}

DynamicLibrary* DynamicLibraryLoader::load(const String& name)
{
    d->last_error.clear();
#ifdef _WIN32
    const bool search_user_dirs = !d->dependency_cookies.empty();
#else
    const bool search_user_dirs = false;
#endif
    for (const auto& candidate : resolveCandidates(d->search_paths, name)) {
        const String key = canonicalFileName(pathOf(candidate));
        auto         it  = d->libs.find(key);
        if (it != d->libs.end()) {
            return it->second.get();
        }

        String reason;
        void*  handle = loadLibraryHandle(candidate, search_user_dirs, reason);
        if (handle == nullptr) {
            d->last_error = describeFailure(candidate, reason);
            continue;
        }

        auto            lib = std::unique_ptr<DynamicLibrary>(new DynamicLibrary(handle, candidate));
        DynamicLibrary* raw = lib.get();

        // A bare name that only the system loader could resolve has no canonical
        // path of its own; identity comes from the file the platform opened. If a
        // different spelling loaded that file first, the temporary above unloads
        // the extra reference when this scope ends.
        const auto   loaded     = loadedFileOf(handle);
        const String actual_key = loaded.empty() ? key : canonicalFileName(loaded);
        auto         existing   = d->libs.find(actual_key);
        if (existing != d->libs.end()) {
            return existing->second.get();
        }

        d->libs.emplace(actual_key, std::move(lib));
        return raw;
    }
    return nullptr;
}

DynamicLibrary* DynamicLibraryLoader::find(const String& name) const
{
    for (const auto& candidate : resolveCandidates(d->search_paths, name)) {
        auto it = d->libs.find(canonicalFileName(pathOf(candidate)));
        if (it != d->libs.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

const String& DynamicLibraryLoader::lastError() const
{
    return d->last_error;
}

std::size_t DynamicLibraryLoader::count() const
{
    return d->libs.size();
}

VN_RUNTIME_NS_END
