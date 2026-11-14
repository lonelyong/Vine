#include <vine/backend_vk/internal/TargetImages.hpp>

#include <cstdint>
#include <cstring>
#include <limits>

#include <vine/backend_vk/internal/StateMapping.hpp>

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
    // A DEPTH-ONLY TARGET (a shadow map) has every object too: its render pass and framebuffer exist, it simply owns
    // no colour view. A target with NEITHER attachment is the one shape that is not built.
    const bool attachments = color_views.size() == colorCount() && (!color_views.empty() || hasDepth());
    // A MULTISAMPLED target is not built without its sample buffers: the render pass draws into those and resolves
    // them, so a target that lost them has nothing to draw into whatever its resolve images say.
    const bool sample_buffers = !isMultisampled() || multisample_color_views.size() == colorCount();
    // ... and a multisampled DEPTH is not built without its resolve picture either: the pass resolves into that one,
    // and it is the image a shader samples (see depthPicture).
    const bool depth_picture = !resolvesDepth() || hasDepthResolve();
    return render_pass != VK_NULL_HANDLE && framebuffer != VK_NULL_HANDLE && attachments && sample_buffers && depth_picture;
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

VkImageAspectFlags depthAspectMask(const VkFormat format) noexcept
{
    if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return VK_IMAGE_ASPECT_DEPTH_BIT;
}

