#include <vine/backend_vk/internal/WindowTarget.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <volk.h>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/TargetObjects.hpp>
#include <vine/backend_vk/internal/WindowSystem.hpp>
#include <vine/graphics/RenderTarget.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The depth format every window target uses (the same D24 the off-screen targets use).
constexpr VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

/// @brief How long an acquire waits for the presentation engine before the frame is skipped (see WindowTarget::acquire).
constexpr std::uint64_t kAcquireTimeoutNanoseconds = 1'000'000'000ULL;

/**
 * @brief Chooses the swapchain format: an 8-bit sRGB format when the surface offers one.
 *
 * @details
 * - sRGB is what the engine's linear shading wants a window to decode (the off-screen targets are the linear
 *   spellings); a surface without one still presents, in its first offered format.
 *
 * @param formats The surface's formats (not empty).
 * @return The chosen format and colour space.
 */
[[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) noexcept
{
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
            (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB)) {
            return format;
        }
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

} // namespace

struct WindowTarget::Impl {
    WindowDevice device{};
    bool         attached{ false };
    bool         detached{ false };
    void*        handle{ nullptr };

    /// The device's window-system services: the connection a surface is created on (borrowed, never closed here).
    WindowSystem* window_system{ nullptr };
    VkInstance    instance{ VK_NULL_HANDLE };
    VkSurfaceKHR  surface{ VK_NULL_HANDLE };

