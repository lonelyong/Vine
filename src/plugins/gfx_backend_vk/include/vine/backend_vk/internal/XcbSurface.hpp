#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/String.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The xcb connection one window target creates its surface on, reached without a build-time XCB dependency.
 *
 * @details
 * - WHAT IT DOES. Opens the display named by `DISPLAY` (`xcb_connect`), and answers the `xcb_connection_t*` a
 *   `VkXcbSurfaceCreateInfoKHR` needs. XCB is a WINDOW-SYSTEM library, not something this backend links: the two
 *   symbols are resolved with `dlopen`, so a machine without libxcb reports "no window system" instead of failing to
 *   load the plugin (the same honesty `capabilities().presentation` is for).
 * - WHAT IT DOES NOT DO. It never creates a window: a window is a PLATFORM object with a lifetime of its own, and
 *   the two kinds (`DevicePresentation::Handle`'s host window, `Probe`'s device-owned one) are owned where they are
 *   made - by the host, or by `ProbeWindow` (see its own note). What this type answers is the CONNECTION a surface
 *   is created on.
 * - PLATFORM. The connection path exists for X11 only; Win32 has no connection object (the HWND with the module
 *   instance is the whole input), and macOS is not served (no surface extension is enabled there).
 */
class VN_BACKEND_VK_API XcbSurface final {
  public:
    /** @brief Builds an unopened connection. */
    XcbSurface();

    /** @brief Closes the connection (idempotent). */
    ~XcbSurface();

    XcbSurface(const XcbSurface&)            = delete;
    XcbSurface& operator=(const XcbSurface&) = delete;

  public:
    /**
     * @brief Opens the display's connection.
     *
     * @param why Receives the reason when the library/display is unavailable, or null.
     * @return true when @ref connection is a live connection.
     */
    [[nodiscard]] bool open(vn::String* why);

    /** @brief Closes the connection (the surface must be gone first). */
    void close() noexcept;

    /** @brief Gets the connection a `VkXcbSurfaceCreateInfoKHR` names (`nullptr` outside X11 or before @ref open). */
    [[nodiscard]] void* connection() const noexcept;

    /** @brief Answers whether this build can create an X11 surface at all. */
    [[nodiscard]] static bool supported() noexcept;

    /**
     * @brief Creates the VkSurfaceKHR for @p window on @p instance.
     *
     * @details
     * - On X11 the window is an `xcb_window_t` carried as a `void*` (the SDK's spelling); on Win32 it is an HWND and
     *   the module instance comes from the executable.
     *
     * @param instance The instance the surface belongs to.
     * @param window   Native window handle (`xcb_window_t` / HWND as `void*`; not null).
     * @param surface  Receives the surface.
     * @param why      Receives the reason when it cannot be created, or null.
     * @return true when @p surface exists.
     */
    [[nodiscard]] bool createSurface(VkInstance instance, void* window, VkSurfaceKHR& surface, vn::String* why) const;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
