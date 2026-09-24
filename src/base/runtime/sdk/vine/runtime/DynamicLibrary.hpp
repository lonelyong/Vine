#pragma once
#include "runtime_global.hpp"

#include <memory>

#include <vine/String.hpp>

VN_RUNTIME_NS_BEGIN

class DynamicLibraryLoader;

/**
 * @brief A dynamically loaded shared library, owned by a DynamicLibraryLoader.
 *
 * The platform handle is acquired by DynamicLibraryLoader and wrapped here;
 * the destructor unloads the library. Exported symbols are resolved with
 * resolveSymbol<T>(). The class is not copyable and can only be constructed
 * by the loader, so a library can never be loaded outside loader management.
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
     * @tparam T Type of the symbol; typically a function pointer type.
     * @param symbol Name of the exported symbol.
     * @return The symbol cast to T*, or nullptr when the symbol is not found.
     */
    template <typename T>
    T* resolveSymbol(const String& symbol) const;

    /**
     * @brief Returns the raw platform library handle.
     *
     * @return The platform handle (HMODULE on Windows, void* on POSIX).
     */
    void* handle() const;

    /**
     * @brief Returns the library file path.
     *
     * @return The file path the library was loaded from.
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

template <typename T>
T* DynamicLibrary::resolveSymbol(const String& symbol) const
{
    return reinterpret_cast<T*>(resolveSymbolRaw(symbol));
}

VN_RUNTIME_NS_END
