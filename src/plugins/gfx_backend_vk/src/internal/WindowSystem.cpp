#include <vine/backend_vk/internal/WindowSystem.hpp>

#include <cstdint>
#include <iterator>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#elif !defined(__APPLE__)
#    include <dlfcn.h>
#endif

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The title a device-owned window carries on every platform: it says which backend brought it up.
constexpr const char* kOwnWindowTitle = "Vine (vk)";

#if defined(_WIN32)
/**
 * @brief The window procedure of the class a device-owned window belongs to.
 *
 * @details
 * - A window without one cannot be created at all (CreateWindowExW refuses a class with no WndProc). This one does
 *   nothing: the window is a presentation target, not a UI - the platform's default handling is the whole job, and a
 *   host that wants its window's messages is one that owns the window (`DevicePresentation::Handle`).
 *
 * @param window  The window the message is for.
 * @param message The message identifier.
 * @param wparam  The message's first parameter.
 * @param lparam  The message's second parameter.
 * @return The platform's default answer.
 */
LRESULT CALLBACK ownWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}
#endif

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

/**
 * @brief `vkCreateXcbSurfaceKHR` / `vkCreateWin32SurfaceKHR`, resolved without volk's platform declarations.
 *
 * @tparam Info The platform create-info type (same field order as the Vulkan header's).
 */
template <typename Info>
using CreateSurfaceFn = VkResult (*)(VkInstance, const Info*, const VkAllocationCallbacks*, VkSurfaceKHR*);

} // namespace

struct WindowSystem::Impl {
#if defined(_WIN32)
    HWND window{ nullptr };
#elif !defined(__APPLE__)
    // The xcb half: the connection a surface is created on.
    void* library{ nullptr };
    void* connection{ nullptr };

    using ConnectFn    = void* (*)(const char*, int*);
    using DisconnectFn = void (*)(void*);
    using HasErrorFn   = int (*)(void*);

    ConnectFn    connect{ nullptr };
    DisconnectFn disconnect{ nullptr };
    HasErrorFn   has_error{ nullptr };

    // The Xlib half: the device-owned window. A separate connection from the surface's, made only when a Probe
    // device asks for its own window (see the header's "one lifetime to obey").
    void* window_library{ nullptr };
    void* display{ nullptr };
    unsigned long window{ 0UL }; // Window (XID)

    using OpenDisplayFn   = void* (*)(const char*);
    using CloseDisplayFn  = int (*)(void*);
    using DefaultRootFn   = unsigned long (*)(void*);
    using CreateWindowFn  = unsigned long (*)(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
    using StoreNameFn     = int (*)(void*, unsigned long, const char*);
    using MapWindowFn     = int (*)(void*, unsigned long);
    using FlushFn         = int (*)(void*);
    using DestroyWindowFn = int (*)(void*, unsigned long);

    OpenDisplayFn   open_display{ nullptr };
    CloseDisplayFn  close_display{ nullptr };
    DefaultRootFn   default_root{ nullptr };
    CreateWindowFn  create_window{ nullptr };
    StoreNameFn     store_name{ nullptr };
    MapWindowFn     map_window{ nullptr };
    FlushFn         flush{ nullptr };
    DestroyWindowFn destroy_window{ nullptr };
#endif
};

WindowSystem::WindowSystem()
  : impl(std::make_unique<Impl>())
{
}

WindowSystem::~WindowSystem()
{
    closeOwnWindow();
    close();
}

const char* WindowSystem::instanceExtension() noexcept
{
#if defined(_WIN32)
    return "VK_KHR_win32_surface";
#elif defined(__APPLE__)
    return "";
#else
    return "VK_KHR_xcb_surface";
#endif
}

bool WindowSystem::open(vn::String* why)
{
#if defined(_WIN32)
    (void)why;
    return true; // no connection object: the HWND and the module instance are the whole input
#elif defined(__APPLE__)
    if (why != nullptr) {
        *why = vn::String(u8"vk: no window-system surface is served on this platform (X11 is served, Win32 is next)");
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

void WindowSystem::close() noexcept
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

bool WindowSystem::createSurface(const VkInstance instance, void* window, VkSurfaceKHR& surface, vn::String* why) const
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
        *why = vn::String(u8"vk: no window-system surface is served on this platform (X11 is served, Win32 is next)");
    }
    return false;
#else
    if (impl->connection == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the window system is not open, so no surface can be created");
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
    info.connection = static_cast<struct xcb_connection_t*>(impl->connection);
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

bool WindowSystem::openOwnWindow(int width, int height, vn::String* why)
{
    if (ownWindow() != nullptr) {
        return true; // one window per session: open is idempotent
    }
    if (width <= 0 || height <= 0) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a device-owned window needs a positive size");
        }
        return false;
    }
#if defined(_WIN32)
    // ONE CLASS PER PROCESS, registered on first use: registering it again answers ERROR_CLASS_ALREADY_EXISTS, which
    // is the same state, not a failure.
    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = &ownWindowProc;
    window_class.hInstance     = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"VineVkOwnWindow";
    window_class.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the device-owned window's class could not be registered");
        }
        return false;
    }
    WCHAR title[32]{};
    MultiByteToWideChar(CP_UTF8, 0, kOwnWindowTitle, -1, title, static_cast<int>(std::size(title)));
    const HWND window = CreateWindowExW(0,
                                        window_class.lpszClassName,
                                        title,
                                        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        width,
                                        height,
                                        nullptr,
                                        nullptr,
                                        window_class.hInstance,
                                        nullptr);
    if (window == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the device-owned window could not be created");
        }
        return false;
    }
    impl->window = window;
    return true;
