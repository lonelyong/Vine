#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <memory>

#include <vine/String.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The window a `DevicePresentation::Probe` device brings up for itself.
 *
 * @details
 * - WHY IT EXISTS. Probe is the SDK's DEFAULT presentation ("nothing either, and the device brings up a window of
 *   its own when the host never asks for one" - see DeviceDesc), and a window is a PLATFORM object, not a Vulkan
 *   one: it has to exist before the instance, exactly like a host-supplied handle does, because the surface is
 *   created on it and the physical device is judged against that surface. This type owns that object for the whole
 *   session the way DeviceDesc::native_handle owns a host window for its host.
 * - WHAT IT OWNS. The display connection the window was made on and the window itself. On X11 the connection is
 *   Xlib's, reached with `dlopen` - the plugin keeps NO build-time X11 dependency (the same rule the xcb surface
 *   follows); on Win32 the window class is registered once per process and the window belongs to it; macOS is not
 *   served (no surface extension is enabled there, and `open` says so).
 * - WHAT IT IS NOT. Not a window-system abstraction: no events are pumped, no resize is tracked, no title bar is
 *   managed - the backend's window half is a SWAPCHAIN, and a Probe window is only the place that swapchain presents
 *   to. A host that owns its window's lifecycle uses `DevicePresentation::Handle` with a handle it made itself.
 * - ORDER. `close()` destroys the window, so every surface created on it must be gone first (see
 *   `Device::shutdown`, which destroys the window targets and then the Probe window).
 *
 * @note Not thread-safe: the device drives one frame at a time (the SDK's contract).
 */
class VN_BACKEND_VK_API ProbeWindow final {
  public:
    ProbeWindow();

    /** @brief Destroys the window and the connection it was made on (idempotent). */
    ~ProbeWindow();

    ProbeWindow(const ProbeWindow&)            = delete;
    ProbeWindow& operator=(const ProbeWindow&) = delete;

  public:
    /**
     * @brief Brings the window up at @p width x @p height.
     *
     * @details
     * - The window is MAPPED before this returns (a surface may be created on it immediately); whether the window
     *   manager has placed it yet is the platform's business and changes nothing about presenting.
     * - Calling it on an open window is a no-op that answers true - the window is one object per session, not a
     *   factory.
     *
     * @param width  Client width in pixels (must be positive).
     * @param height Client height in pixels (must be positive).
     * @param why    Receives the reason when no window came up (no display, no window-system library), or null.
     * @return true when the window exists and @ref handle is valid.
     */
    [[nodiscard]] bool open(int width, int height, vn::String* why);

    /** @brief Destroys the window and closes its connection (idempotent; the surfaces must be gone first). */
    void close() noexcept;

    /** @brief Gets the native handle a surface is created from (`xcb_window_t`-shaped on X11, HWND on Win32). */
    [[nodiscard]] void* handle() const noexcept;

    /** @brief Gets whether this build can bring a window up at all (the library itself may still be missing). */
    [[nodiscard]] static bool supported() noexcept;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
