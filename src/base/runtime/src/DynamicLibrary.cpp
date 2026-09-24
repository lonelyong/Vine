#include <vine/runtime/DynamicLibrary.hpp>

#include <utility>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

VN_RUNTIME_NS_BEGIN

struct DynamicLibrary::Impl {
    String file_name;
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

DynamicLibrary::DynamicLibrary(void* handle, String file_name)
  : d(new Impl)
{
    d->file_name = std::move(file_name);
#ifdef _WIN32
    d->handle = reinterpret_cast<HMODULE>(handle);
#else
    d->handle = handle;
#endif
}

DynamicLibrary::~DynamicLibrary()
{
#ifdef _WIN32
    if (d->handle != nullptr) {
        FreeLibrary(d->handle);
    }
#else
    if (d->handle != nullptr) {
        dlclose(d->handle);
    }
#endif
}

void* DynamicLibrary::resolveSymbolRaw(const String& symbol) const
{
#ifdef _WIN32
    FARPROC proc = GetProcAddress(d->handle, reinterpret_cast<const char*>(symbol.c_str()));
    return proc != nullptr ? reinterpret_cast<void*>(proc) : nullptr;
#else
    return dlsym(d->handle, reinterpret_cast<const char*>(symbol.c_str()));
#endif
}

void* DynamicLibrary::handle() const
{
    return d->handle;
}

const String& DynamicLibrary::fileName() const
{
    return d->file_name;
}

VN_RUNTIME_NS_END
