#include <vine/backend_vk/internal/WindowTarget.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include <volk.h>

#include <vine/backend_vk/internal/WindowSystem.hpp>
#include <vine/graphics/RenderTarget.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The depth format every window target uses (the same D24 the off-screen targets use).
constexpr VkFormat kDepthFormat = VK_FORMAT_D24_UNORM_S8_UINT;

/**
 * @brief Finds a memory type that serves @p bits and has every property in @p wanted.
 *
 * @param memory The device's memory properties.
 * @param bits   The type bits the allocation requires.
 * @param wanted The property flags the memory must have.
 * @param out    Receives the type index.
 * @return true when a matching type exists.
 */
[[nodiscard]] bool findMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                  std::uint32_t                          bits,
                                  VkMemoryPropertyFlags                  wanted,
                                  std::uint32_t&                         out) noexcept
{
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            out = index;
            return true;
        }
    }
    return false;
}

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

/**
 * @brief Creates one of the window's two render passes (colour CLEAR or colour LOAD).
 *
 * @details
 * - Colour: CLEAR or LOAD, STORE, into `COLOR_ATTACHMENT_OPTIMAL` (the device records the present transition after
 *   the last pass). Depth: CLEAR in the first pass, LOAD in a later pass into the same image.
 * - A LOAD op may not name UNDEFINED, so the LOAD variant starts in the attachment layouts the earlier pass left.
 *
 * @param table       The device's entry points.
 * @param device      The logical device.
 * @param format      The swapchain's colour format.
 * @param load_color  Whether the colour attachment is LOADed.
 * @param load_depth  Whether the depth attachment is LOADed.
 * @param out         Receives the render pass.
 * @return true when the render pass exists.
 */
[[nodiscard]] bool createWindowRenderPass(const VolkDeviceTable& table,
                                          const VkDevice         device,
                                          const VkFormat         format,
                                          const bool             load_color,
                                          const bool             load_depth,
                                          VkRenderPass&          out)
{
    VkAttachmentDescription color{};
    color.format         = format;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = load_color ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = load_color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format         = kDepthFormat;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = load_depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = load_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    const VkAttachmentDescription attachments[2]{ color, depth };
    const VkAttachmentReference   color_ref{ 0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    const VkAttachmentReference   depth_ref{ 1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1U;
    subpass.pColorAttachments       = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0U;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0U;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2U;
    info.pAttachments    = attachments;
    info.subpassCount    = 1U;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1U;
    info.pDependencies   = &dependency;
    if (table.vkCreateRenderPass(device, &info, nullptr, &out) != VK_SUCCESS) {
        out = VK_NULL_HANDLE;
        return false;
    }
    return true;
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

    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;

    VkImage        depth{ VK_NULL_HANDLE };
    VkDeviceMemory depth_memory{ VK_NULL_HANDLE };
    VkImageView    depth_view{ VK_NULL_HANDLE };

    VkRenderPass pass_clear{ VK_NULL_HANDLE };
    VkRenderPass pass_load{ VK_NULL_HANDLE };

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

    /** @brief Gets the present-wait semaphore of the image acquired NOW (`VK_NULL_HANDLE` when none is). */
    [[nodiscard]] VkSemaphore renderFinishedOfAcquiredImage() const noexcept
    {
        return image_acquired && image_index < render_finished.size() ? render_finished[image_index] : VK_NULL_HANDLE;
    }

    /** @brief Creates the swapchain's views, depth image, render passes and framebuffers for the current images. */
    [[nodiscard]] bool createSwapchainObjects(vn::String* why)
    {
        views.resize(images.size(), VK_NULL_HANDLE);
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

        // THE DEPTH IMAGE: one image shared by every swapchain image (the render pass clears it per frame).
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
        std::uint32_t memory_type = 0;
        if (!findMemoryType(device.memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type)) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: no device-local memory type serves the window's depth image");
            }
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize  = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (device.table->vkAllocateMemory(device.device, &allocate, nullptr, &depth_memory) != VK_SUCCESS ||
            device.table->vkBindImageMemory(device.device, depth, depth_memory, 0U) != VK_SUCCESS) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's depth image memory could not be prepared");
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

        if (!createWindowRenderPass(*device.table, device.device, format, false, false, pass_clear) ||
            !createWindowRenderPass(*device.table, device.device, format, true, true, pass_load)) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's render pass could not be created");
            }
            return false;
        }

        framebuffers.resize(images.size(), VK_NULL_HANDLE);
        for (std::size_t index = 0; index < images.size(); ++index) {
            const VkImageView attachments[2]{ views[index], depth_view };
            VkFramebufferCreateInfo info{};
            info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass      = pass_clear;
            info.attachmentCount = 2U;
            info.pAttachments    = attachments;
            info.width           = extent.width;
            info.height          = extent.height;
            info.layers          = 1U;
            if (device.table->vkCreateFramebuffer(device.device, &info, nullptr, &framebuffers[index]) != VK_SUCCESS) {
                framebuffers[index] = VK_NULL_HANDLE;
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a window framebuffer could not be created");
                }
                return false;
            }
        }
        return true;
    }

    /** @brief Destroys the swapchain and everything hanging off it (idempotent). */
    void destroySwapchain() noexcept
    {
        if (device.device != VK_NULL_HANDLE && device.table != nullptr) {
            for (const VkFramebuffer framebuffer : framebuffers) {
                if (framebuffer != VK_NULL_HANDLE) {
                    device.table->vkDestroyFramebuffer(device.device, framebuffer, nullptr);
                }
            }
            framebuffers.clear();
            if (pass_clear != VK_NULL_HANDLE) {
                device.table->vkDestroyRenderPass(device.device, pass_clear, nullptr);
                pass_clear = VK_NULL_HANDLE;
            }
            if (pass_load != VK_NULL_HANDLE) {
                device.table->vkDestroyRenderPass(device.device, pass_load, nullptr);
                pass_load = VK_NULL_HANDLE;
            }
            if (depth_view != VK_NULL_HANDLE) {
                device.table->vkDestroyImageView(device.device, depth_view, nullptr);
                depth_view = VK_NULL_HANDLE;
            }
            if (depth != VK_NULL_HANDLE) {
                device.table->vkDestroyImage(device.device, depth, nullptr);
                depth = VK_NULL_HANDLE;
            }
            if (depth_memory != VK_NULL_HANDLE) {
                device.table->vkFreeMemory(device.device, depth_memory, nullptr);
                depth_memory = VK_NULL_HANDLE;
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

        (void)device.table->vkDeviceWaitIdle(device.device);
        destroySwapchain();

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
    impl->destroySwapchain();
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

bool WindowTarget::createSurface(const VkInstance instance, vn::String* why)
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
    if (!impl->window_system->createSurface(instance, impl->handle, impl->surface, why)) {
        return false;
    }
    return true;
}

bool WindowTarget::attach(const WindowDevice& device, vn::String* why)
{
    impl->device      = device;
    impl->attached    = true;
    impl->device_lost = false; // a fresh device: the previous session's loss says nothing about this one
    if (!createSurface(device.instance, why)) {
        impl->attached = false;
        return false;
    }

    // THE ACQUIRE SEMAPHORE lives as long as the target (the swapchain can be rebuilt under it): the submit waits on
    // it and the fence wait below that makes it free again, so one is enough. The present-wait pair is per IMAGE and
    // is created with the swapchain's objects (see render_finished).
    if (impl->image_available == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (device.table->vkCreateSemaphore(device.device, &info, nullptr, &impl->image_available) != VK_SUCCESS) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: the window's acquire semaphore could not be created");
            }
            impl->attached = false;
            return false;
        }
    }

    if (!impl->createSwapchain(why)) {
        impl->attached = false;
        return false;
    }
    return true;
}

