#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <memory>

#include <volk.h>

#include <vine/String.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief One device session's window-system services: the connection surfaces are made on, the surface itself, and
 *        the window a `Probe` device brings up.
 *
 * @details
 * - WHY ONE OBJECT PER DEVICE. A window target needs two platform facts the Vulkan layer cannot invent: the
 *   connection a `VkSurfaceKHR` is created on, and the native handle it is created from. Both are the WINDOW SYSTEM's,
 *   not a target's - and the device-owned window is a third. Keeping them in one object gives the platform code ONE
 *   place to live and one lifetime to obey (surfaces first, then the window, then the connection).
 * - THE PLATFORM SEAM. This class is the seam a second platform joins through: today the implementation serves X11
 *   (xcb for the connection, Xlib for the device-owned window, both reached with `dlopen` so the plugin keeps no
 *   build-time X dependency); Win32 is the next implementation and needs no change here or in the device. A platform
 *   this build does not serve answers false with a reason rather than pretending - the same honesty
 *   `SurfaceTargetResult::UnsupportedSurface` carries.
 * - WHAT IT IS NOT. Not a window-system abstraction: no events are pumped, no resize tracked, no title bar managed.
 *   A Probe window exists only to be the place a swapchain presents to; a host that owns its window's lifecycle uses
 *   `DevicePresentation::Handle` and never touches this type.
 *
 * @note Not thread-safe: the device drives one frame at a time (the SDK's contract).
 *
 * @warning The connection must outlive every surface created through it: destroy the window targets, then
 *          @ref closeOwnWindow, then @ref close - which is the order `Device::shutdown` states.
 */
class VN_BACKEND_VK_API WindowSystem final {
  public:
    WindowSystem();

    /** @brief Closes the own window and the connection (idempotent; the surfaces must be gone first). */
    ~WindowSystem();

    WindowSystem(const WindowSystem&)            = delete;
    WindowSystem& operator=(const WindowSystem&) = delete;

  public:
    /**
     * @brief Gets the instance extension a surface needs on this build, or an empty string where none is served.
     *
     * @details
     * - Static because it is a fact about the BUILD, not a session: the device asks before an instance exists.
     *
     * @return `"VK_KHR_xcb_surface"` on X11, `"VK_KHR_win32_surface"` on Win32, `""` where no window is served.
     */
    [[nodiscard]] static const char* instanceExtension() noexcept;

    /**
     * @brief Opens what a surface needs (on X11, the display connection).
     *
     * @details
     * - Idempotent, and a no-op that answers true on platforms whose surface creation needs no connection (Win32).
     *
     * @param why Receives the reason when the connection cannot be opened, or null.
     * @return true when @ref createSurface can be called.
     */
    [[nodiscard]] bool open(vn::String* why);

    /** @brief Closes the connection (idempotent; every surface created through it must be destroyed first). */
    void close() noexcept;

    /**
     * @brief Creates the `VkSurfaceKHR` for @p window on @p instance.
     *
     * @details
     * - On X11 @p window is an `xcb_window_t` carried as a `void*` (the SDK's spelling); on Win32 it is an HWND and
     *   the module instance comes from the executable.
     * - @ref open must have answered true first on platforms that need a connection.
     *
     * @param instance The instance the surface belongs to.
     * @param window   Native window handle (not null).
     * @param surface  Receives the surface (`VK_NULL_HANDLE` on failure).
     * @param why      Receives the reason when it cannot be created, or null.
     * @return true when @p surface exists.
     */
    [[nodiscard]] bool createSurface(VkInstance instance, void* window, VkSurfaceKHR& surface, vn::String* why) const;

    /**
     * @brief Brings up the window a `Probe` device presents through, once per session.
     *
     * @details
     * - The window is MAPPED before this returns (a surface may be created on it immediately); whether the window
     *   manager has placed it yet changes nothing about presenting.
     * - Calling it on an open window is a no-op that answers true - the window is one object per session.
     *
     * @param width  Client width in pixels (must be positive).
     * @param height Client height in pixels (must be positive).
     * @param why    Receives the reason when no window came up (no display, no window-system library), or null.
     * @return true when @ref ownWindow answers a live handle.
     */
    [[nodiscard]] bool openOwnWindow(int width, int height, vn::String* why);

    /** @brief Destroys the device-owned window (idempotent; every surface created on it must be gone first). */
    void closeOwnWindow() noexcept;

    /** @brief Gets the device-owned window's native handle, or null when none was brought up. */
    [[nodiscard]] void* ownWindow() const noexcept;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