    VkSwapchainKHR swapchain{ VK_NULL_HANDLE };
    VkFormat       format{ VK_FORMAT_UNDEFINED };
    VkColorSpaceKHR color_space{ VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    VkExtent2D     extent{ 64U, 64U };
    int            requested_width{ 0 };
    int            requested_height{ 0 };
    bool           needs_recreate{ false };
    /// True once an acquire or present answered `VK_ERROR_DEVICE_LOST`: the session is over, and no swapchain can
    /// fix it (see WindowTarget::deviceLost).
    bool           device_lost{ false };

    std::vector<VkImage>     images;
    std::vector<VkImageView> views;

    VkImage       depth{ VK_NULL_HANDLE };
    MemorySlice   depth_memory{};
    VkImageView   depth_view{ VK_NULL_HANDLE };

    /// The layout each swapchain image is in, indexed like @ref images (see WindowTarget::beginPass): `Undefined` on its
    /// first use, `Present` once it has been presented, the colour attachment layout in between. PER IMAGE because the
    /// presentation engine hands out a different one each frame, and "never used yet" is not a fact about the target.
    std::vector<ImageLayout> image_layouts;
    /// The layout the shared depth image is in: `Undefined` on its first use, a depth attachment afterwards.
    ImageLayout depth_layout{ ImageLayout::Undefined };

    VkSemaphore image_available{ VK_NULL_HANDLE };
    /// ONE PRESENT-WAIT SEMAPHORE PER SWAPCHAIN IMAGE, indexed by the acquired image (see renderFinishedOfAcquiredImage).
    ///
    /// @note
    /// - WHY NOT ONE: `vkQueuePresentKHR` keeps the semaphore until the presentation engine is done with the image, and
    ///   an image may be presented and NOT re-acquired for several frames. Signalling one semaphore again before that
    ///   is a reuse the spec forbids (VUID-vkQueueSubmit-pSignalSemaphores-00067), which the validation layer reports
    ///   as "may still be in use by VkSwapchainKHR" - measured in the app. One per image makes each signal happen only
    ///   after that image's previous present has been consumed by the next acquire.
    std::vector<VkSemaphore> render_finished;

    std::uint32_t image_index{ 0U };
    bool          image_acquired{ false };
    /// True between @ref beginPass and @ref endPass in the file's own terms: it records whether a rendering instance is
    /// open, so the pair cannot close one that was never opened (see WindowTarget::beginPass' guard).
    bool          pass_open{ false };

    /** @brief Gets the present-wait semaphore of the image acquired NOW (`VK_NULL_HANDLE` when none is). */
    [[nodiscard]] VkSemaphore renderFinishedOfAcquiredImage() const noexcept
    {
        return image_acquired && image_index < render_finished.size() ? render_finished[image_index] : VK_NULL_HANDLE;
    }

    /** @brief Creates the swapchain's views, depth image and layout bookkeeping for the current images. */
    [[nodiscard]] bool createSwapchainObjects(vn::String* why)
    {
        // EVERY IMAGE STARTS UNDEFINED: a swapchain image nobody has presented is in `UNDEFINED` on its first use, and
        // which one the presentation engine hands out next is its choice - so the record is per image (see beginPass).
        image_layouts.assign(images.size(), ImageLayout::Undefined);
        depth_layout = ImageLayout::Undefined;
        views.resize(images.size(), VK_NULL_HANDLE);

        // THE ACQUIRE SEMAPHORE IS REBUILT WITH THE CHAIN, AND THAT IS THE REPAIR FOR AN ABANDONED FRAME: `abandonFrame`
        // gives the acquired image back without presenting it, so the signal `vkAcquireNextImageKHR` left on this
        // semaphore is never consumed - and the next acquire requires an UNSIGNALLED one
        // (VUID-vkAcquireNextImageKHR-semaphore-01286). Every path here ran after `destroySwapchainWhenIdle`, so nothing
        // waits on the old object and destroying it is legal.
        if (image_available != VK_NULL_HANDLE) {
            device.table->vkDestroySemaphore(device.device, image_available, nullptr);
            image_available = VK_NULL_HANDLE;
        }
        {
            VkSemaphoreCreateInfo acquire_info{};
            acquire_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (device.table->vkCreateSemaphore(device.device, &acquire_info, nullptr, &image_available) != VK_SUCCESS) {
                image_available = VK_NULL_HANDLE;
                if (why != nullptr) {
                    *why = vn::String(u8"vk: the window's acquire semaphore could not be created");
                }
                return false;
            }
        }
        for (std::size_t index = 0; index < images.size(); ++index) {
            VkImageViewCreateInfo view{};
            view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image                           = images[index];
            view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view.format                          = format;
            view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount     = 1U;
            view.subresourceRange.layerCount     = 1U;
            if (device.table->vkCreateImageView(device.device, &view, nullptr, &views[index]) != VK_SUCCESS) {
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a swapchain image view could not be created");
                }
                return false;
            }
        }

        // ONE PRESENT-WAIT SEMAPHORE PER IMAGE (see render_finished): created with the images they belong to, so a
        // rebuilt swapchain (a resize) gets a fresh set and nothing names a semaphore the presentation engine holds.
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        render_finished.assign(images.size(), VK_NULL_HANDLE);
        for (std::size_t index = 0; index < images.size(); ++index) {
            if (device.table->vkCreateSemaphore(device.device, &semaphore_info, nullptr, &render_finished[index]) != VK_SUCCESS) {
                render_finished[index] = VK_NULL_HANDLE;
                if (why != nullptr) {
                    *why = vn::String(u8"vk: the window's present semaphores could not be created");
                }
                return false;
            }
        }

        // THE DEPTH IMAGE: one image shared by every swapchain image (the first pass of every frame clears it).
        VkImageCreateInfo depth_info{};        depth_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_info.imageType     = VK_IMAGE_TYPE_2D;
        depth_info.extent        = VkExtent3D{ extent.width, extent.height, 1U };
        depth_info.mipLevels     = 1U;
        depth_info.arrayLayers   = 1U;
        depth_info.format        = kDepthFormat;
        depth_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depth_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        depth_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (device.table->vkCreateImage(device.device, &depth_info, nullptr, &depth) != VK_SUCCESS) {
            depth = VK_NULL_HANDLE;
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's depth image could not be created");
            }
            return false;
        }