bool WindowTarget::ready() const noexcept
{
    return impl != nullptr && impl->attached && !impl->detached && impl->swapchain != VK_NULL_HANDLE && impl->pass_clear != VK_NULL_HANDLE;
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
        const VkResult result = impl->device.table->vkAcquireNextImageKHR(impl->device.device,
                                                                          impl->swapchain,
                                                                          UINT64_MAX,
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

VkRenderPass WindowTarget::renderPass(const bool load_color, const bool load_depth) const noexcept
{
    return load_color && impl->pass_load != VK_NULL_HANDLE ? impl->pass_load : impl->pass_clear;
}

VkFramebuffer WindowTarget::framebuffer() const noexcept
{
    if (impl == nullptr || !impl->image_acquired || impl->image_index >= impl->framebuffers.size()) {
        return VK_NULL_HANDLE;
    }
    return impl->framebuffers[impl->image_index];
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

vn::graphics::backend::RenderPassCompatibility WindowTarget::compatibility() const noexcept
{
    using vn::graphics::RenderTarget;
    vn::graphics::backend::TargetShape shape;
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    shape.device_color_formats.push_back(static_cast<std::uint32_t>(impl->format));
    shape.depth_format        = RenderTarget::DepthFormat::D24;
    shape.device_depth_format = static_cast<std::uint32_t>(kDepthFormat);
    shape.samples             = 1U;
    shape.subpass             = 0U;
    return shape.compatibility();
}

VkSemaphore WindowTarget::imageAvailableSemaphore() const noexcept
{
    return impl->image_available;
}

VkSemaphore WindowTarget::renderFinishedSemaphore() const noexcept
{
    return impl->renderFinishedOfAcquiredImage();
}

void WindowTarget::recordPresentTransition(const VkCommandBuffer commands) const
{
    if (commands == VK_NULL_HANDLE || impl == nullptr || !impl->image_acquired || impl->image_index >= impl->images.size()) {
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = impl->images[impl->image_index];
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0U;
    barrier.subresourceRange.levelCount     = 1U;
    barrier.subresourceRange.baseArrayLayer = 0U;
    barrier.subresourceRange.layerCount     = 1U;
    barrier.srcAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask                   = 0U;
    impl->device.table->vkCmdPipelineBarrier(commands,
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                             0U,
                                             0U,
                                             nullptr,
                                             0U,
                                             nullptr,
                                             1U,
                                             &barrier);
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
    impl->destroySwapchain();
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
    if (!createSurface(impl->instance, &why) || !impl->createSwapchain(&why)) {
        impl->detached = true;
    }
}

void* WindowTarget::nativeHandle() const
{
    return impl != nullptr ? impl->handle : nullptr;
}

VN_BACKEND_VK_NS_END
