#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>
#include <vine/graphics/backend/TargetPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

class WindowSystem;

/** @brief The device state one window target needs: the Vulkan objects the DEVICE owns (borrowed, never destroyed). */
struct WindowDevice {
    const VolkDeviceTable*           table{ nullptr };           // The device's entry points (volk table).
    VkInstance                       instance{ VK_NULL_HANDLE };  // Instance the surface is created on.
    VkPhysicalDevice                 physical{ VK_NULL_HANDLE };  // For the surface's capabilities/formats.
    VkDevice                         device{ VK_NULL_HANDLE };    // Logical device the swapchain belongs to.
    VkQueue                          queue{ VK_NULL_HANDLE };     // The queue that presents (graphics + present).
    std::uint32_t                    queue_family{ 0U };          // The family the queue belongs to.
    VkPhysicalDeviceMemoryProperties memory{};                    // For the depth image's memory.
};

/**
 * @brief A window's swapchain on the direct-Vulkan backend: the surface, its images, one render pass and the
 *        per-image framebuffers.
 *
 * @details
 * - ONE LIVE TARGET PER WINDOW, which is the platform constraint `WindowPresentTarget` states: the device hands one
 *   out (see `Device::createSurfaceTarget`) and refuses a second for the same handle.
 * - WHAT IT OWNS. The `VkSurfaceKHR` (created through the device's window system; Win32 uses the host module), the
 *   swapchain and its images/views, one D24 depth image, TWO render passes (colour CLEAR / colour LOAD - a load op is
 *   not part of compatibility) and one framebuffer per swapchain image.
 * - WHAT IT BORROWS. The `VkInstance`, `VkDevice` and queue: the device owns them and hands the target a
 *   borrowed `WindowDevice` (see @ref attach), so the target never destroys what it did not make. The CONNECTION a
 *   surface is created on is borrowed the same way, from the device's `WindowSystem` - one per session, outliving
 *   every target made through it.
 * - LIFECYCLE. @ref attach is called once the logical device exists; the first @ref acquire builds the swapchain, and
 *   a resize or an out-of-date answer rebuilds it (`vkDeviceWaitIdle` first, then the old swapchain is replaced).
 *   A window the platform destroyed under the target leaves it DETACHED: every later call answers the truth instead
 *   of touching a dead surface.
 * - THE WINDOW PIXELS ARE THE SWAPCHAIN'S. A presented image cannot be read back (that is the platform's picture),
 *   which is why `readColorBuffer` stays for off-screen targets only; a test that wants the pixels captures the
 *   window, not the target.
 *
 * @warning Not thread-safe: the device drives one frame at a time (the SDK's contract).
 */
class VN_BACKEND_VK_API WindowTarget final : public vn::graphics::WindowPresentTarget {
  public:
    /**
     * @brief Creates a target for @p native_handle, without a surface yet (see @ref attach).
     *
     * @param window_system The device's window-system services (borrowed; outlives this target).
     * @param native_handle The host window, or the device-owned window's handle for a `Probe` device.
     */
    WindowTarget(WindowSystem& window_system, void* native_handle);

    /** @brief Destroys the swapchain and the surface (the borrowed connection is the device's). */
    ~WindowTarget() override;

    WindowTarget(const WindowTarget&)            = delete;
    WindowTarget& operator=(const WindowTarget&) = delete;

  public:
    /**
     * @brief Creates the surface on @p instance (the step that must happen before the logical device exists).
     *
     * @param instance The instance the surface belongs to.
     * @param why      Receives the reason when the surface cannot be created, or null.
     * @return true when the surface exists and the device may be brought up for it.
     */
    [[nodiscard]] bool createSurface(VkInstance instance, vn::String* why);

    /**
     * @brief Gives the target the device objects it borrows, then creates the surface and the swapchain.
     *
     * @param device The device's handles (borrowed; outlives this target).
     * @param why    Receives the reason when the surface/swapchain cannot be created, or null.
     * @return true when the target can present.
     */
    [[nodiscard]] bool attach(const WindowDevice& device, vn::String* why);

    /** @brief Answers whether a swapchain and its render passes currently exist. */
    [[nodiscard]] bool ready() const noexcept;

    /** @brief Gets the surface the target presents through (`VK_NULL_HANDLE` when it has none). */
    [[nodiscard]] VkSurfaceKHR surfaceHandle() const noexcept;

    /**
     * @brief Acquires the next image and makes sure the swapchain objects exist.
     *
     * @details
     * - Called once per frame by the device, before the window's passes are recorded. An out-of-date surface is
     *   rebuilt here (once per call); a detached or failed target answers false and the frame presents nothing.
     *
     * @param why Receives the reason when no image could be acquired, or null.
     * @return true when @ref imageIndex names a recorded image.
     */
    [[nodiscard]] bool acquire(vn::String* why);

    /** @brief Gets the render pass a window pass records with (colour and depth load ops as stated). */
    [[nodiscard]] VkRenderPass renderPass(bool load_color, bool load_depth) const noexcept;

    /** @brief Gets the framebuffer of the acquired image. */
    [[nodiscard]] VkFramebuffer framebuffer() const noexcept;

    /** @brief Gets the acquired image's index (valid between @ref acquire and @ref present). */
    [[nodiscard]] std::uint32_t imageIndex() const noexcept;

    /** @brief Gets the swapchain's current extent in pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept;
    /** @brief Gets the swapchain's current height in pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept;

    /** @brief Gets the pipeline compatibility of the window's attachment set. */
    [[nodiscard]] vn::graphics::backend::RenderPassCompatibility compatibility() const noexcept;

    /** @brief Gets the semaphore a submit must wait on (signalled by the acquire). */
    [[nodiscard]] VkSemaphore imageAvailableSemaphore() const noexcept;
    /** @brief Gets the semaphore a submit must signal when the window's commands finished. */
    [[nodiscard]] VkSemaphore renderFinishedSemaphore() const noexcept;

    /**
     * @brief Records the transition from the attachment layout to the one presenting requires.
     *
     * @details
     * - Called by the device after the window's last pass, before the command buffer ends: the render passes leave
     *   the image in `COLOR_ATTACHMENT_OPTIMAL`, and `vkQueuePresentKHR` needs `PRESENT_SRC_KHR`.
     *
     * @param commands The command buffer the frame is being recorded into (outside any render pass).
     */
    void recordPresentTransition(VkCommandBuffer commands) const;

    /** @brief Presents the acquired image (the submit that produced it must have completed). */
    [[nodiscard]] vn::graphics::PresentOutcome present() override;

  public:
    /** @brief Announces a surface size; the swapchain follows it on the next acquire. */
    void resize(int width, int height) override;

    /**
     * @brief Moves the target onto @p native_handle.
     *
     * @details
     * - A platform recreation is a MOVE: the surface is re-created on the same instance and the swapchain is rebuilt;
     *   the device, its caches and every pipeline stay. A null handle detaches the target (the window is gone).
     */
    void setWindowHandle(void* native_handle) override;

    /** @brief Gets the handle the target is attached to (null when detached). */
    [[nodiscard]] void* nativeHandle() const override;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
