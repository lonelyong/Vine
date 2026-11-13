#include <vine/backend_vk/internal/XcbSurface.hpp>

#include <cstdint>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#elif !defined(__APPLE__)
#    include <dlfcn.h>
#endif

VN_BACKEND_VK_NS_BEGIN

namespace
{

#if !defined(_WIN32) && !defined(__APPLE__)
/// @brief `VkXcbSurfaceCreateInfoKHR` without including the platform header (same field order and sizes).
struct XcbSurfaceCreateInfo {
    VkStructureType          sType;
    const void*              pNext;
    VkFlags                  flags;
    struct xcb_connection_t* connection;
    std::uint32_t            window;
};
#endif

/// @brief `vkCreateXcbSurfaceKHR` / `vkCreateWin32SurfaceKHR`, resolved without volk's platform declarations.
template <typename Info>
using CreateSurfaceFn = VkResult (*)(VkInstance, const Info*, const VkAllocationCallbacks*, VkSurfaceKHR*);

} // namespace

struct XcbSurface::Impl {
#if !defined(_WIN32) && !defined(__APPLE__)
    void* library{ nullptr };
    void* connection{ nullptr };

    using ConnectFn    = void* (*)(const char*, int*);
    using DisconnectFn = void (*)(void*);
    using HasErrorFn   = int (*)(void*);

    ConnectFn    connect{ nullptr };
    DisconnectFn disconnect{ nullptr };
    HasErrorFn   has_error{ nullptr };
#endif
};

XcbSurface::XcbSurface()
  : impl(std::make_unique<Impl>())
{
}

XcbSurface::~XcbSurface()
{
    close();
}

bool XcbSurface::supported() noexcept
{
#if defined(__APPLE__)
    return false;
#else
    return true;
#endif
}

bool XcbSurface::open(vn::String* why)
{
#if defined(_WIN32)
    (void)why;
    return true; // no connection object: the HWND and the module instance are the whole input
#elif defined(__APPLE__)
    if (why != nullptr) {
        *why = vn::String(u8"vk: no window-system surface is served on this platform (M5 serves X11 and Win32)");
    }
    return false;
#else
    if (impl->connection != nullptr) {
        return true;
    }
    impl->library = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    if (impl->library == nullptr) {
        impl->library = dlopen("libxcb.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (impl->library == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libxcb is not available, so no window surface can be created");
        }
        return false;
    }
    impl->connect    = reinterpret_cast<Impl::ConnectFn>(dlsym(impl->library, "xcb_connect"));
    impl->disconnect = reinterpret_cast<Impl::DisconnectFn>(dlsym(impl->library, "xcb_disconnect"));
    impl->has_error  = reinterpret_cast<Impl::HasErrorFn>(dlsym(impl->library, "xcb_connection_has_error"));
    if (impl->connect == nullptr || impl->disconnect == nullptr || impl->has_error == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libxcb does not export the connection entry points this backend needs");
        }
        close();
        return false;
    }
    int screen       = 0;
    impl->connection = impl->connect(nullptr, &screen);
    if (impl->connection == nullptr || impl->has_error(impl->connection) != 0) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: no X display is reachable (is DISPLAY set?)");
        }
        close();
        return false;
    }
    return true;
#endif
}

void XcbSurface::close() noexcept
{
#if !defined(_WIN32) && !defined(__APPLE__)
    if (impl == nullptr) {
        return;
    }
    if (impl->connection != nullptr && impl->disconnect != nullptr) {
        impl->disconnect(impl->connection);
        impl->connection = nullptr;
    }
    if (impl->library != nullptr) {
        dlclose(impl->library);
        impl->library    = nullptr;
        impl->connect    = nullptr;
        impl->disconnect = nullptr;
        impl->has_error  = nullptr;
    }
#endif
}

void* XcbSurface::connection() const noexcept
{
#if !defined(_WIN32) && !defined(__APPLE__)
    return impl != nullptr ? impl->connection : nullptr;
#else
    return nullptr;
#endif
}

bool XcbSurface::createSurface(const VkInstance instance, void* window, VkSurfaceKHR& surface, vn::String* why) const
{
    surface = VK_NULL_HANDLE;
    if (instance == VK_NULL_HANDLE || window == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a window surface needs an instance and a window handle");
        }
        return false;
    }
#if defined(_WIN32)
    // `VkWin32SurfaceCreateInfoKHR` without including <vulkan/vulkan_win32.h>.
    struct Win32SurfaceCreateInfo {
        VkStructureType sType;
        const void*     pNext;
        VkFlags         flags;
        void*           hinstance;
        void*           hwnd;
    };
    const auto create = reinterpret_cast<CreateSurfaceFn<Win32SurfaceCreateInfo>>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the loader does not export vkCreateWin32SurfaceKHR");
        }
        return false;
    }
    Win32SurfaceCreateInfo info{};
    info.sType     = static_cast<VkStructureType>(1000009000U); // VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd      = window;
    if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) {
        surface = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: vkCreateWin32SurfaceKHR refused the host window");
        }
        return false;
    }
    return true;
#elif defined(__APPLE__)
    (void)window;
    if (why != nullptr) {
        *why = vn::String(u8"vk: no window-system surface is served on this platform (M5 serves X11 and Win32)");
    }
    return false;
#else
    const void* raw_connection = connection();
    if (raw_connection == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the window target has no open xcb connection");
        }
        return false;
    }
    const auto create = reinterpret_cast<CreateSurfaceFn<XcbSurfaceCreateInfo>>(vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));
    if (create == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the loader does not export vkCreateXcbSurfaceKHR");
        }
        return false;
    }
    XcbSurfaceCreateInfo info{};
    info.sType      = static_cast<VkStructureType>(1000005000U); // VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR
    info.connection = static_cast<struct xcb_connection_t*>(const_cast<void*>(raw_connection));
    info.window     = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(window));
    if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) {
        surface = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: vkCreateXcbSurfaceKHR refused the host window");
        }
        return false;
    }
    return true;
#endif
}

VN_BACKEND_VK_NS_END
