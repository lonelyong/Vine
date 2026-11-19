#pragma once
#include "runtime_global.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/String.hpp>

#include "DynamicLibrary.hpp"

VN_RUNTIME_NS_BEGIN

/**
 * @brief Loads and caches dynamic libraries, one object per file.
 *
 * The loader performs the actual loading and hands out DynamicLibrary objects
 * that wrap the loaded handle. Loading the same file twice returns the
 * already-loaded DynamicLibrary: the cache key is the canonical path of the file
 * the platform reports for the loaded module, so a symlink, a dot-segment
 * spelling or a bare name found through a search path all reuse one instance.
 * fileName() keeps the spelling that resolved.
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
 * Retention is chosen at construction. A loader created by instance() keeps its
 * libraries mapped for the process lifetime: plugin code must outlive the
 * registries (commands, render-backend factories) that hold its function
 * pointers, and unloading it during exit-time destruction crashes. A loader
 * created directly unloads its libraries when it is destroyed.
 *
 * Use instance() for the process-wide shared loader so every module that links
 * vn::Runtime manages its libraries through one place. The public constructors
 * remain available for isolated loaders, e.g. in tests.
 *
 * @warning
 * - The loader is not thread-safe: load(), find(), lastError() and the
 *   search-path mutators must not run concurrently with one another.
 */
class VN_RUNTIME_API DynamicLibraryLoader {

  public:
    /**
     * @brief What a loader does with its libraries when it is destroyed.
     */
    enum class Retention : std::uint8_t {
        /// Unload every library; its handles become invalid at destruction.
        UnloadOnDestruction,
        /// Keep every library mapped for the process lifetime (plugin code).
        KeepMapped,
    };

  public:
    /**
     * @brief Creates an empty loader that unloads its libraries when destroyed.
     */
    DynamicLibraryLoader();

    /**
     * @brief Creates an empty loader with the given retention policy.
     *
     * @param retention What to do with the loaded libraries when this loader is destroyed.
     */
    explicit DynamicLibraryLoader(Retention retention);

    /**
     * @brief Destroys the loader, unloading its libraries unless Retention::KeepMapped
     *        was chosen.
     *
     * KeepMapped is what instance() uses: the libraries are deliberately leaked
     * so the registries that reference plugin code are torn down while it is
     * still mapped, and the OS reclaims them at process exit. When real plugin
     * unload support is added, those registries must be cleared first.
     */
    ~DynamicLibraryLoader();

    DynamicLibraryLoader(const DynamicLibraryLoader&)            = delete;
    DynamicLibraryLoader& operator=(const DynamicLibraryLoader&) = delete;

  public:
    /**
     * @brief Returns the process-wide shared library loader.
     *
     * The shared instance is created on first use, keeps its libraries mapped
     * for the process lifetime (Retention::KeepMapped), and lives until process
     * exit.
     *
     * @return The shared loader; a static object, valid for the process lifetime.
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
     * @return Borrowed; the directories searched in order for bare library names.
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
     * @return Borrowed; the directories searched for a loaded library's dependencies.
     */
    const std::vector<String>& dependencyPaths() const;

    /**
     * @brief Loads a dynamic library, reusing the cached instance if present.
     *
     * @details
     * - When name contains no path separator it is resolved against the search
     *   paths: each directory is tried in order, then the bare name falls back
     *   to the system default search.
     * - Identity is the canonical path of the file the platform reports for the
     *   loaded module, so one file loaded through a symlink, a dot-segment
     *   spelling or a search-path name is one DynamicLibrary.
     * - A failure records the platform's reason in lastError().
     *
     * @param name Library file name or path.
     * @return Borrowed; the loaded library, owned by this loader, or nullptr when
     *         loading fails.
     */
    DynamicLibrary* load(const String& name);

    /**
     * @brief Returns the library already loaded for the given name or path.
     *
     * The name is resolved with the same rules as load(). A bare name that only
     * the system loader could have found has no cache entry to match, because
     * the file's location is unknown without loading it.
     *
     * @param name Library file name or path.
     * @return Borrowed; the cached library, owned by this loader, or nullptr when
     *         it is not loaded.
     */
    DynamicLibrary* find(const String& name) const;

    /**
     * @brief Returns why the most recent load() failed.
     *
     * @details
     * - The string is the platform's own reason (`dlerror` on POSIX, the Win32
     *   error text on Windows) prefixed with the candidate that failed; the last
     *   candidate tried is the one reported.
     * - It is cleared when load() succeeds, so a non-empty value always belongs
     *   to the call that just returned nullptr.
     *
     * @return Borrowed; empty when the last load() succeeded or has not run.
     */
    const String& lastError() const;

    /**
     * @brief Returns the number of distinct files loaded.
     *
     * @return The count of distinct library files.
     */
    std::size_t count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_RUNTIME_NS_END