        VkMemoryRequirements requirements{};
        device.table->vkGetImageMemoryRequirements(device.device, depth, &requirements);
        depth_memory = device.allocator->allocate(requirements, MemoryUsage::DeviceLocal, MemoryKind::Optimal);
        if (!depth_memory.valid()) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the pool could not serve the window's depth image");
            }
            return false;
        }
        if (device.table->vkBindImageMemory(device.device, depth, depth_memory.memory, depth_memory.offset) != VK_SUCCESS) {
            device.allocator->release(depth_memory);
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's depth image memory could not be bound");
            }
            return false;
        }

        VkImageViewCreateInfo depth_view_info{};
        depth_view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depth_view_info.image                           = depth;
        depth_view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        depth_view_info.format                          = kDepthFormat;
        depth_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_view_info.subresourceRange.levelCount     = 1U;
        depth_view_info.subresourceRange.layerCount     = 1U;
        if (device.table->vkCreateImageView(device.device, &depth_view_info, nullptr, &depth_view) != VK_SUCCESS) {
            depth_view = VK_NULL_HANDLE;
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's depth view could not be created");
            }
            return false;
        }

        // NO RENDER PASS AND NO FRAMEBUFFER: a window pass states these attachments on the command buffer (see
        // beginPass). The formats a pipeline is built for come from `compatibility()`, not from a structure.
        return true;
    }

    /**
     * @brief Ends the queue's use of the swapchain, then destroys it (idempotent).
     *
     * @details
     * - THE WAIT IS THE POINT: a submitted batch still refers to the swapchain and to its per-image present
     *   semaphores, so destroying either while the queue holds them is undefined behaviour - validation says so by
     *   name (`VUID-vkDestroySemaphore-semaphore-05149`, found by moving a window onto a new handle).
     * - Every teardown goes through this, never through destroySwapchain(): a host verb and a session teardown are
     *   both moments when frames may be in flight, and `createSwapchain` needs the same idle before it replaces the
     *   chain. The wait costs one stall per window move / resize / shutdown - never a steady frame.
     */
    void destroySwapchainWhenIdle() noexcept
    {
        if (device.device != VK_NULL_HANDLE && device.table != nullptr) {
            (void)device.table->vkDeviceWaitIdle(device.device);
        }
        destroySwapchain();
    }

    /** @brief Destroys the swapchain and everything hanging off it (idempotent).
     *
     * @warning The caller has proved the queue names none of it (see destroySwapchainWhenIdle).
     */
    void destroySwapchain() noexcept
    {
        if (device.device != VK_NULL_HANDLE && device.table != nullptr) {
            if (depth_view != VK_NULL_HANDLE) {
                device.table->vkDestroyImageView(device.device, depth_view, nullptr);
                depth_view = VK_NULL_HANDLE;
            }
            if (depth != VK_NULL_HANDLE) {
                device.table->vkDestroyImage(device.device, depth, nullptr);
                depth = VK_NULL_HANDLE;
            }
            if (device.allocator != nullptr) {
                device.allocator->release(depth_memory);
            }
            for (const VkImageView view : views) {
                if (view != VK_NULL_HANDLE) {
                    device.table->vkDestroyImageView(device.device, view, nullptr);
                }
            }
            views.clear();
            for (const VkSemaphore semaphore : render_finished) {
                if (semaphore != VK_NULL_HANDLE) {
                    device.table->vkDestroySemaphore(device.device, semaphore, nullptr);
                }
            }
            render_finished.clear();
            if (swapchain != VK_NULL_HANDLE) {
                device.table->vkDestroySwapchainKHR(device.device, swapchain, nullptr);
                swapchain = VK_NULL_HANDLE;
            }
            images.clear();
            image_layouts.clear();
            depth_layout = ImageLayout::Undefined;
        }
        image_acquired = false;
    }

    /** @brief Creates (or recreates) the swapchain from the surface's current capabilities. */
    [[nodiscard]] bool createSwapchain(vn::String* why)
    {
        if (!attached || detached || surface == VK_NULL_HANDLE || device.device == VK_NULL_HANDLE) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window target has no surface or device to build a swapchain on");
            }
            return false;
        }

        destroySwapchainWhenIdle();

        VkSurfaceCapabilitiesKHR capabilities{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical, surface, &capabilities) != VK_SUCCESS) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window surface reported no capabilities");
            }
            return false;
        }

        std::uint32_t format_count = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical, surface, &format_count, nullptr) != VK_SUCCESS || format_count == 0U) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window surface offers no formats");
            }
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical, surface, &format_count, formats.data()) != VK_SUCCESS) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window surface's formats could not be read");
            }
            return false;
        }
        const VkSurfaceFormatKHR chosen = chooseSurfaceFormat(formats);
        format                          = chosen.format;
        color_space                     = chosen.colorSpace;

        // EXTENT: a windowed surface states one (and follows it); a free one takes the announced size.
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            extent = capabilities.currentExtent;
        }
        else {
            const std::uint32_t wanted_width  = requested_width > 0 ? static_cast<std::uint32_t>(requested_width) : 64U;
            const std::uint32_t wanted_height = requested_height > 0 ? static_cast<std::uint32_t>(requested_height) : 64U;
            extent.width  = std::clamp(wanted_width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(wanted_height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        if (extent.width == 0U || extent.height == 0U) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window surface has a zero extent");
            }
            return false;
        }

        std::uint32_t image_count = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0U && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0U) {
            // Take the first supported mode rather than failing: the picture is the same.
            const VkCompositeAlphaFlagsKHR supported = capabilities.supportedCompositeAlpha;
            composite = (supported & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) != 0U ? VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR
                        : (supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0U ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface          = surface;
        info.minImageCount    = image_count;
        info.imageFormat      = format;
        info.imageColorSpace  = color_space;
        info.imageExtent      = extent;
        info.imageArrayLayers = 1U;
        info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform     = capabilities.currentTransform;
        info.compositeAlpha   = composite;
        info.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped          = VK_TRUE;
        info.oldSwapchain     = VK_NULL_HANDLE;
        if (device.table->vkCreateSwapchainKHR(device.device, &info, nullptr, &swapchain) != VK_SUCCESS) {
            swapchain = VK_NULL_HANDLE;
            if (why != nullptr) {
                *why = vn::String(u8"vk: vkCreateSwapchainKHR refused the window's surface");
            }
            return false;
        }

        std::uint32_t created = 0;
        if (device.table->vkGetSwapchainImagesKHR(device.device, swapchain, &created, nullptr) != VK_SUCCESS || created == 0U) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the swapchain reported no images");
            }
            return false;
        }
        images.resize(created);
        if (device.table->vkGetSwapchainImagesKHR(device.device, swapchain, &created, images.data()) != VK_SUCCESS) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the swapchain's images could not be read");
            }
            return false;
        }
        if (!createSwapchainObjects(why)) {
            destroySwapchain();
            return false;
        }
        needs_recreate = false;
        return true;
    }
};

