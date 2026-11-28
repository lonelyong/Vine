#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>
#include <span>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/backend_vk/internal/DynamicRendering.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/WindowPresentTarget.hpp>
#include <vine/graphics/TargetPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

class WindowSystem;

/** @brief The device state one window target needs: the Vulkan objects the DEVICE owns (borrowed, never destroyed). */
struct WindowDevice {
    const VolkDeviceTable* table{ nullptr };           // The device's entry points (volk table).
    VkInstance             instance{ VK_NULL_HANDLE }; // Instance the surface is created on.
    VkPhysicalDevice       physical{ VK_NULL_HANDLE }; // For the surface's capabilities/formats.
    VkDevice               device{ VK_NULL_HANDLE };   // Logical device the swapchain belongs to.
    VkQueue                queue{ VK_NULL_HANDLE };    // The queue that presents (graphics + present).
    std::uint32_t          queue_family{ 0U };         // The family the queue belongs to.
    MemoryAllocator*       allocator{ nullptr };       // The device's memory pool (the depth image's slice).
};

/**
 * @brief A window's swapchain on the direct-Vulkan backend: the surface, its images and the depth they are drawn with.
 *
 * @details
 * - ONE LIVE TARGET PER WINDOW, which is the platform constraint `WindowPresentTarget` states: the device hands one
 *   out (see `Device::createSurfaceTarget`) and refuses a second for the same handle.
 * - WHAT IT OWNS. The `VkSurfaceKHR` (created through the device's window system; Win32 uses the host module), the
 *   swapchain and its images/views, and one D24 depth image shared by every acquired image.
 * - NO RENDER PASS, NO FRAMEBUFFER. Dynamic rendering states the attachments on the command buffer, so a window pass
 *   needs only the acquired image, the depth image and the layout each one is in - which is why this class records its
 *   own attachment transitions (see @ref beginPass) instead of owning a family of render passes.
 * - WHAT IT BORROWS. The `VkInstance`, `VkDevice` and queue: the device owns them and hands the target a
 *   borrowed `WindowDevice` (see @ref attach), so the target never destroys what it did not make. The CONNECTION a
 *   surface is created on is borrowed the same way, from the device's `WindowSystem` - one per session, outliving
 *   every target made through it.
 * - LIFECYCLE. @ref attach is called once the logical device exists; the first @ref acquire builds the swapchain, and
 *   a resize or an out-of-date answer rebuilds it (`vkDeviceWaitIdle` first, then the old swapchain is replaced).
 *   A REBUILD THAT FAILS IS A STATE, NOT AN ENDING: the old chain is destroyed before the new one is built, so a
 *   failure leaves the target attached with a chain OWED, and the next @ref acquire builds it (see @ref ready).
 *   A window the platform destroyed under the target leaves it DETACHED: every later call answers the truth instead
 *   of touching a dead surface.
 * - THE WINDOW PIXELS ARE THE SWAPCHAIN'S. A presented image cannot be read back (that is the platform's picture),
 *   which is why `readColorImage` stays for off-screen targets only; a test that wants the pixels captures the
 *   window, not the target.
 *
 * @warning Not thread-safe: the device drives one frame at a time (the SDK's contract).
 */
class VN_BACKEND_VK_API WindowTarget final : public vn::graphics::WindowPresentTarget {
  public:
    /** @brief Why this target cannot present right now (see @ref presentationState). */
    enum class PresentationState : std::uint8_t
    {
        Ready,       // A swapchain and the depth image it draws with exist.
        Detached,    // No live host window: setWindowHandle(nullptr), or the move to a new handle could not be rebuilt.
        NotAttached, // attach() was never called, or it was refused.
        NoSwapchain, // Attached, but the last swapchain (re)build produced none: the next acquire builds one.
    };

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
    [[nodiscard]] bool createArea(VkInstance instance, vn::String* why);

