#include <vine/backend_vk/internal/ProbeWindow.hpp>

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

/// @brief The window's title on every platform: a Probe window says which backend brought it up.
constexpr const char* kProbeWindowTitle = "Vine (vk)";

#if defined(_WIN32)
/**
 * @brief The window procedure of the classes a Probe window belongs to.
 *
 * @details
 * - A window without one cannot be created at all (CreateWindowExW refuses a class with no WndProc). This one does
 *   nothing: a Probe window is a presentation target, not a UI - the platform's default handling is the whole job,
 *   and a host that wants its window's messages is one that owns the window (`DevicePresentation::Handle`).
 *
 * @return The platform's default answer.
 */
LRESULT CALLBACK probeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}
#endif

} // namespace

struct ProbeWindow::Impl {
#if defined(_WIN32)
    HWND window{ nullptr };
#elif !defined(__APPLE__)
    void* library{ nullptr };      // libX11.so, opened once per window (dlclose'd with it).
    void* display{ nullptr };      // Display*
    unsigned long window{ 0UL };   // Window (XID)

    // The Xlib entry points the window needs, resolved with dlsym (see the header's "no build-time dependency").
    using OpenDisplayFn    = void* (*)(const char*);
    using CloseDisplayFn   = int (*)(void*);
    using DefaultRootFn    = unsigned long (*)(void*);
    using CreateWindowFn   = unsigned long (*)(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long);
    using StoreNameFn      = int (*)(void*, unsigned long, const char*);
    using MapWindowFn      = int (*)(void*, unsigned long);
    using FlushFn          = int (*)(void*);
    using DestroyWindowFn  = int (*)(void*, unsigned long);

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

ProbeWindow::ProbeWindow()
  : impl(std::make_unique<Impl>())
{
}

ProbeWindow::~ProbeWindow()
{
    close();
}

bool ProbeWindow::supported() noexcept
{
#if defined(__APPLE__)
    return false;
#else
    return true;
#endif
}

bool ProbeWindow::open(int width, int height, vn::String* why)
{
    if (handle() != nullptr) {
        return true; // one window per session: open is idempotent
    }
    if (width <= 0 || height <= 0) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a Probe window needs a positive size");
        }
        return false;
    }
#if defined(_WIN32)
    // ONE CLASS PER PROCESS, registered on first use: registering it again answers ERROR_CLASS_ALREADY_EXISTS, which
    // is the same state, not a failure.
    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.lpfnWndProc   = &probeWindowProc;
    window_class.hInstance     = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"VineVkProbeWindow";
    window_class.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the Probe window's class could not be registered");
        }
        return false;
    }
    WCHAR title[32]{};
    MultiByteToWideChar(CP_UTF8, 0, kProbeWindowTitle, -1, title, static_cast<int>(std::size(title)));
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
            *why = vn::String(u8"vk: the Probe window could not be created");
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
    impl->library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (impl->library == nullptr) {
        impl->library = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (impl->library == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libX11 is not available, so a device-owned window cannot be brought up (name a host window with Handle, or ask for None)");
        }
        return false;
    }
    impl->open_display   = reinterpret_cast<Impl::OpenDisplayFn>(dlsym(impl->library, "XOpenDisplay"));
    impl->close_display  = reinterpret_cast<Impl::CloseDisplayFn>(dlsym(impl->library, "XCloseDisplay"));
    impl->default_root   = reinterpret_cast<Impl::DefaultRootFn>(dlsym(impl->library, "XDefaultRootWindow"));
    impl->create_window  = reinterpret_cast<Impl::CreateWindowFn>(dlsym(impl->library, "XCreateSimpleWindow"));
    impl->store_name     = reinterpret_cast<Impl::StoreNameFn>(dlsym(impl->library, "XStoreName"));
    impl->map_window     = reinterpret_cast<Impl::MapWindowFn>(dlsym(impl->library, "XMapWindow"));
    impl->flush          = reinterpret_cast<Impl::FlushFn>(dlsym(impl->library, "XFlush"));
    impl->destroy_window = reinterpret_cast<Impl::DestroyWindowFn>(dlsym(impl->library, "XDestroyWindow"));
    if (impl->open_display == nullptr || impl->close_display == nullptr || impl->default_root == nullptr || impl->create_window == nullptr ||
        impl->store_name == nullptr || impl->map_window == nullptr || impl->flush == nullptr || impl->destroy_window == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: libX11 does not export the window entry points this backend needs");
        }
        close();
        return false;
    }
    impl->display = impl->open_display(nullptr);
    if (impl->display == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: no X display is reachable for a device-owned window (is DISPLAY set?)");
        }
        close();
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
            *why = vn::String(u8"vk: the Probe window could not be created");
        }
        close();
        return false;
    }
    // Mapped and flushed HERE: the surface may be created on this window as soon as open() returns, and a window
    // that is not mapped is what a compositor reports as an out-of-date surface on the first acquire.
    (void)impl->store_name(impl->display, impl->window, kProbeWindowTitle);
    (void)impl->map_window(impl->display, impl->window);
    (void)impl->flush(impl->display);
    return true;
#endif
}

void ProbeWindow::close() noexcept
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
    if (impl->library != nullptr) {
        dlclose(impl->library);
        impl->library      = nullptr;
        impl->open_display = nullptr;
        impl->close_display = nullptr;
        impl->default_root = nullptr;
        impl->create_window = nullptr;
        impl->store_name   = nullptr;
        impl->map_window   = nullptr;
        impl->flush        = nullptr;
        impl->destroy_window = nullptr;
    }
#endif
}

void* ProbeWindow::handle() const noexcept
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