WindowTarget::WindowTarget(WindowSystem& window_system, void* native_handle)
  : impl(std::make_unique<Impl>())
{
    impl->window_system = &window_system;
    impl->handle        = native_handle;
}

WindowTarget::~WindowTarget()
{
    if (impl == nullptr) {
        return;
    }
    // THE SESSION'S OWN TEARDOWN ALREADY STOOD THE QUEUE DOWN (Device::shutdown), so this wait answers "already idle"
    // - it is here so the destructor does not depend on that order being remembered (an object that is dropped while
    // a frame is in flight must still not hand a live semaphore to vkDestroySemaphore).
    impl->destroySwapchainWhenIdle();
    if (impl->device.device != VK_NULL_HANDLE && impl->device.table != nullptr) {
        if (impl->image_available != VK_NULL_HANDLE) {
            impl->device.table->vkDestroySemaphore(impl->device.device, impl->image_available, nullptr);
            impl->image_available = VK_NULL_HANDLE;
        }
    }
    if (impl->surface != VK_NULL_HANDLE && impl->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        impl->surface = VK_NULL_HANDLE;
    }
}

bool WindowTarget::createArea(const VkInstance instance, vn::String* why)
{
    if (impl->surface != VK_NULL_HANDLE) {
        return true;
    }
    if (impl->handle == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a window surface needs a host window handle (DevicePresentation::Probe is not built)");
        }
        return false;
    }
    impl->instance = instance;
    if (impl->window_system == nullptr) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the window target has no window system to create its surface on");
        }
        return false;
    }
    if (!impl->window_system->createArea(instance, impl->handle, impl->surface, why)) {
        return false;
    }
    return true;
}

