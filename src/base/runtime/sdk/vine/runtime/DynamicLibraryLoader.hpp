#pragma once
#include "runtime_global.hpp"

#include <cstddef>
#include <memory>
#include <vector>

#include <vine/String.hpp>

#include "DynamicLibrary.hpp"

VN_RUNTIME_NS_BEGIN

/**
 * @brief Loads and caches dynamic libraries, one per file path.
 *
 * The loader performs the actual loading and hands out DynamicLibrary objects
 * that wrap the loaded handle. Loading the same path twice returns the
 * already-loaded DynamicLibrary, so each library is loaded exactly once per
 * loader. The loader owns all loaded libraries and keeps them mapped for the
 * process lifetime (see the destructor): its only client, the appfw plugin
 * manager, loads plugins once and never unloads them, so unloading the
 * libraries at process exit — before the registries that reference their code
 * are torn down — would crash (SIGSEGV).
 *
 * A bare library name passed to load() is resolved through the configured
 * search paths: each directory is tried in order, then the name falls back to
 * the system default search.
 *
 * On Windows a loaded library's dependencies are resolved from its own
 * directory and the standard directories; extra dependency directories may be
 * registered with addDependencyPath(). On POSIX dependency resolution follows
 * the system dynamic linker (RPATH / LD_LIBRARY_PATH).
 *
 * Use instance() for the process-wide shared loader so every module that
 * links vn::Runtime manages its libraries through one place. The public
 * constructor remains available for isolated loaders, e.g. in tests.
 */
class VN_RUNTIME_API DynamicLibraryLoader {

  public:
    /**
     * @brief Creates an empty loader.
     */
    DynamicLibraryLoader();

    /**
     * @brief Destroys the loader WITHOUT unloading its libraries.
     *
     * Loaded libraries are deliberately leaked (never dlclose'd): plugins live
     * for the process lifetime, and tearing the code down during exit-time
     * static destruction would leave the registries (commands / render-backend
     * factories) referencing unmapped plugin code. The OS reclaims the
     * libraries when the process exits.
     */
    ~DynamicLibraryLoader();

    DynamicLibraryLoader(const DynamicLibraryLoader&)            = delete;
    DynamicLibraryLoader& operator=(const DynamicLibraryLoader&) = delete;

  public:
    /**
     * @brief Returns the process-wide shared library loader.
     *
     * A single loader is shared by all modules that link vn::Runtime, so a
     * library is loaded and unloaded exactly once per process regardless of
     * which module requests it. The instance is created on first use and
     * lives until process exit.
     *
     * @return The shared DynamicLibraryLoader.
     */
    static DynamicLibraryLoader& instance();

    /**
     * @brief Appends a directory to the search path list.
     *
     * Directories are tried in the order they were added when load() resolves
     * a bare library name.
     *
     * @param dir_path Directory to search; e.g. the plugin directory.
     */
    void addSearchPath(const String& dir_path);

    /**
     * @brief Removes a directory from the search path list.
     *
     * @param dir_path Directory to remove.
     */
    void removeSearchPath(const String& dir_path);

    /**
     * @brief Removes all search paths.
     */
    void clearSearchPaths();

    /**
     * @brief Returns the search path list.
     *
     * @return The directories searched in order for bare library names.
     */
    const std::vector<String>& searchPaths() const;

    /**
     * @brief Appends a directory searched for a loaded library's dependencies.
     *
     * On Windows the directory is registered with AddDllDirectory and is
     * searched, together with the library's own directory and the standard
     * directories, when the library's imports are resolved.
     *
     * @param dir_path Directory that may contain dependency libraries.
     */
    void addDependencyPath(const String& dir_path);

    /**
     * @brief Removes a dependency search directory.
     *
     * @param dir_path Directory to remove.
     */
    void removeDependencyPath(const String& dir_path);

    /**
     * @brief Removes all dependency search directories.
     */
    void clearDependencyPaths();

    /**
     * @brief Returns the dependency search directory list.
     *
     * @return The directories searched for a loaded library's dependencies.
     */
    const std::vector<String>& dependencyPaths() const;

    /**
     * @brief Loads a dynamic library, reusing the cached instance if present.
     *
     * When name contains no path separator it is resolved against the search
     * paths: each directory is tried in order, then the bare name falls back
     * to the system default search. The cache is keyed by the resolved path,
     * so loading the same file through different names reuses one instance.
     *
     * @param name Library file name or path.
     * @return The loaded library, or nullptr when loading fails.
     */
    DynamicLibrary* load(const String& name);

    /**
     * @brief Returns the library already loaded for the given name or path.
     *
     * The name is resolved with the same rules as load().
     *
     * @param name Library file name or path.
     * @return The cached library, or nullptr when it is not loaded.
     */
    DynamicLibrary* find(const String& name) const;

    /**
     * @brief Returns the number of loaded libraries.
     *
     * @return The count of distinct loaded library paths.
     */
    std::size_t count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_RUNTIME_NS_END
