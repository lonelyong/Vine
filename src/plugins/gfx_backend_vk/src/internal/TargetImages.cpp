#include <vine/backend_vk/internal/TargetImages.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Finds a memory type that is device-local and satisfies @p bits.
 *
 * @param memory Device memory properties.
 * @param bits   The type bits the image reports.
 * @return The index, or `UINT32_MAX` when no type fits.
 */
[[nodiscard]] std::uint32_t findDeviceLocalType(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t bits) noexcept
{
    constexpr VkMemoryPropertyFlags kRequired = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        const bool allowed = (bits & (1U << index)) != 0U;
        const bool fits    = (memory.memoryTypes[index].propertyFlags & kRequired) == kRequired;
        if (allowed && fits) {
            return index;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

/**
 * @brief Creates one image, allocates and binds its memory.
 *
 * @param table   Device entry points.
 * @param device  Logical device.
 * @param memory  Memory properties.
 * @param create  Fully filled create info.
 * @param image   Receives the image.
 * @param storage Receives the memory.
 * @return true when the image exists and owns its memory.
 */
[[nodiscard]] bool createImage(const VolkDeviceTable&                  table,
                               VkDevice                                device,
                               const VkPhysicalDeviceMemoryProperties& memory,
                               const VkImageCreateInfo&                create,
                               VkImage&                                image,
                               VkDeviceMemory&                         storage)
{
    if (table.vkCreateImage(device, &create, nullptr, &image) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    table.vkGetImageMemoryRequirements(device, image, &requirements);

    const std::uint32_t type = findDeviceLocalType(memory, requirements.memoryTypeBits);
    if (type == std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize  = requirements.size;
    allocate.memoryTypeIndex = type;
    if (table.vkAllocateMemory(device, &allocate, nullptr, &storage) != VK_SUCCESS) {
        storage = VK_NULL_HANDLE;
        return false;
    }
    return table.vkBindImageMemory(device, image, storage, 0U) == VK_SUCCESS;
}

/**
 * @brief Creates a 2D view over @p image.
 *
 * @param table  Device entry points.
 * @param device Logical device.
 * @param image  Image to view.
 * @param format The image's format.
 * @param aspect The aspect the view exposes.
 * @param view   Receives the view.
 * @return true when the view exists.
 */
[[nodiscard]] bool createView(const VolkDeviceTable& table,
                              VkDevice               device,
                              VkImage                image,
                              VkFormat               format,
                              VkImageAspectFlags     aspect,
                              VkImageView&           view)
{
    VkImageViewCreateInfo create{};
    create.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    create.image                           = image;
    create.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    create.format                          = format;
    create.subresourceRange.aspectMask     = aspect;
    create.subresourceRange.baseMipLevel   = 0U;
    create.subresourceRange.levelCount     = 1U;
    create.subresourceRange.baseArrayLayer = 0U;
    create.subresourceRange.layerCount     = 1U;
    return table.vkCreateImageView(device, &create, nullptr, &view) == VK_SUCCESS;
}

} // namespace

bool TargetImages::matches(const int w, const int h, const VkFormat color, const VkFormat depth_fmt) const noexcept
{
    return width == w && height == h && color_format == color && depth_format == depth_fmt;
}

bool TargetImages::built() const noexcept
{
    return render_pass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE && !color_views.empty();
}

VkFormat colorFormatOf(const vn::graphics::RenderTarget::ColorFormat format) noexcept
{
    using ColorFormat = vn::graphics::RenderTarget::ColorFormat;
    switch (format) {
    case ColorFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case ColorFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ColorFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat depthFormatOf(const vn::graphics::RenderTarget::DepthFormat format) noexcept
{
    using DepthFormat = vn::graphics::RenderTarget::DepthFormat;
    switch (format) {
    case DepthFormat::D16: return VK_FORMAT_D16_UNORM;
    case DepthFormat::D24: return VK_FORMAT_D24_UNORM_S8_UINT;
    case DepthFormat::D32: return VK_FORMAT_D32_SFLOAT; // Vulkan has no unsigned-integer depth format
    case DepthFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

bool buildTarget(const VolkDeviceTable&                  table,
                 const VkDevice                          device,
                 const VkPhysicalDeviceMemoryProperties& memory,
                 TargetImages&                           images)
{
    const auto width  = static_cast<std::uint32_t>(images.width);
    const auto height = static_cast<std::uint32_t>(images.height);
    if (images.width <= 0 || images.height <= 0 || images.color_format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    // ---- colour attachments
    images.colors.resize(1U);
    images.color_memories.resize(1U);
    images.color_views.resize(1U);

    VkImageCreateInfo image_create{};
    image_create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create.imageType     = VK_IMAGE_TYPE_2D;
    image_create.extent.width  = width;
    image_create.extent.height = height;
    image_create.extent.depth  = 1U;
    image_create.mipLevels     = 1U;
    image_create.arrayLayers   = 1U;
    image_create.format        = images.color_format;
    image_create.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_create.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_create.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    if (!createImage(table, device, memory, image_create, images.colors[0], images.color_memories[0])) {
        destroyTarget(table, device, images);
        return false;
    }
    if (!createView(table, device, images.colors[0], images.color_format, VK_IMAGE_ASPECT_COLOR_BIT, images.color_views[0])) {
        destroyTarget(table, device, images);
        return false;
    }

    // ---- depth attachment (optional)
    if (images.depth_format != VK_FORMAT_UNDEFINED) {
        VkImageCreateInfo depth_create = image_create;
        depth_create.format            = images.depth_format;
        depth_create.usage             = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (!createImage(table, device, memory, depth_create, images.depth, images.depth_memory)) {
            destroyTarget(table, device, images);
            return false;
        }
        if (!createView(table, device, images.depth, images.depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_view)) {
            destroyTarget(table, device, images);
            return false;
        }
    }

    // ---- render pass: one subpass, CLEAR/STORE, UNDEFINED in (M1 clears; see the header note).
    std::vector<VkAttachmentDescription> attachments;
    VkAttachmentReference               color_reference{ 0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference               depth_reference{ 1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkAttachmentDescription color_attachment{};
    color_attachment.format         = images.color_format;
    color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments.push_back(color_attachment);

    if (images.depth_format != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription depth_attachment{};
        depth_attachment.format         = images.depth_format;
        depth_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth_attachment);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1U;
    subpass.pColorAttachments       = &color_reference;
    subpass.pDepthStencilAttachment = images.depth_format != VK_FORMAT_UNDEFINED ? &depth_reference : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0U;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0U;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo pass_create{};
    pass_create.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_create.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass_create.pAttachments    = attachments.data();
    pass_create.subpassCount    = 1U;
    pass_create.pSubpasses      = &subpass;
    pass_create.dependencyCount = 1U;
    pass_create.pDependencies   = &dependency;

    if (table.vkCreateRenderPass(device, &pass_create, nullptr, &images.render_pass) != VK_SUCCESS) {
        images.render_pass = VK_NULL_HANDLE;
        destroyTarget(table, device, images);
        return false;
    }

    // ---- framebuffer
    std::vector<VkImageView> views{ images.color_views[0] };
    if (images.depth_format != VK_FORMAT_UNDEFINED) {
        views.push_back(images.depth_view);
    }

    VkFramebufferCreateInfo framebuffer_create{};
    framebuffer_create.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_create.renderPass      = images.render_pass;
    framebuffer_create.attachmentCount = static_cast<std::uint32_t>(views.size());
    framebuffer_create.pAttachments    = views.data();
    framebuffer_create.width           = width;
    framebuffer_create.height          = height;
    framebuffer_create.layers          = 1U;

    if (table.vkCreateFramebuffer(device, &framebuffer_create, nullptr, &images.framebuffer) != VK_SUCCESS) {
        images.framebuffer = VK_NULL_HANDLE;
        destroyTarget(table, device, images);
        return false;
    }
    return true;
}

void destroyTarget(const VolkDeviceTable& table, const VkDevice device, TargetImages& images) noexcept
{
    if (device != VK_NULL_HANDLE) {
        if (images.framebuffer != VK_NULL_HANDLE) {
            table.vkDestroyFramebuffer(device, images.framebuffer, nullptr);
        }
        if (images.render_pass != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass, nullptr);
        }
        if (images.depth_view != VK_NULL_HANDLE) {
            table.vkDestroyImageView(device, images.depth_view, nullptr);
        }
        if (images.depth != VK_NULL_HANDLE) {
            table.vkDestroyImage(device, images.depth, nullptr);
        }
        if (images.depth_memory != VK_NULL_HANDLE) {
            table.vkFreeMemory(device, images.depth_memory, nullptr);
        }
        for (const VkImageView view : images.color_views) {
            if (view != VK_NULL_HANDLE) {
                table.vkDestroyImageView(device, view, nullptr);
            }
        }
        for (const VkImage image : images.colors) {
            if (image != VK_NULL_HANDLE) {
                table.vkDestroyImage(device, image, nullptr);
            }
        }
        for (const VkDeviceMemory memory : images.color_memories) {
            if (memory != VK_NULL_HANDLE) {
                table.vkFreeMemory(device, memory, nullptr);
            }
        }
    }

    images.colors.clear();
    images.color_memories.clear();
    images.color_views.clear();
    images.depth        = VK_NULL_HANDLE;
    images.depth_memory = VK_NULL_HANDLE;
    images.depth_view   = VK_NULL_HANDLE;
    images.render_pass  = VK_NULL_HANDLE;
    images.framebuffer  = VK_NULL_HANDLE;
    images.width        = 0;
    images.height       = 0;
    images.color_format = VK_FORMAT_UNDEFINED;
    images.depth_format = VK_FORMAT_UNDEFINED;
}

VN_BACKEND_VK_NS_END