bool WindowTarget::attach(const WindowDevice& device, vn::String* why)
{
    impl->device      = device;
    impl->attached    = true;
    impl->device_lost = false; // a fresh device: the previous session's loss says nothing about this one
    if (!createArea(device.instance, why)) {
        impl->attached = false;
        return false;
    }

    // THE QUEUE MUST BE ABLE TO PRESENT TO THIS SURFACE. The family was chosen from the FIRST surface the session had
    // (`Device::initialize`), and a session's SECOND window may sit on a surface that family cannot serve - a swapchain
    // built there would be refused by every present, forever. Asked here, once per surface, before anything is made;
    // the caller forwards the sentence through `vk.window.attach_failed`.
    VkBool32 presents = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(device.physical, device.queue_family, impl->surface, &presents) != VK_SUCCESS || presents == VK_FALSE) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the device's queue family cannot present to this window's surface (the family is "
                              "the one the session chose for its first surface, and a second window may need another)");
        }
        impl->attached = false;
        return false;
    }

    // THE ACQUIRE SEMAPHORE IS CREATED WITH THE SWAPCHAIN'S OBJECTS (see createSwapchainObjects), because it has to be
    // rebuilt whenever the chain is: an abandoned acquire (see abandonFrame) leaves it signalled, and a rebuild is the
    // moment that signal is retired.
    if (!impl->createSwapchain(why)) {
        impl->attached = false;
        return false;
    }
    return true;
}

bool WindowTarget::ready() const noexcept
{
    return impl != nullptr && impl->attached && !impl->detached && impl->swapchain != VK_NULL_HANDLE && impl->depth_view != VK_NULL_HANDLE;
}

WindowTarget::PresentationState WindowTarget::presentationState() const noexcept
{
    // THE FACTS IN THE ORDER THAT DECIDES THE SENTENCE (see the header): no window at all, a window this target was
    // never attached to, then the swapchain the (re)build leaves behind. `detached` with a live handle means a move to
    // a new handle could not be rebuilt, which is the same answer to the pass as "there is no window".
    if (impl == nullptr || impl->handle == nullptr || impl->detached) {
        return PresentationState::Detached;
    }
    if (!impl->attached) {
        return PresentationState::NotAttached;
    }
    return ready() ? PresentationState::Ready : PresentationState::NoSwapchain;
}

bool WindowTarget::deviceLost() const noexcept
{
    return impl != nullptr && impl->device_lost;
}

VkSurfaceKHR WindowTarget::surfaceHandle() const noexcept
{
    return impl != nullptr ? impl->surface : VK_NULL_HANDLE;
}

bool WindowTarget::acquire(vn::String* why)
{
    if (impl->device_lost) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the device was lost; the window's next image cannot be acquired");
        }
        return false;
    }
    if (!ready()) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the window target has no live swapchain (a detached or failed window)");
        }
        return false;
    }
    if (impl->needs_recreate) {
        if (!impl->createSwapchain(why)) {
            return false;
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::uint32_t index  = 0U;
        // A FINITE TIMEOUT. `UINT64_MAX` turns a surface the presentation engine never hands an image for (a stuck
        // window, a broken driver) into a hang with no diagnostic; a second is far longer than any healthy frame, so
        // a working surface never sees it, and the timeout is answered like any other failed acquire: the frame is
        // skipped and the episode is reported once (see the caller).
        const VkResult result = impl->device.table->vkAcquireNextImageKHR(impl->device.device,
                                                                          impl->swapchain,
                                                                          kAcquireTimeoutNanoseconds,
                                                                          impl->image_available,
                                                                          VK_NULL_HANDLE,
                                                                          &index);
        if (result == VK_SUCCESS) {
            impl->image_index    = index;
            impl->image_acquired = true;
            return true;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            // NOT out of date: recreating the swapchain cannot fix a lost device (see deviceLost()). The device makes
            // the session-level report - this target only remembers the fact.
            impl->device_lost    = true;
            impl->image_acquired = false;
            if (why != nullptr) {
                *why = vn::String(u8"vk: vkAcquireNextImageKHR answered VK_ERROR_DEVICE_LOST");
            }
            return false;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            impl->needs_recreate = true;
            if (attempt == 0 && impl->createSwapchain(why)) {
                continue;
            }
        }
        impl->image_acquired = false;
        if (why != nullptr) {
            *why = vn::String(u8"vk: vkAcquireNextImageKHR did not hand out the window's next image");
        }
        return false;
    }
    return false;
}

