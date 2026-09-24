#include <vine/runtime/DynamicLibraryLoader.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

VN_RUNTIME_NS_BEGIN

namespace
{

bool isDirectPath(const String& name)
{
    return name.find(u8'/') != String::npos || name.find(u8'\\') != String::npos;
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

void* loadLibraryHandle(const String& file_path, bool search_user_dirs)
{
#ifdef _WIN32
    const auto wide_path = file_path.toUtf16();
    DWORD      flags     = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
    if (search_user_dirs) {
        flags |= LOAD_LIBRARY_SEARCH_USER_DIRS;
    }
    return reinterpret_cast<void*>(LoadLibraryExW(reinterpret_cast<const wchar_t*>(wide_path.data()), nullptr, flags));
#else
    (void)search_user_dirs;
    return dlopen(reinterpret_cast<const char*>(file_path.c_str()), RTLD_LAZY);
#endif
}

} // namespace

struct DynamicLibraryLoader::Impl {
    std::map<String, std::unique_ptr<DynamicLibrary>> libs;
    std::vector<String>                               search_paths;     // Library-name search directories.
    std::vector<String>                               dependency_paths; // Dependency dll search directories.
#ifdef _WIN32
    std::vector<void*> dependency_cookies; // Handles returned by AddDllDirectory.
#endif
};

DynamicLibraryLoader::DynamicLibraryLoader()
  : d(new Impl)
{}

DynamicLibraryLoader::~DynamicLibraryLoader()
{
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
    static DynamicLibraryLoader loader;
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
#ifdef _WIN32
    const bool search_user_dirs = !d->dependency_cookies.empty();
#else
    const bool search_user_dirs = false;
#endif
    for (const auto& candidate : resolveCandidates(d->search_paths, name)) {
        auto it = d->libs.find(candidate);
        if (it != d->libs.end()) {
            return it->second.get();
        }

        void* handle = loadLibraryHandle(candidate, search_user_dirs);
        if (handle == nullptr) {
            continue;
        }

        auto            lib = std::unique_ptr<DynamicLibrary>(new DynamicLibrary(handle, candidate));
        DynamicLibrary* raw = lib.get();
        d->libs.emplace(candidate, std::move(lib));
        return raw;
    }
    return nullptr;
}

DynamicLibrary* DynamicLibraryLoader::find(const String& name) const
{
    for (const auto& candidate : resolveCandidates(d->search_paths, name)) {
        auto it = d->libs.find(candidate);
        if (it != d->libs.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

std::size_t DynamicLibraryLoader::count() const
{
    return d->libs.size();
}

VN_RUNTIME_NS_END