    /**
     * @brief Gives the target the device objects it borrows, then creates the surface and the swapchain.
     *
     * @param device The device's handles (borrowed; outlives this target).
     * @param why    Receives the reason when the surface/swapchain cannot be created, or null.
     * @return true when the target can present.
     */
    [[nodiscard]] bool attach(const WindowDevice& device, vn::String* why);

    /** @brief Answers whether a swapchain and its images currently exist. */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Gets why @ref ready() answers what it does: the window's own state, as a value.
     *
     * @details
     * - THE WINDOW KNOWS WHY ITS PASSES CANNOT RUN; the device owns the report (`vk.window.not_ready`).
     *   "Detached", "never attached" and "the last swapchain build produced none" all leave @ref ready false, and a
     *   host reading a missing picture had no way to tell them apart from the sentence that used to accuse it of
     *   naming a foreign presenter (review 19 `Z14`).
     * - IT READS, IT DOES NOT ASK THE DEVICE: every fact it names is state this target already holds.
     *
     * @return #PresentationState::Ready when @ref ready() is true; otherwise the fact that stops it.
     */
    [[nodiscard]] PresentationState presentationState() const noexcept;

    /**
     * @brief Answers whether the device was lost through this window (an acquire or present answered so).
     *
     * @details
     * - A LOST DEVICE IS NOT A WINDOW PROBLEM: the swapchain cannot be recreated to fix it, so nothing here asks for
     *   a recreate once this is true. The device reads this after a failed acquire or a `Lost` present to classify
     *   the outcome once (see `Device::noteDeviceLost`) instead of leaving a host to guess from a missing picture.
     *
     * @return true when a queue call made on this window's behalf reported the device gone.
     */
    [[nodiscard]] bool deviceLost() const noexcept;

    /** @brief Gets the surface the target presents through (`VK_NULL_HANDLE` when it has none). */
    [[nodiscard]] VkSurfaceKHR surfaceHandle() const noexcept;

    /**
     * @brief Acquires the next image, building the swapchain first when the target owes one.
     *
     * @details
     * - Called once per frame by the device, before the window's passes are recorded. It BUILDS WHAT THE TARGET OWES:
     *   the first chain, the chain a resize asked for, and the chain a failed rebuild left missing (the rebuild
     *   destroys the old chain before it builds the new one, so a failure leaves none - review 20 `Z23`).
     * - ONLY A WINDOW THIS TARGET CANNOT PRESENT ON REFUSES: detached, or never attached. "There is no chain right
     *   now" is not that - asking it as a guard is what used to make one failed rebuild permanent.
     * - An out-of-date answer rebuilds and asks once more; a SUBOPTIMAL one is an image with a rebuild owed to the next
     *   call (the same meaning @ref present gives it), so the frame draws and the chain follows on the next frame.
     *
     * @param why Receives the reason when no image could be acquired, or null.
     * @return true when @ref imageIndex names a recorded image.
     */
    [[nodiscard]] bool acquire(vn::String* why);

    /**
     * @brief Records the acquired image's and the depth image's attachment transitions, then opens the rendering instance.
     *
     * @details
     * - THE TWO LAYOUTS THIS TARGET TRACKS (see the class note): the ACQUIRED IMAGE is in `PRESENT_SRC_KHR` after it was
     *   presented, in `UNDEFINED` on its first use, and in the colour attachment layout between two passes of one frame;
     *   the shared DEPTH image is `UNDEFINED` on its first use and a depth attachment afterwards. A per-image record is
     *   what makes "the first use" exact - a swapchain image the presentation engine has never handed out is not the
     *   one that was presented last frame.
     * - THE LOAD POLICY IS THE PASS' OWN (see `AttachmentPolicy`'s two independent dimensions): the first pass into an image
     *   that has no earlier pass this frame CLEARs, and a later pass keeps what the earlier one wrote unless it asks to
     *   reset an attachment.
     *
     * @param commands The command buffer being recorded (outside any rendering instance).
     * @param load     The pass' load policy and clear values (one colour attachment, one depth attachment).
     * @param tally    Receives what the pass' entry dependency cost (see `RecordingTally`), or null.
     *
     * @warning The caller acquired an image first (@ref acquire) and ends the pass with @ref endPass.
     */
    void beginPass(VkCommandBuffer commands, const PassAttachmentOps& load, RecordingTally* tally = nullptr);