void WindowTarget::beginPass(const VkCommandBuffer commands, const PassAttachmentOps& load, RecordingTally* const tally)
{
    if (impl == nullptr || !impl->image_acquired || impl->image_index >= impl->images.size()) {
        return; // nothing acquired: the caller never reaches here (see acquire), and a pass with no image is nothing
    }
    const std::size_t index = impl->image_index;

    // THE IMAGE RENDERING WRITES is the acquired one; its colour FROM is what this image's own record says (Undefined on
    // first use, Present after it was presented, the colour layout between two passes of one frame). ONE IMAGE, ONE
    // RECORD, which is why the attachment's colour starting layouts are a one-entry span (see `PassAttachmentSet`).
    const VkImage     colour_image = impl->images[index];
    const VkImageView colour_view  = impl->views[index];
    const ImageLayout colour_from  = impl->image_layouts[index];

    PassAttachmentSet attachments{};
    attachments.extent   = VkExtent2D{ impl->extent.width, impl->extent.height };
    attachments.color    = PassRole{ std::span<const VkImage>(&colour_image, 1U),
                                     std::span<const VkImageView>(&colour_view, 1U),
                                     std::span<const ImageLayout>(&colour_from, 1U) };
    attachments.depth        = PassImage{ impl->depth, impl->depth_view, impl->depth_layout };
    attachments.depth_aspect = depthAspectMask(kDepthFormat);

    // A PRESENTED IMAGE IS ALWAYS STORED. `storeOp = DontCare` would discard the very picture the presentation engine
    // is about to hand to the compositor, so the window's one colour attachment takes `Store` whatever the pass
    // stated. The DEPTH is not forced: it is not presented, and a pass may discard it (a later pass that loads it is
    // the author's to order - the same rule every off-screen attachment follows).
    ResolvedAttachment window_colour = load.color.empty() ? ResolvedAttachment{} : load.color.front();
    window_colour.store              = vn::graphics::StoreOp::Store;
    const std::array<ResolvedAttachment, 1U> window_colour_ops{ window_colour };
    PassAttachmentOps                    effective = load;
    effective.color                                    = std::span<const ResolvedAttachment>(window_colour_ops);

    // `beginRendering` records the barriers before the instance: every window pass announces its own, because the
    // instance may start in a layout the presentation engine left (Present) or one an earlier pass left (colour).
    beginRendering(*impl->device.table, commands, attachments, effective, tally);
    impl->image_layouts[index] = ImageLayout::ColorAttachment;
    impl->depth_layout         = ImageLayout::DepthAttachment;
    impl->pass_open            = true;
}

void WindowTarget::endPass(const VkCommandBuffer commands) const noexcept
{
    if (impl == nullptr || !impl->pass_open) {
        return; // nothing was opened (see beginPass): the pair closes only what it opened
    }
    impl->pass_open = false;
    endRendering(*impl->device.table, commands);
}

std::uint32_t WindowTarget::imageIndex() const noexcept
{
    return impl->image_index;
}

std::uint32_t WindowTarget::width() const noexcept
{
    return impl->extent.width;
}

std::uint32_t WindowTarget::height() const noexcept
{
    return impl->extent.height;
}

vn::graphics::RenderPassCompatibility WindowTarget::compatibility() const noexcept
{
    using vn::graphics::RenderTarget;
    vn::graphics::RenderPassCompatibility compatibility;
    compatibility.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    compatibility.device_color_formats.push_back(vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(impl->format) });
    compatibility.depth_format        = RenderTarget::DepthFormat::D24;
    compatibility.device_depth_format = vn::graphics::DeviceFormat{ static_cast<std::uint32_t>(kDepthFormat) };
    compatibility.samples             = 1U;
    compatibility.render_pass_key     = kSingleSubpassRenderPass;
    return compatibility;
}

VkSemaphore WindowTarget::imageAvailableSemaphore() const noexcept
{
    return impl->image_available;
}

