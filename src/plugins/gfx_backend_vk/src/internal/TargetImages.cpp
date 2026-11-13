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

bool TargetImages::built() const noexcept
{
    return render_pass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE && !color_views.empty() && color_views.size() == colorCount();
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

namespace
{

/**
 * @brief Destroys the images, views and framebuffer, KEEPING the render passes.
 *
 * @details
 * - The extent-bound half of a target (see the header's shape-vs-extent split): a resize replaces exactly this and
 *   must not touch what pipelines were compiled against.
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param images The record to strip.
 */
void destroyAttachments(const VolkDeviceTable& table, const VkDevice device, TargetImages& images) noexcept
{
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (images.framebuffer != VK_NULL_HANDLE) {
        table.vkDestroyFramebuffer(device, images.framebuffer, nullptr);
        images.framebuffer = VK_NULL_HANDLE;
    }
    if (images.depth_view != VK_NULL_HANDLE) {
        table.vkDestroyImageView(device, images.depth_view, nullptr);
        images.depth_view = VK_NULL_HANDLE;
    }
    if (images.depth != VK_NULL_HANDLE) {
        table.vkDestroyImage(device, images.depth, nullptr);
        images.depth = VK_NULL_HANDLE;
    }
    if (images.depth_memory != VK_NULL_HANDLE) {
        table.vkFreeMemory(device, images.depth_memory, nullptr);
        images.depth_memory = VK_NULL_HANDLE;
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
    images.colors.clear();
    images.color_memories.clear();
    images.color_views.clear();
}

/**
 * @brief Creates one render pass over the target's attachment set.
 *
 * @details
 * - Colour attachments CLEAR and STORE; the depth attachment CLEARs or LOADs according to @p depth_load. A LOAD op is
 *   NOT part of render-pass compatibility, so the two variants share one framebuffer and one set of pipelines.
 * - Every colour attachment is referenced, in order: a fragment's output location i writes attachment i (MRT).
 *
 * @param table      The device's entry points (volk table).
 * @param device     The logical device.
 * @param images     The target whose shape states the attachment set.
 * @param depth_load The depth attachment's load op.
 * @param out        Receives the render pass.
 * @return true when the render pass exists.
 */
[[nodiscard]] bool createRenderPass(const VolkDeviceTable& table,
                                    const VkDevice         device,
                                    const TargetImages&    images,
                                    const VkAttachmentLoadOp depth_load,
                                    VkRenderPass&          out)
{
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference>   color_references;
    attachments.reserve(images.colorCount() + (images.hasDepth() ? 1U : 0U));
    color_references.reserve(images.colorCount());

    for (std::size_t index = 0; index < images.colorCount(); ++index) {
        VkAttachmentDescription color{};
        color.format         = images.colorFormat(index);
        color.samples        = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(color);
        color_references.push_back(VkAttachmentReference{ static_cast<std::uint32_t>(index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }

    VkAttachmentReference depth_reference{};
    if (images.hasDepth()) {
        VkAttachmentDescription depth{};
        depth.format         = images.depthFormat();
        depth.samples        = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp         = depth_load;
        depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // A LOAD op may not name an UNDEFINED image (VUID-VkAttachmentDescription-format-06699): the LOAD variant is
        // only ever used after a CLEAR variant left the image in its attachment layout (see TargetImages::fresh).
        depth.initialLayout  = depth_load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);
        depth_reference = VkAttachmentReference{ static_cast<std::uint32_t>(images.colorCount()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = static_cast<std::uint32_t>(color_references.size());
    subpass.pColorAttachments       = color_references.data();
    subpass.pDepthStencilAttachment = images.hasDepth() ? &depth_reference : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0U;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0U;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (images.hasDepth()) {
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkRenderPassCreateInfo pass_create{};
    pass_create.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_create.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    pass_create.pAttachments    = attachments.data();
    pass_create.subpassCount    = 1U;
    pass_create.pSubpasses      = &subpass;
    pass_create.dependencyCount = 1U;
    pass_create.pDependencies   = &dependency;

    if (table.vkCreateRenderPass(device, &pass_create, nullptr, &out) != VK_SUCCESS) {
        out = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/**
 * @brief Creates the images, their views and the framebuffer for `images.desc` (the render passes must exist).
 *
 * @details
 * - The framebuffer is created against the CLEAR variant and is equally usable with the LOAD variant (same attachment
 *   formats), which is what keeps one framebuffer valid across both load-op choices.
 * - On failure the partial objects are destroyed before answering false.
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param memory The device's memory properties.
 * @param images The record to fill.
 * @return true when every attachment object exists.
 */
[[nodiscard]] bool createAttachments(const VolkDeviceTable&                  table,
                                     const VkDevice                          device,
                                     const VkPhysicalDeviceMemoryProperties& memory,
                                     TargetImages&                           images)
{
    const auto width  = static_cast<std::uint32_t>(images.desc.width);
    const auto height = static_cast<std::uint32_t>(images.desc.height);
    if (images.desc.width <= 0 || images.desc.height <= 0 || images.colorCount() == 0U) {
        return false;
    }

    VkImageCreateInfo image_create{};
    image_create.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create.imageType     = VK_IMAGE_TYPE_2D;
    image_create.extent.width  = width;
    image_create.extent.height = height;
    image_create.extent.depth  = 1U;
    image_create.mipLevels     = 1U;
    image_create.arrayLayers   = 1U;
    image_create.tiling        = VK_IMAGE_TILING_OPTIMAL;
    image_create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_create.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_create.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    images.colors.resize(images.colorCount());
    images.color_memories.resize(images.colorCount());
    images.color_views.resize(images.colorCount());
    for (std::size_t index = 0; index < images.colorCount(); ++index) {
        image_create.format = images.colorFormat(index);
        if (!createImage(table, device, memory, image_create, images.colors[index], images.color_memories[index]) ||
            !createView(table, device, images.colors[index], image_create.format, VK_IMAGE_ASPECT_COLOR_BIT, images.color_views[index])) {
            destroyAttachments(table, device, images);
            return false;
        }
    }

    if (images.hasDepth()) {
        image_create.format = images.depthFormat();
        image_create.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (!createImage(table, device, memory, image_create, images.depth, images.depth_memory) ||
            !createView(table, device, images.depth, image_create.format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_view)) {
            destroyAttachments(table, device, images);
            return false;
        }
    }

    std::vector<VkImageView> views(images.color_views.begin(), images.color_views.end());
    if (images.hasDepth()) {
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
        destroyAttachments(table, device, images);
        return false;
    }
    return true;
}

} // namespace

bool buildTarget(const VolkDeviceTable&                  table,
                 const VkDevice                          device,
                 const VkPhysicalDeviceMemoryProperties& memory,
                 TargetImages&                           images)
{
    if (images.desc.width <= 0 || images.desc.height <= 0 || images.colorCount() == 0U) {
        return false;
    }

    // RENDER PASSES FIRST: the framebuffer names one of them, and their formats come from the shape alone.
    if (!createRenderPass(table, device, images, VK_ATTACHMENT_LOAD_OP_CLEAR, images.render_pass)) {
        destroyTarget(table, device, images);
        return false;
    }
    if (images.hasDepth() && !createRenderPass(table, device, images, VK_ATTACHMENT_LOAD_OP_LOAD, images.render_pass_load_depth)) {
        destroyTarget(table, device, images);
        return false;
    }
    if (!createAttachments(table, device, memory, images)) {
        destroyTarget(table, device, images);
        return false;
    }
    images.fresh = true;
    return true;
}

bool resizeTarget(const VolkDeviceTable&                  table,
                  const VkDevice                          device,
                  const VkPhysicalDeviceMemoryProperties& memory,
                  TargetImages&                           images)
{
    if (images.render_pass == VK_NULL_HANDLE) {
        return false; // nothing to keep: a resize presupposes a built target
    }
    // The SHAPE is unchanged (the caller only takes this path when `planTarget` answered ResizeInPlace), so the
    // render passes and every pipeline compiled against them stay; only the extent-bound objects are replaced.
    destroyAttachments(table, device, images);
    if (!createAttachments(table, device, memory, images)) {
        destroyTarget(table, device, images);
        return false;
    }
    images.fresh = true;
    return true;
}

void destroyTarget(const VolkDeviceTable& table, const VkDevice device, TargetImages& images) noexcept
{
    destroyAttachments(table, device, images);
    if (device != VK_NULL_HANDLE) {
        if (images.render_pass_load_depth != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass_load_depth, nullptr);
        }
        if (images.render_pass != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass, nullptr);
        }
    }
    images.render_pass            = VK_NULL_HANDLE;
    images.render_pass_load_depth = VK_NULL_HANDLE;
    images.framebuffer            = VK_NULL_HANDLE;
    images.fresh                  = true;
    images.desc                   = vn::graphics::backend::TargetDesc{};
}

VN_BACKEND_VK_NS_END