    /**
     * @brief Closes the rendering instance @ref beginPass opened.
     *
     * @details
     * - The pair is symmetric: a call that follows a @ref beginPass which opened nothing (no image acquired) closes
     *   nothing, so a frame can never record `vkCmdEndRendering` without a matching instance.
     *
     * @param commands The command buffer the pass was recorded into.
     */
    void endPass(VkCommandBuffer commands) const noexcept;

    /** @brief Gets the acquired image's index (valid between @ref acquire and @ref present). */
    [[nodiscard]] std::uint32_t imageIndex() const noexcept;

    /** @brief Gets the swapchain's current extent in pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept;
    /** @brief Gets the swapchain's current height in pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept;

    /** @brief Gets the pipeline compatibility of the window's attachment set. */
    [[nodiscard]] vn::graphics::RenderPassCompatibility compatibility() const noexcept;

    /** @brief Gets the semaphore a submit must wait on (signalled by the acquire). */
    [[nodiscard]] VkSemaphore imageAvailableSemaphore() const noexcept;
    /** @brief Gets the semaphore a submit must signal when the window's commands finished. */
    [[nodiscard]] VkSemaphore renderFinishedSemaphore() const noexcept;

    /**
     * @brief Records the transition from the attachment layout to the one presenting requires.
     *
     * @details
     * - Called by the device after the window's last pass, before the command buffer ends: the passes leave the image in
     *   `COLOR_ATTACHMENT_OPTIMAL`, and `vkQueuePresentKHR` needs `PRESENT_SRC_KHR`.
     * - IT UPDATES THE BOOKKEEPING: the acquired image is then in the present layout, which is what the next acquire of
     *   that image states as its starting point (see @ref beginPass).
     *
     * @param commands The command buffer the frame is being recorded into (outside any rendering instance).
     */
    void recordPresentTransition(VkCommandBuffer commands);

    /** @brief Presents the acquired image (the submit that produced it must have completed). */
    [[nodiscard]] vn::graphics::PresentOutcome present() override;

    /**
     * @brief Abandons this frame's acquired image without presenting it, because the frame never reached the queue.
     *
     * @details
     * - AN ACQUIRE IS A PAIR OR IT IS A LEAK. `vkAcquireNextImageKHR` took an image and SIGNALLED `image_available`;
     *   the pair is a submit that waits on that semaphore and a present that hands the image back. A frame that failed
     *   between the acquire and its submission (a command buffer that would not end, a submit the driver refused) has
     *   done neither, and the next acquire may not hand that same signalled semaphore to the driver again
     *   (`VUID-vkAcquireNextImageKHR-semaphore-01286`) - nor is the image ours to keep.
     * - THE ANSWER IS THE PATH A RESIZE ALREADY TAKES: the swapchain is rebuilt. Destroying it returns the acquired
     *   image to the presentation engine, the rebuilt chain starts every record at `Undefined` (see
     *   `createSwapchainObjects`), and the semaphores are fresh - so nothing is left signalled and no stale layout
     *   survives. It costs one device idle on a path that is already a failure; a steady frame never sees it.
     * - The DEVICE decides when: the frame body calls this on the paths where the command buffer did not run (see
     *   `Device::FrameDriver::executeFrame`), and only while an image is acquired.
     */
    void abandonFrame() noexcept;

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

/**
 * @brief Why a surface allows no swapchain this backend can draw into (see `surfaceSwapchain`).
 */
enum class SurfaceRefusal : std::uint8_t
{
    None,          ///< The surface allows one; every field of `SurfaceSwapchain` may be requested.
    NoFormats,     ///< The surface offers no format pair at all.
    ZeroExtent,    ///< The extent the surface dictates (or clamps to) is zero - a minimised window.
    NoColorUsage,  ///< The surface's images cannot be colour attachments, so nothing can be drawn into them.
};

/**
 * @brief What ONE swapchain of this surface may be: the surface's answers resolved into one request.
 *
 * @details
 * - THE SURFACE IS THE AUTHORITY (review 29). `VkSurfaceCapabilitiesKHR` states the extent the presentation engine
 *   presents at, the image count it allows, the composite modes it supports and the usages its images permit, and the
 *   format list states the format pairs it prefers. This value is those answers RESOLVED: every field is one the
 *   surface states, so `VkSwapchainCreateInfoKHR` is filled from it without a second check anywhere else.
 * - THE ONE ANSWER IT REFUSES TO RESOLVE is a surface that cannot be drawn into (see @ref refusal): every pass this
 *   backend records draws into the window's image, so a surface without colour-attachment usage has no window target,
 *   and that is a named refusal rather than a driver error whose sentence names nothing.
 */
struct SurfaceSwapchain {
    VkSurfaceFormatKHR          format{};      ///< The format pair to request; never `VK_FORMAT_UNDEFINED`.
    VkExtent2D                  extent{};      ///< The image extent to request (the surface's own, or the announced size clamped into its range).
    std::uint32_t               image_count{ 0U }; ///< At least the surface's minimum, at most its maximum (when it states one).
    VkCompositeAlphaFlagBitsKHR composite_alpha{ VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR }; ///< A mode `supportedCompositeAlpha` states.
    SurfaceRefusal              refusal{ SurfaceRefusal::None }; ///< `None` when every field above may be requested.