namespace
{

/**
 * @brief Destroys the images, views and framebuffer, KEEPING the render passes.
 *
 * @details
 * - The extent-bound half of a target (see the header's shape-vs-extent split): a resize replaces exactly this and
 *   must not touch what pipelines were compiled against.
 * - Both halves of a multisampled attachment go: the sample buffers are no more reusable than the resolve images they
 *   were built with (they share one sample count, which is part of the shape).
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
    if (images.depth_resolve_view != VK_NULL_HANDLE) {
        table.vkDestroyImageView(device, images.depth_resolve_view, nullptr);
        images.depth_resolve_view = VK_NULL_HANDLE;
    }
    if (images.depth_resolve != VK_NULL_HANDLE) {
        table.vkDestroyImage(device, images.depth_resolve, nullptr);
        images.depth_resolve = VK_NULL_HANDLE;
    }
    if (images.depth_resolve_memory != VK_NULL_HANDLE) {
        table.vkFreeMemory(device, images.depth_resolve_memory, nullptr);
        images.depth_resolve_memory = VK_NULL_HANDLE;
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
    for (const VkImageView view : images.multisample_color_views) {
        if (view != VK_NULL_HANDLE) {
            table.vkDestroyImageView(device, view, nullptr);
        }
    }
    for (const VkImage image : images.multisample_colors) {
        if (image != VK_NULL_HANDLE) {
            table.vkDestroyImage(device, image, nullptr);
        }
    }
    for (const VkDeviceMemory memory : images.multisample_color_memories) {
        if (memory != VK_NULL_HANDLE) {
            table.vkFreeMemory(device, memory, nullptr);
        }
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
    images.multisample_colors.clear();
    images.multisample_color_memories.clear();
    images.multisample_color_views.clear();
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
 * - ON A MULTISAMPLED TARGET the attachment array is [sample buffers..., resolve images..., depth, depth resolve],
 *   the subpass' colour references point at the sample buffers and `pResolveAttachments` at the resolve images. A
 *   resolve attachment only ever STORES (it is written by the resolve, never loaded), which is why its description
 *   states DONT_CARE/STORE in every variant - and why the three load-op variants stay compatible with one another.
 * - THE DEPTH IS RESOLVED TOO (see the header's class note): `pDepthStencilResolveAttachment` names the single-sample
 *   picture and the mode is `sample0`, so the picture is what a single-sample draw of the same scene would have
 *   written. That reference only exists in `VkSubpassDescription2`, so a target that resolves depth is created with
 *   `vkCreateRenderPass2` (Vulkan 1.2); every other target keeps the 1.0 call, which is the entry point every device
 *   has.
 *
 * @param table      The device's entry points (volk table).
 * @param device     The logical device.
 * @param images     The target whose shape states the attachment set.
 * @param color_load The colour attachments' load op.
 * @param depth_load The depth attachment's load op.
 * @param out        Receives the render pass.
 * @return true when the render pass exists.
 */
[[nodiscard]] bool createRenderPass(const VolkDeviceTable& table,
                                    const VkDevice         device,
                                    const TargetImages&    images,
                                    const VkAttachmentLoadOp color_load,
                                    const VkAttachmentLoadOp depth_load,
                                    VkRenderPass&          out)
{
    const VkSampleCountFlagBits sample_count = StateMapping::sampleCount(images.samples());
    const bool                  resolved     = images.isMultisampled();

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference>   color_references;
    std::vector<VkAttachmentReference>   resolve_references;
    const std::size_t                    resolve_base = resolved ? images.colorCount() : 0U;
    attachments.reserve(images.colorCount() * (resolved ? 2U : 1U) + (images.hasDepth() ? 1U : 0U));
    color_references.reserve(images.colorCount());
    resolve_references.reserve(images.colorCount());

    for (std::size_t index = 0; index < images.colorCount(); ++index) {
        VkAttachmentDescription color{};
        color.format         = images.colorFormat(index);
        color.samples        = sample_count;
        color.loadOp         = color_load;
        color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // A LOAD op may not name an UNDEFINED image (VUID-VkAttachmentDescription-format-06699): the colour-LOAD
        // variant is only ever used after an earlier pass left the image in its attachment layout.
        color.initialLayout  = color_load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(color);
        color_references.push_back(VkAttachmentReference{ static_cast<std::uint32_t>(index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }

    // THE RESOLVE IMAGES: the picture, written once at the end of the subpass. Their load op is DONT_CARE because a
    // resolve overwrites every pixel (and a load op on a resolve attachment has no meaning to give it).
    for (std::size_t index = 0; index < resolve_base; ++index) {
        VkAttachmentDescription resolve{};
        resolve.format         = images.colorFormat(index);
        resolve.samples        = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(resolve);
        resolve_references.push_back(VkAttachmentReference{ static_cast<std::uint32_t>(resolve_base + index), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }

    const std::size_t depth_index = images.depthAttachmentIndex();
    VkAttachmentReference depth_reference{};
    if (images.hasDepth()) {
        VkAttachmentDescription depth{};
        depth.format         = images.depthFormat();
        depth.samples        = sample_count;
        depth.loadOp         = depth_load;
        depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // A LOAD op may not name an UNDEFINED image (VUID-VkAttachmentDescription-format-06699): the LOAD variant is
        // only ever used after a CLEAR variant left the image in its attachment layout (see TargetImages::fresh).
        depth.initialLayout  = depth_load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);
        depth_reference = VkAttachmentReference{ static_cast<std::uint32_t>(depth_index), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }

    // THE DEPTH PICTURE: the single-sample image the subpass resolves the depth into (see depthPicture). It is the
    // DEPTH RESOLVE ATTACHMENT, which only exists on Vulkan 1.2's VkSubpassDescription2 - so a target that resolves
    // depth is created below with vkCreateRenderPass2. Its load op is DONT_CARE because the resolve overwrites every
    // pixel, and both halves are STOREd (the depth/stencil format may carry a stencil component the resolve writes).
    VkAttachmentReference depth_resolve_reference{};
    if (images.resolvesDepth()) {
        VkAttachmentDescription resolve{};
        resolve.format         = images.depthFormat();
        resolve.samples        = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(resolve);
        depth_resolve_reference = VkAttachmentReference{ static_cast<std::uint32_t>(depth_index + 1U),
                                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = static_cast<std::uint32_t>(color_references.size());
    subpass.pColorAttachments       = color_references.empty() ? nullptr : color_references.data();
    subpass.pResolveAttachments     = resolve_references.empty() ? nullptr : resolve_references.data();
    subpass.pDepthStencilAttachment = images.hasDepth() ? &depth_reference : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0U;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // The SOURCE scope covers what an earlier submission may have done with these attachments: written them, or
    // SAMPLED them (a screen draw sampling the colour picture, a shader sampling the depth picture - see
    // Device::resolveShadow). The second half is what orders this pass' first write after those reads.
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (images.hasDepth()) {
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    if (images.resolvesDepth()) {
        // THE ONE SHAPE THAT NEEDS THE 1.2 ENTRY POINT: the depth resolve reference lives in
        // VkSubpassDescriptionDepthStencilResolve, which only VkSubpassDescription2 carries. `sample0` is required to
        // be supported wherever the feature exists, and it is the only mode that cannot change the picture.
        // StencilResolveMode must not be NONE when the format has a stencil component (D24 here does).
        VkSubpassDescriptionDepthStencilResolve depth_resolve{};
        depth_resolve.sType                          = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        depth_resolve.depthResolveMode               = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        depth_resolve.stencilResolveMode             = depthAspectMask(images.depthFormat()) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                           ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                                           : VK_RESOLVE_MODE_NONE;

        // THE 2-SPELLINGS ARE BUILT FROM THE 1.0 ONES, field by field: the structs only APPEND sType/pNext/viewMask,
        // so casting the 1.0 arrays would still be wrong - the appended fields would be read out of unrelated memory.
        std::vector<VkAttachmentDescription2> attachments2(attachments.size());
        for (std::size_t index = 0; index < attachments.size(); ++index) {
            attachments2[index].sType          = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachments2[index].flags          = attachments[index].flags;
            attachments2[index].format         = attachments[index].format;
            attachments2[index].samples        = attachments[index].samples;
            attachments2[index].loadOp         = attachments[index].loadOp;
            attachments2[index].storeOp        = attachments[index].storeOp;
            attachments2[index].stencilLoadOp  = attachments[index].stencilLoadOp;
            attachments2[index].stencilStoreOp = attachments[index].stencilStoreOp;
            attachments2[index].initialLayout  = attachments[index].initialLayout;
            attachments2[index].finalLayout    = attachments[index].finalLayout;
        }
        const auto references2 = [](const std::vector<VkAttachmentReference>& from) {
            std::vector<VkAttachmentReference2> to(from.size());
            for (std::size_t index = 0; index < from.size(); ++index) {
                to[index].sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
                to[index].attachment = from[index].attachment;
                to[index].layout     = from[index].layout;
            }
            return to;
        };
        const std::vector<VkAttachmentReference2> color_references2   = references2(color_references);
        const std::vector<VkAttachmentReference2> resolve_references2 = references2(resolve_references);
        VkAttachmentReference2                    depth_reference2{};
        depth_reference2.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depth_reference2.attachment = depth_reference.attachment;
        depth_reference2.layout     = depth_reference.layout;
        VkAttachmentReference2 depth_resolve_reference2{};
        depth_resolve_reference2.sType      = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depth_resolve_reference2.attachment = depth_resolve_reference.attachment;
        depth_resolve_reference2.layout     = depth_resolve_reference.layout;
        depth_resolve.pDepthStencilResolveAttachment = &depth_resolve_reference2;

        VkSubpassDescription2 subpass2{};
        subpass2.sType                   = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
        subpass2.pNext                   = &depth_resolve;
        subpass2.pipelineBindPoint       = subpass.pipelineBindPoint;
        subpass2.colorAttachmentCount    = static_cast<std::uint32_t>(color_references2.size());
        subpass2.pColorAttachments       = color_references2.empty() ? nullptr : color_references2.data();
        subpass2.pResolveAttachments     = resolve_references2.empty() ? nullptr : resolve_references2.data();
        subpass2.pDepthStencilAttachment = images.hasDepth() ? &depth_reference2 : nullptr;

        VkSubpassDependency2 dependency2{};
        dependency2.sType           = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
        dependency2.srcSubpass      = dependency.srcSubpass;
        dependency2.dstSubpass      = dependency.dstSubpass;
        dependency2.srcStageMask    = dependency.srcStageMask;
        dependency2.dstStageMask    = dependency.dstStageMask;
        dependency2.srcAccessMask   = dependency.srcAccessMask;
        dependency2.dstAccessMask   = dependency.dstAccessMask;
        dependency2.dependencyFlags = dependency.dependencyFlags;

        VkRenderPassCreateInfo2 pass_create2{};
        pass_create2.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        pass_create2.attachmentCount = static_cast<std::uint32_t>(attachments2.size());
        pass_create2.pAttachments    = attachments2.data();
        pass_create2.subpassCount    = 1U;
        pass_create2.pSubpasses      = &subpass2;
        pass_create2.dependencyCount = 1U;
        pass_create2.pDependencies   = &dependency2;
        if (table.vkCreateRenderPass2(device, &pass_create2, nullptr, &out) != VK_SUCCESS) {
            out = VK_NULL_HANDLE;
            return false;
        }
        return true;
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
 * - A MULTISAMPLED shape creates the sample buffers FIRST and then their resolve images: the render pass draws into the
 *   former and writes the latter, so both exist before the framebuffer names them (in that order - the order the
 *   render pass declares its attachments).
 * - The sample buffers ask for no SAMPLED or TRANSFER_SRC usage: nothing samples them and nothing copies them, and
 *   asking for a capability nothing uses would only widen what the driver has to allocate.
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
    if (images.desc.width <= 0 || images.desc.height <= 0 || (images.colorCount() == 0U && !images.hasDepth())) {
        return false;
    }

    const VkSampleCountFlagBits sample_count = StateMapping::sampleCount(images.samples());
    const bool                  resolved     = images.isMultisampled();

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
    image_create.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_create.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    // THE SAMPLE BUFFERS, when the shape asks for them: one per colour attachment, at the shape's sample count.
    if (resolved) {
        image_create.samples = sample_count;
        image_create.usage   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        images.multisample_colors.resize(images.colorCount());
        images.multisample_color_memories.resize(images.colorCount());
        images.multisample_color_views.resize(images.colorCount());
        for (std::size_t index = 0; index < images.colorCount(); ++index) {
            image_create.format = images.colorFormat(index);
            if (!createImage(table,
                             device,
                             memory,
                             image_create,
                             images.multisample_colors[index],
                             images.multisample_color_memories[index]) ||
                !createView(table, device, images.multisample_colors[index], image_create.format, VK_IMAGE_ASPECT_COLOR_BIT, images.multisample_color_views[index])) {
                destroyAttachments(table, device, images);
                return false;
            }
        }
        image_create.samples = VK_SAMPLE_COUNT_1_BIT;
        image_create.usage   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    // THE PICTURES: drawn into directly on a single-sampled target, RESOLVED into on a multisampled one.
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
        image_create.format  = images.depthFormat();
        // A MULTISAMPLED depth is a DRAW target only (the API forbids sampling or copying it); the single-sample one
        // IS the picture, and it carries the same pair the colour pictures carry: SAMPLED (the shadow path reads it -
        // see Device::resolveShadow) and TRANSFER_SRC (a readDepthBuffer copy reads it).
        image_create.usage   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             (resolved ? 0U : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        image_create.samples = sample_count;
        if (!createImage(table, device, memory, image_create, images.depth, images.depth_memory) ||
            !createView(table, device, images.depth, image_create.format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_view)) {
            destroyAttachments(table, device, images);
            return false;
        }

        // THE DEPTH PICTURE of a multisampled target: the resolve attachment (see TargetImages::depthPicture). It keeps
        // every capability a single-sample depth attachment used to have - DEPTH_STENCIL_ATTACHMENT (the pass resolves
        // into it, a borrower attaches it), SAMPLED (a shader samples it) and TRANSFER_SRC (a readback copies it) - at
        // one sample per texel.
        if (resolved) {
            image_create.samples = VK_SAMPLE_COUNT_1_BIT;
            image_create.usage   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!createImage(table, device, memory, image_create, images.depth_resolve, images.depth_resolve_memory) ||
                !createView(table, device, images.depth_resolve, image_create.format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_resolve_view)) {
                destroyAttachments(table, device, images);
                return false;
            }
        }
    }

    // THE FRAMEBUFFER, in the render pass' own order: sample buffers, resolve images, depth, depth resolve.
    std::vector<VkImageView> views(images.multisample_color_views.begin(), images.multisample_color_views.end());
    views.insert(views.end(), images.color_views.begin(), images.color_views.end());
    if (images.hasDepth()) {
        views.push_back(images.depth_view);
    }
    if (images.hasDepthResolve()) {
        views.push_back(images.depth_resolve_view);
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
    if (images.desc.width <= 0 || images.desc.height <= 0 || (images.colorCount() == 0U && !images.hasDepth())) {
        return false;
    }

    // RENDER PASSES FIRST: the framebuffer names one of them, and their formats come from the shape alone.
    if (!createRenderPass(table, device, images, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR, images.render_pass)) {
        destroyTarget(table, device, images);
        return false;
    }
    if (images.hasDepth() &&
        !createRenderPass(table, device, images, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_LOAD, images.render_pass_load_depth)) {
        destroyTarget(table, device, images);
        return false;
    }
    if (!createRenderPass(table, device, images, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, images.render_pass_load)) {
        destroyTarget(table, device, images);
        return false;
    }
    if (!createAttachments(table, device, memory, images)) {
        destroyTarget(table, device, images);
        return false;
    }
    images.fresh         = true;
    images.color_sampled = false;
    images.depth_sampled = false;
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
    images.fresh         = true;
    images.color_sampled = false;
    images.depth_sampled = false;
    return true;
}

void destroyTarget(const VolkDeviceTable& table, const VkDevice device, TargetImages& images) noexcept
{
    destroyAttachments(table, device, images);
    if (device != VK_NULL_HANDLE) {
        if (images.render_pass_load_depth != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass_load_depth, nullptr);
        }
        if (images.render_pass_load != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass_load, nullptr);
        }
        if (images.render_pass != VK_NULL_HANDLE) {
            table.vkDestroyRenderPass(device, images.render_pass, nullptr);
        }
    }
    images.render_pass            = VK_NULL_HANDLE;
    images.render_pass_load_depth = VK_NULL_HANDLE;
    images.render_pass_load       = VK_NULL_HANDLE;
    images.framebuffer            = VK_NULL_HANDLE;
    images.fresh                  = true;
    images.color_sampled          = false;
    images.depth_sampled          = false;
    images.desc                   = vn::graphics::backend::TargetDesc{};
}

VN_BACKEND_VK_NS_END
