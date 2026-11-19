#pragma once
#include "runtime_global.hpp"

#include <memory>
#include <type_traits>

#include <vine/String.hpp>

VN_RUNTIME_NS_BEGIN

class DynamicLibraryLoader;

/**
 * @brief A dynamically loaded shared library, owned by a DynamicLibraryLoader.
 *
 * The platform handle is acquired by DynamicLibraryLoader and wrapped here;
 * destroying the object unloads the library, so whether that ever happens is the
 * loader's Retention policy (an isolated loader unloads at destruction, the
 * process-wide instance keeps its libraries mapped). Exported symbols are
 * resolved with resolveSymbol<T>(). The class is not copyable and can only be
 * constructed by the loader, so a library can never be loaded outside loader
 * management.
 */
class VN_RUNTIME_API DynamicLibrary {

  public:
    /**
     * @brief Unloads the library.
     */
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&)            = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  public:
    /**
     * @brief Resolves an exported symbol.
     *
     * @tparam T Type of the symbol; must be a function pointer type.
     * @param symbol Name of the exported symbol.
     * @return Borrowed; the symbol cast to T, or nullptr when the symbol is not
     *         found. Valid while the owning loader keeps the library mapped.
     *
     * @note
     * - The conversion is a `reinterpret_cast`: a raw symbol address arrives as `void*`, and the
     *   standard allows a `static_cast` only between pointers to objects.
     */
    template <typename T>
    T resolveSymbol(const String& symbol) const
    {
        static_assert(std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>, "DynamicLibrary::resolveSymbol, T must be a function pointer type");
        return reinterpret_cast<T>(resolveSymbolRaw(symbol));
    }

    /**
     * @brief Returns the raw platform library handle.
     *
     * @return Borrowed; the platform handle (HMODULE on Windows, void* on POSIX),
     *         valid while the owning loader keeps the library mapped.
     */
    void* handle() const;

    /**
     * @brief Returns the file name or path that resolved when the library was loaded.
     *
     * This is the candidate the loader passed to the platform (a search-path join
     * or the caller's path), not the canonical identity used for caching.
     *
     * @return Borrowed; the spelling that loaded the library.
     */
    const String& fileName() const;

  private:
    /**
     * @brief Wraps an already-loaded platform handle.
     *
     * Takes ownership of the handle; only DynamicLibraryLoader may create a
     * DynamicLibrary, keeping the actual loading under loader control.
     *
     * @param handle Loaded platform library handle.
     * @param file_name Path the library was loaded from.
     */
    DynamicLibrary(void* handle, String file_name);

    /**
     * @brief Resolves an exported symbol as a raw address.
     *
     * @param symbol Name of the exported symbol.
     * @return The symbol address as void*, or nullptr when not found.
     */
    void* resolveSymbolRaw(const String& symbol) const;

    struct Impl;
    std::unique_ptr<Impl> d;

    friend class DynamicLibraryLoader;
};

VN_RUNTIME_NS_END
