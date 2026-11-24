#include <vine/backend_vk/internal/TargetObjects.hpp>

#include <cstdint>
#include <cstring>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Creates one image, carves its memory out of the device's pool and binds it.
 *
 * @details
 * - THE IMAGE IS CREATED BEFORE THE MEMORY IS ASKED FOR: `vkGetImageMemoryRequirements` is the only thing that knows
 *   the size, the alignment, and - through the type bits - which memory types may serve the image.
 * - A FAILED BIND GIVES THE SLICE BACK: a range must not stay live in the pool for an image that does not name it.
 *   The image stays in @p image so the caller's `destroyAttachments` destroys it like any other partial object.
 *
 * @param table     Device entry points.
 * @param device    Logical device.
 * @param allocator The device's memory pool.
 * @param create    Fully filled create info.
 * @param image     Receives the image.
 * @param storage   Receives the slice of the pool the image is bound into.
 * @return true when the image exists and names its memory.
 */
[[nodiscard]] bool createImage(const VolkDeviceTable&   table,
                               const VkDevice           device,
                               MemoryAllocator&         allocator,
                               const VkImageCreateInfo& create,
                               VkImage&                 image,
                               MemorySlice&             storage)
{
    if (table.vkCreateImage(device, &create, nullptr, &image) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    table.vkGetImageMemoryRequirements(device, image, &requirements);

    storage = allocator.allocate(requirements, MemoryUsage::DeviceLocal, MemoryKind::Optimal);
    if (!storage.valid()) {
        return false;
    }
    if (table.vkBindImageMemory(device, image, storage.memory, storage.offset) != VK_SUCCESS) {
        allocator.release(storage);
        return false;
    }
    return true;
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

bool TargetObjects::built() const noexcept
{
    // A DEPTH-ONLY TARGET (a shadow map) has every object too: it simply owns no colour view. A target with NEITHER
    // attachment is the one shape that is not built.
    const bool attachments = color_views.size() == colorCount() && (!color_views.empty() || hasDepth());
    // A MULTISAMPLED target is not built without its sample buffers: the rendering instance draws into those and
    // resolves them, so a target that lost them has nothing to draw into whatever its resolve images say.
    const bool sample_buffers = !isMultisampled() || multisample_color_views.size() == colorCount();
    // ... and a multisampled DEPTH is not built without its resolve picture either: the instance resolves into that one,
    // and it is the image a shader samples (see depthPicture).
    const bool depth_picture = !resolvesDepth() || hasDepthResolve();
    // A BORROWED depth has no images of its own to check: what it needs is the LENDER's RECORD - the image a barrier
    // names, the view the pass attaches and the layout that image is in all belong to it (see TargetTable::ensure).
    const bool borrowed_picture = !depth_borrowed || (depth_lender != nullptr && depth_lender->built());
    return attachments && sample_buffers && depth_picture && borrowed_picture;
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
 * @brief Destroys the images and views, KEEPING the shape.
 *
 * @details
 * - The extent-bound half of a target (see the header's shape-vs-extent split): a resize replaces exactly this and
 *   must not touch what pipelines were keyed by.
 * - Both halves of a multisampled attachment go: the sample buffers are no more reusable than the resolve images they
 *   were built with (they share one sample count, which is part of the shape).
 *
 * @param table     The device's entry points (volk table).
 * @param device    The logical device.
 * @param allocator The device's memory pool (the slices are given back to it).
 * @param images    The record to strip.
 */
void destroyAttachments(const VolkDeviceTable& table, const VkDevice device, MemoryAllocator& allocator, TargetObjects& images) noexcept
{
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (images.depth_resolve_view != VK_NULL_HANDLE) {
        table.vkDestroyImageView(device, images.depth_resolve_view, nullptr);
        images.depth_resolve_view = VK_NULL_HANDLE;
    }
    if (images.depth_resolve != VK_NULL_HANDLE) {
        table.vkDestroyImage(device, images.depth_resolve, nullptr);
        images.depth_resolve = VK_NULL_HANDLE;
    }
    allocator.release(images.depth_resolve_memory);
    if (images.depth_view != VK_NULL_HANDLE) {
        table.vkDestroyImageView(device, images.depth_view, nullptr);
        images.depth_view = VK_NULL_HANDLE;
    }
    if (images.depth != VK_NULL_HANDLE) {
        table.vkDestroyImage(device, images.depth, nullptr);
        images.depth = VK_NULL_HANDLE;
    }
    allocator.release(images.depth_memory);
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
    for (MemorySlice& memory : images.multisample_color_memory) {
        allocator.release(memory);
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
    for (MemorySlice& memory : images.color_memory) {
        allocator.release(memory);
    }
    images.multisample_colors.clear();
    images.multisample_color_memory.clear();
    images.multisample_color_views.clear();
    images.colors.clear();
    images.color_memory.clear();
    images.color_views.clear();
    // THE LAYOUT RECORDS GO WITH THE IMAGES THEY DESCRIBE: the vectors are re-sized by createAttachments, and a record
    // that outlived its image would be an answer about something that no longer exists.
    images.multisample_color_layouts.clear();
    images.color_layouts.clear();
    images.depth_layout         = ImageLayout::Undefined;
    images.depth_picture_layout = ImageLayout::Undefined;
}

/**
 * @brief Creates the images and their views for `images.desc`.
 *
 * @details
 * - A MULTISAMPLED shape creates the sample buffers FIRST and then their resolve images: the rendering instance draws
 *   into the former and resolves into the latter, so both exist before a pass names them.
 * - The sample buffers ask for no SAMPLED or TRANSFER_SRC usage: nothing samples them and nothing copies them, and
 *   asking for a capability nothing uses would only widen what the driver has to allocate.
 * - On failure the partial objects are destroyed before answering false.
 *
 * @param table     The device's entry points (volk table).
 * @param device    The logical device.
 * @param allocator The device's memory pool.
 * @param images    The record to fill.
 * @return true when every attachment object exists.
 */
[[nodiscard]] bool createAttachments(const VolkDeviceTable& table,
                                     const VkDevice         device,
                                     MemoryAllocator&       allocator,
                                     TargetObjects&          images)
{
    const auto width  = static_cast<std::uint32_t>(images.desc.width);
    const auto height = static_cast<std::uint32_t>(images.desc.height);
    // A SHAPE THAT CANNOT BE RENDERED IS NOT BUILT: no extent, no attachment at all, or more colour attachments than
    // `maxColorAttachments` may ever allow (the recorder's attachment-info array is a stack value of that size - see
    // `kMaxColorAttachments`). `TargetTable::ensure` names the last case before it gets here; this is the belt.
    if (images.desc.width <= 0 || images.desc.height <= 0 || (images.colorCount() == 0U && !images.hasDepth()) ||
        images.colorCount() > kMaxColorAttachments) {
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
        images.multisample_color_memory.resize(images.colorCount());
        images.multisample_color_views.resize(images.colorCount());
        for (std::size_t index = 0; index < images.colorCount(); ++index) {
            image_create.format = images.colorFormat(index);
            if (!createImage(table,
                             device,
                             allocator,
                             image_create,
                             images.multisample_colors[index],
                             images.multisample_color_memory[index]) ||
                !createView(table, device, images.multisample_colors[index], image_create.format, VK_IMAGE_ASPECT_COLOR_BIT, images.multisample_color_views[index])) {
                destroyAttachments(table, device, allocator, images);
                return false;
            }
        }
        image_create.samples = VK_SAMPLE_COUNT_1_BIT;
        image_create.usage   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    // THE PICTURES: drawn into directly on a single-sampled target, RESOLVED into on a multisampled one.
    images.colors.resize(images.colorCount());
    images.color_memory.resize(images.colorCount());
    images.color_views.resize(images.colorCount());
    for (std::size_t index = 0; index < images.colorCount(); ++index) {
        image_create.format = images.colorFormat(index);
        if (!createImage(table, device, allocator, image_create, images.colors[index], images.color_memory[index]) ||
            !createView(table, device, images.colors[index], image_create.format, VK_IMAGE_ASPECT_COLOR_BIT, images.color_views[index])) {
            destroyAttachments(table, device, allocator, images);
            return false;
        }
    }

    // A BORROWED DEPTH IS NEVER CLEARED: the image already holds what the LENDER's pass wrote, and that picture is the
    // whole reason this target borrows. So every pass LOADs it (see `beginPass`), and the borrower owns no depth image
    // of its own.
    if (!images.depth_borrowed && images.hasDepth()) {
        image_create.format  = images.depthFormat();
        // A MULTISAMPLED depth is a DRAW target only (the API forbids sampling or copying it); the single-sample one
        // IS the picture, and it carries the same pair the colour pictures carry: SAMPLED (the shadow path reads it -
        // see Device::resolveShadow) and TRANSFER_SRC (a readDepthBuffer copy reads it).
        image_create.usage   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             (resolved ? 0U : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
        image_create.samples = sample_count;
        if (!createImage(table, device, allocator, image_create, images.depth, images.depth_memory) ||
            !createView(table, device, images.depth, image_create.format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_view)) {
            destroyAttachments(table, device, allocator, images);
            return false;
        }

        // THE DEPTH PICTURE of a multisampled target: the resolve attachment (see TargetObjects::depthPicture). It keeps
        // every capability a single-sample depth attachment used to have - DEPTH_STENCIL_ATTACHMENT (the pass resolves
        // into it, a borrower attaches it), SAMPLED (a shader samples it) and TRANSFER_SRC (a readback copies it) - at
        // one sample per texel.
        if (resolved) {
            image_create.samples = VK_SAMPLE_COUNT_1_BIT;
            image_create.usage   = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (!createImage(table, device, allocator, image_create, images.depth_resolve, images.depth_resolve_memory) ||
                !createView(table, device, images.depth_resolve, image_create.format, VK_IMAGE_ASPECT_DEPTH_BIT, images.depth_resolve_view)) {
                destroyAttachments(table, device, allocator, images);
                return false;
            }
        }
    }

    // THE LAYOUT RECORDS ARE BORN WITH THE IMAGES, one per image, all UNDEFINED (see the header: the command that moves
    // an image is the one that writes its record, and no image has been moved yet).
    images.color_layouts.assign(images.colors.size(), ImageLayout::Undefined);
    images.multisample_color_layouts.assign(images.multisample_colors.size(), ImageLayout::Undefined);
    images.depth_layout         = ImageLayout::Undefined;
    images.depth_picture_layout = ImageLayout::Undefined;

    return true;
}

} // namespace

void TargetObjects::beginPass(const VolkDeviceTable& table, const VkCommandBuffer commands, const PassLoad& load, RecordingTally* const tally)
{
    // TRANSCRIPTION, NOT DERIVATION: every layout below is the record of the image it names. The only decisions taken
    // here are which image plays which role, and those are shape facts (`isMultisampled`, `hasDepth`, `hasDepthResolve`).
    PassAttachments attachments{};
    attachments.extent = VkExtent2D{ static_cast<std::uint32_t>(width()), static_cast<std::uint32_t>(height()) };
    // RENDERING WRITES THE SAMPLE BUFFERS when the shape resolves; the pictures are the resolve targets then.
    if (isMultisampled()) {
        attachments.color   = PassRole{ multisample_colors, multisample_color_views, multisample_color_layouts };
        attachments.picture = PassRole{ colors, color_views, color_layouts };
    }
    else {
        attachments.color = PassRole{ colors, color_views, color_layouts };
    }
    // A SET WITH NO DEPTH ATTACHMENT HAS NO DEPTH IMAGE TO STATE: the role stays absent rather than naming an image that
    // does not exist, and no depth record is written for it below (a colour-only target is an ordinary off-screen
    // target). The resolve picture is a SECOND image, so it is a role of its own.
    if (hasDepth()) {
        attachments.depth      = PassImage{ depthAttachmentImage(), depthAttachmentView(), depthAttachmentLayout() };
        attachments.depth_aspect = depthAspectMask(depthFormat());
    }
    if (hasDepthResolve()) {
        attachments.depth_picture = PassImage{ depth_resolve, depth_resolve_view, depthPictureLayout() };
    }

    // A BORROWED DEPTH IS ALWAYS LOADED AND STORED: the image is the LENDER's, and the depth it holds is the whole
    // reason this target borrows it - clearing it would erase another pass' depth, and discarding it would erase the
    // result the lender still needs. The pass' own policy describes THIS set's attachments, so this is the one place
    // that overrides it (the shape rules already make a borrowing target single-sampled, so its own depth never exists
    // to be cleared).
    PassLoad effective = load;
    if (depth_borrowed) {
        effective.depth.load  = vn::graphics::LoadOp::Load;
        effective.depth.store = vn::graphics::StoreOp::Store;
    }

    beginRendering(table, commands, attachments, effective, tally);
    // THE PASS HAS NOW WRITTEN THEM, in command order (see the header): every image rendering writes - and every resolve
    // target, which the instance writes through - is in the attachment layout from here on. That is one statement per
    // IMAGE, which is why a single-sampled depth does not need a second, contradicting one.
    for (ImageLayout& layout : multisample_color_layouts) {
        layout = ImageLayout::ColorAttachment;
    }
    for (ImageLayout& layout : color_layouts) {
        layout = ImageLayout::ColorAttachment;
    }
    // THE DEPTH RECORDS FOLLOW THE IMAGES THE SET HAS: a colour-only target has none, a borrower's belong to the lender
    // (the setters know - see the header), and a multisampled target keeps its sample buffer and its picture apart.
    if (hasDepth()) {
        setDepthAttachmentLayout(ImageLayout::DepthAttachment);
        setDepthPictureLayout(ImageLayout::DepthAttachment);
    }
    fresh.store(false, std::memory_order_relaxed);
}

void TargetObjects::endPass(const VolkDeviceTable& table, const VkCommandBuffer commands) const noexcept
{
    endRendering(table, commands);
}

bool TargetObjects::transitionDepthPicture(const VolkDeviceTable& table,
                                          const VkCommandBuffer  commands,
                                          const ImageLayout      to,
                                          RecordingTally* const  tally) noexcept
{
    const VkImage    picture = depthPicture();
    const ImageLayout from    = depthPictureLayout();
    if (picture == VK_NULL_HANDLE || from == to) {
        return false; // nothing to move (a borrower owns no picture; an equal record needs no barrier)
    }
    recordBarrier(table,
                  commands,
                  barrierBetween(picture, stateOf(from), stateOf(to), depthAspectMask(depthFormat())),
                  tally);
    setDepthPictureLayout(to);
    return true;
}

void TargetObjects::resetLayouts() noexcept
{
    for (ImageLayout& layout : multisample_color_layouts) {
        layout = ImageLayout::Undefined;
    }
    for (ImageLayout& layout : color_layouts) {
        layout = ImageLayout::Undefined;
    }
    // THE DEPTH HALF MAY BELONG TO ANOTHER RECORD (a borrowed depth is the LENDER's picture), so the rewind goes through
    // the setters - they know whose record it is - instead of touching the fields. Only THIS set's images move: a
    // lender's colour attachments were never written by this set and keep their records.
    setDepthAttachmentLayout(ImageLayout::Undefined);
    setDepthPictureLayout(ImageLayout::Undefined);
    fresh.store(true, std::memory_order_relaxed);
}

bool buildTarget(const VolkDeviceTable& table, const VkDevice device, MemoryAllocator& allocator, TargetObjects& images)
{
    if (images.desc.width <= 0 || images.desc.height <= 0 || (images.colorCount() == 0U && !images.hasDepth()) ||
        images.colorCount() > kMaxColorAttachments) {
        return false;
    }

    // NO RENDER PASS TO BUILD: the attachments' formats are the shape, and a pass states its load ops on the command
    // buffer (see DynamicRendering). Nothing here depends on the extent, which is why a resize keeps every pipeline.
    if (!createAttachments(table, device, allocator, images)) {
        destroyTarget(table, device, allocator, images);
        return false;
    }
    images.fresh.store(true, std::memory_order_relaxed);
    return true;
}

bool resizeTarget(const VolkDeviceTable& table, const VkDevice device, MemoryAllocator& allocator, TargetObjects& images)
{
    if (!images.built()) {
        return false; // nothing to keep: a resize presupposes a built target
    }
    // The SHAPE is unchanged (the caller only takes this path when `planTarget` answered ResizeInPlace), so every
    // pipeline compiled for it stays; only what the extent decides is replaced.
    destroyAttachments(table, device, allocator, images);
    if (!createAttachments(table, device, allocator, images)) {
        destroyTarget(table, device, allocator, images);
        return false;
    }
    images.fresh.store(true, std::memory_order_relaxed);
    return true;
}

void destroyTarget(const VolkDeviceTable& table, const VkDevice device, MemoryAllocator& allocator, TargetObjects& images) noexcept
{
    destroyAttachments(table, device, allocator, images);
    images.depth_borrowed         = false;
    images.borrowed_depth_view    = VK_NULL_HANDLE;
    images.depth_lender.reset();
    images.fresh.store(true, std::memory_order_relaxed);
    images.desc                   = vn::graphics::TargetDesc{};
}

VN_BACKEND_VK_NS_END