#elif defined(__APPLE__)
    (void)width;
    (void)height;
    if (why != nullptr) {
        *why = vn::String(u8"vk: a device-owned window is not served on this platform (no surface extension is enabled)");
    }
    return false;
#else
    impl->window_library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (impl->window_library == nullptr) {
        impl->window_library = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (impl->window_library == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libX11 is not available, so a device-owned window cannot be brought up (name a host window with Handle, or ask for None)");
        }
        return false;
    }
    impl->open_display    = reinterpret_cast<Impl::OpenDisplayFn>(dlsym(impl->window_library, "XOpenDisplay"));
    impl->close_display   = reinterpret_cast<Impl::CloseDisplayFn>(dlsym(impl->window_library, "XCloseDisplay"));
    impl->default_root    = reinterpret_cast<Impl::DefaultRootFn>(dlsym(impl->window_library, "XDefaultRootWindow"));
    impl->create_window   = reinterpret_cast<Impl::CreateWindowFn>(dlsym(impl->window_library, "XCreateSimpleWindow"));
    impl->store_name      = reinterpret_cast<Impl::StoreNameFn>(dlsym(impl->window_library, "XStoreName"));
    impl->map_window      = reinterpret_cast<Impl::MapWindowFn>(dlsym(impl->window_library, "XMapWindow"));
    impl->flush           = reinterpret_cast<Impl::FlushFn>(dlsym(impl->window_library, "XFlush"));
    impl->destroy_window  = reinterpret_cast<Impl::DestroyWindowFn>(dlsym(impl->window_library, "XDestroyWindow"));
    if (impl->open_display == nullptr || impl->close_display == nullptr || impl->default_root == nullptr || impl->create_window == nullptr ||
        impl->store_name == nullptr || impl->map_window == nullptr || impl->flush == nullptr || impl->destroy_window == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libX11 does not export the window entry points this backend needs");
        }
        closeOwnWindow();
        return false;
    }
    impl->display = impl->open_display(nullptr);
    if (impl->display == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: no X display is reachable for a device-owned window (is DISPLAY set?)");
        }
        closeOwnWindow();
        return false;
    }
    impl->window = impl->create_window(impl->display,
                                       impl->default_root(impl->display),
                                       0,
                                       0,
                                       static_cast<unsigned int>(width),
                                       static_cast<unsigned int>(height),
                                       0,
                                       0,
                                       0);
    if (impl->window == 0UL) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the device-owned window could not be created");
        }
        closeOwnWindow();
        return false;
    }
    // Mapped and flushed HERE: the surface may be created on this window as soon as the call returns, and a window
    // that is not mapped is what a compositor reports as an out-of-date surface on the first acquire.
    (void)impl->store_name(impl->display, impl->window, kOwnWindowTitle);
    (void)impl->map_window(impl->display, impl->window);
    (void)impl->flush(impl->display);
    return true;
#endif
}

void WindowSystem::closeOwnWindow() noexcept
{
#if defined(_WIN32)
    if (impl != nullptr && impl->window != nullptr) {
        DestroyWindow(impl->window);
        impl->window = nullptr;
    }
#elif !defined(__APPLE__)
    if (impl == nullptr) {
        return;
    }
    if (impl->window != 0UL && impl->display != nullptr && impl->destroy_window != nullptr) {
        (void)impl->destroy_window(impl->display, impl->window);
        impl->window = 0UL;
    }
    if (impl->display != nullptr && impl->close_display != nullptr) {
        (void)impl->close_display(impl->display);
        impl->display = nullptr;
    }
    if (impl->window_library != nullptr) {
        dlclose(impl->window_library);
        impl->window_library = nullptr;
        impl->open_display   = nullptr;
        impl->close_display  = nullptr;
        impl->default_root   = nullptr;
        impl->create_window  = nullptr;
        impl->store_name     = nullptr;
        impl->map_window     = nullptr;
        impl->flush          = nullptr;
        impl->destroy_window = nullptr;
    }
#endif
}

void* WindowSystem::ownWindow() const noexcept
{
#if defined(_WIN32)
    return impl != nullptr ? impl->window : nullptr;
#elif defined(__APPLE__)
    return nullptr;
#else
    return impl != nullptr && impl->window != 0UL ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(impl->window)) : nullptr;
#endif
}

VN_BACKEND_VK_NS_END