    /** @brief Answers whether the surface allows a swapchain at all, i.e. whether the fields above are usable. */
    [[nodiscard]] bool ok() const noexcept
    {
        return refusal == SurfaceRefusal::None;
    }
};

/**
 * @brief Resolves a surface's answers into the swapchain ONE window may request (see `SurfaceSwapchain`).
 *
 * @details
 * - EXTENT: a windowed surface states `currentExtent` and the chain follows it (the presentation engine owns the
 *   window's size); a free surface (`currentExtent` is `UINT32_MAX`) takes the announced size CLAMPED into
 *   `minImageExtent`..`maxImageExtent`, and an unannounced size falls back to 64x64 before that clamp.
 * - FORMAT: the surface's preferred 8-bit sRGB pair, else the first pair it lists; the `VK_FORMAT_UNDEFINED` sentinel
 *   (the spec's "no preferred format" answer) becomes the classic `B8G8R8A8_UNORM` + `SRGB_NONLINEAR` pair, because
 *   `VkSwapchainCreateInfoKHR::imageFormat` may not be `VK_FORMAT_UNDEFINED`.
 * - COUNT: `minImageCount + 1` (one image presenting while the next is drawn), capped by `maxImageCount` when the
 *   surface states a cap (0 means none).
 * - COMPOSITE ALPHA: `OPAQUE` when supported, else the first mode the surface states in POST / PRE / INHERIT order -
 *   the picture is the same, so a stated mode beats refusing.
 * - What it never resolves is a surface that cannot be drawn into (see `SurfaceRefusal::NoColorUsage`).
 *
 * @param capabilities  The surface's answers (`vkGetPhysicalDeviceSurfaceCapabilitiesKHR`).
 * @param formats       The format pairs the surface offers, in its preferred order.
 * @param wanted_width  The announced width for a free surface (0 takes the 64-pixel fallback).
 * @param wanted_height The announced height for a free surface (0 takes the 64-pixel fallback).
 * @return The request, or the refusal that says why the surface allows none.
 */
[[nodiscard]] SurfaceSwapchain surfaceSwapchain(const VkSurfaceCapabilitiesKHR& capabilities,
                                                std::span<const VkSurfaceFormatKHR> formats,
                                                std::uint32_t wanted_width,
                                                std::uint32_t wanted_height) noexcept;

VN_BACKEND_VK_NS_END