VkSemaphore WindowTarget::renderFinishedSemaphore() const noexcept
{
    return impl->renderFinishedOfAcquiredImage();
}

void WindowTarget::recordPresentTransition(const VkCommandBuffer commands)
{
    if (commands == VK_NULL_HANDLE || impl == nullptr || !impl->image_acquired || impl->image_index >= impl->images.size()) {
        return;
    }
    // THE TRANSITION IS TWO ROWS OF THE BARRIER TABLE: colour attachment -> present, with the record written by the same
    // call (see BarrierPlan::transitionImage). The table owns the layouts, the stage masks and the access masks, so this
    // function names none of them and cannot pair a layout with the wrong mask. The next acquire of this image then
    // starts from Present (see beginPass).
    (void)transitionImage(*impl->device.table,
                          commands,
                          impl->images[impl->image_index],
                          impl->image_layouts[impl->image_index],
                          ImageLayout::Present,
                          VK_IMAGE_ASPECT_COLOR_BIT);
}

vn::graphics::PresentOutcome WindowTarget::present()
{
    if (impl == nullptr || !impl->image_acquired || impl->swapchain == VK_NULL_HANDLE ||
        impl->image_index >= impl->render_finished.size()) {
        return vn::graphics::PresentOutcome::Skipped;
    }

    VkPresentInfoKHR info{};
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1U;
    info.pWaitSemaphores    = &impl->render_finished[impl->image_index];
    info.swapchainCount     = 1U;
    info.pSwapchains        = &impl->swapchain;
    info.pImageIndices      = &impl->image_index;
    const VkResult result   = impl->device.table->vkQueuePresentKHR(impl->device.queue, &info);
    impl->image_acquired    = false;

    if (result == VK_SUCCESS) {
        return vn::graphics::PresentOutcome::Presented;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        // NOT a window problem: a recreate cannot fix a lost device, so needs_recreate stays where it is and the
        // device ends the session once (see deviceLost(), and Device::noteDeviceLost for the report).
        impl->device_lost = true;
        return vn::graphics::PresentOutcome::Lost;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        impl->needs_recreate = true;
        return vn::graphics::PresentOutcome::Presented;
    }
    impl->needs_recreate = true;
    return vn::graphics::PresentOutcome::Lost;
}

void WindowTarget::abandonFrame() noexcept
{
    if (impl == nullptr || !impl->image_acquired) {
        return; // nothing was taken from the presentation engine, so there is nothing to give back
    }
    // THE IMAGE AND THE SEMAPHORE BOTH GO BACK: `needs_recreate` makes the next acquire rebuild the chain, which
    // returns the acquired image, retires the signalled `image_available` and starts every layout record at Undefined
    // (see the header's note and `createSwapchainObjects`). Setting `image_acquired` false first keeps `present()` and
    // `endPass()` from touching an image this target no longer holds.
    impl->image_acquired = false;
    impl->pass_open      = false;
    impl->needs_recreate = true;
}

void WindowTarget::resize(const int width, const int height)
{
    if (impl == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (impl->requested_width != width || impl->requested_height != height) {
        impl->requested_width  = width;
        impl->requested_height = height;
        impl->needs_recreate   = true;
    }
}

void WindowTarget::setWindowHandle(void* native_handle)
{
    if (impl == nullptr || impl->handle == native_handle) {
        return;
    }
    impl->destroySwapchainWhenIdle();
    if (impl->surface != VK_NULL_HANDLE && impl->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        impl->surface = VK_NULL_HANDLE;
    }
    // The CONNECTION is the device's (one per session), so a moved handle re-creates its surface on it: closing it
    // here would take every other window target's surface with it.
    impl->handle   = native_handle;
    impl->detached = native_handle == nullptr;
    if (impl->detached || !impl->attached) {
        return;
    }
    vn::String why;
    if (!createArea(impl->instance, &why) || !impl->createSwapchain(&why)) {
        impl->detached = true;
    }
}

void* WindowTarget::nativeHandle() const
{
    return impl != nullptr ? impl->handle : nullptr;
}

VN_BACKEND_VK_NS_END
