#include <vine/backend_vk/internal/BarrierPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::backend_vk::ImageLayout;
using vn::graphics::LoadOp;
using vn::graphics::StoreOp;

ImageState stateOf(const ImageLayout usage) noexcept
{
    // THE TABLE, one row per usage. Every row states all three spellings together, because they are one fact: a layout
    // without its stages waits for nothing, and an access mask under the wrong layout makes writes visible to a reader
    // that is not allowed to read yet.
    switch (usage) {
    case ImageLayout::Undefined:
        return ImageState{ VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                           0 };
    case ImageLayout::ColorAttachment:
        // READ AND WRITE: a pass both writes the attachment and reads it (a LOAD op, a blend against the destination).
        // The read half is stated here because dynamic rendering inserts no subpass dependency: a barrier whose dst
        // access names only the write does not order the LOAD that follows it (see DynamicRendering::beginRendering).
        return ImageState{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT };
    case ImageLayout::DepthAttachment:
        // READ AND WRITE for the same reason as the colour row: the depth TEST reads the attachment and the write mask
        // writes it, and a pass that LOADs a depth an earlier pass wrote needs the read in the destination scope.
        return ImageState{ VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    case ImageLayout::ShaderReadOnly:
        return ImageState{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_2_SHADER_READ_BIT };
    case ImageLayout::Present:
        // The one layout the platform owns rather than a queue operation: nothing is synchronised beyond handing the
        // image over, which is why it carries the bottom-of-pipe stage and no access mask.
        return ImageState{ VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                           0 };
    }
    // An out-of-range value is not a usage this backend serves; answering "undefined" is the honest fallback because it
    // discards contents instead of pretending they are valid in a layout nobody asked for.
    return ImageState{};
}

bool requiresTransition(const ImageLayout from, const ImageLayout to) noexcept
{
    return from != to;
}

ImageState transferWriteState() noexcept
{
    return ImageState{ VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT };
}

ImageState transferReadState() noexcept
{
    return ImageState{ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT };
}

VkImageMemoryBarrier2 barrierBetween(const VkImage            image,
                                     const ImageState&        from,
                                     const ImageState&        to,
                                     const VkImageAspectFlags aspect) noexcept
{
    VkImageSubresourceRange range{};
    range.aspectMask     = aspect;
    range.levelCount     = 1U;
    range.layerCount     = 1U;
    return barrierBetween(image, from, to, range);
}

VkImageMemoryBarrier2 barrierBetween(const VkImage               image,
                                     const ImageState&            from,
                                     const ImageState&            to,
                                     const VkImageSubresourceRange& range) noexcept
{
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = from.stages;
    barrier.srcAccessMask       = from.access;
    barrier.dstStageMask        = to.stages;
    barrier.dstAccessMask       = to.access;
    barrier.oldLayout           = from.layout;
    barrier.newLayout           = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = range;
    return barrier;
}

VkImageMemoryBarrier2 barrierFor(const VkImage              image,
                                 const ImageLayout          from,
                                 const ImageLayout          to,
                                 const VkImageAspectFlags   aspect) noexcept
{
    return barrierBetween(image, stateOf(from), stateOf(to), aspect);
}

void recordBarriers(const VolkDeviceTable&                 table,
                    const VkCommandBuffer                  commands,
                    const std::span<const VkImageMemoryBarrier2> barriers,
                    RecordingTally* const                  tally) noexcept
{
    // ONE DEPENDENCY, N BARRIERS (see the header). An empty batch owes the queue nothing: a pass with no image to move
    // records no command at all rather than an empty one.
    if (barriers.empty()) {
        return;
    }
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers    = barriers.data();
    table.vkCmdPipelineBarrier2(commands, &dependency);
    if (tally != nullptr) {
        ++tally->dependencies;
        tally->barriers += barriers.size();
    }
}

void recordBarrier(const VolkDeviceTable&       table,
                   const VkCommandBuffer        commands,
                   const VkImageMemoryBarrier2& barrier,
                   RecordingTally* const        tally) noexcept
{
    recordBarriers(table, commands, std::span<const VkImageMemoryBarrier2>(&barrier, 1U), tally);
}

BarrierBatch::BarrierBatch(const VolkDeviceTable&              table,
                           const VkCommandBuffer               commands,
                           const std::span<VkImageMemoryBarrier2> storage,
                           RecordingTally* const               tally) noexcept
  : table_(&table)
  , commands_(commands)
  , storage_(storage)
  , tally_(tally)
{
}

bool BarrierBatch::move(const VkImage image, ImageLayout& record, const ImageLayout to, const VkImageAspectFlags aspect) noexcept
{
    // THE SAME RULE AS transitionImage (see there): nothing to do when the record already states the target, and nothing
    // to state for an absent image.
    if (record == to || image == VK_NULL_HANDLE) {
        return false;
    }
    if (used_ >= storage_.size()) {
        return false; // the caller sized the batch by the roles it states; a full batch is its defect, not the device's
    }
    storage_[used_++] = barrierBetween(image, stateOf(record), stateOf(to), aspect);
    record            = to;
    return true;
}

void BarrierBatch::flush() noexcept
{
    if (used_ == 0U) {
        return;
    }
    recordBarriers(*table_, commands_, storage_.first(used_), tally_);
    used_ = 0U;
}

bool transitionImage(const VolkDeviceTable& table,
                     const VkCommandBuffer  commands,
                     const VkImage          image,
                     ImageLayout&           record,
                     const ImageLayout      to,
                     const VkImageAspectFlags aspect,
                     RecordingTally* const  tally) noexcept
{
    // ONE CALL FOR THE PAIR (see the header): a mover that recorded a barrier without writing the record - or wrote a
    // record the barrier did not state - is the defect this signature removes. Nothing to do when the image is already
    // where the caller wants it, which is also the "an earlier pass of this frame already stated it" skip.
    if (record == to || image == VK_NULL_HANDLE) {
        return false;
    }
    recordBarrier(table, commands, barrierBetween(image, stateOf(record), stateOf(to), aspect), tally);
    record = to;
    return true;
}

VkAttachmentLoadOp loadOpOf(const LoadOp op) noexcept
{
    switch (op) {
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp storeOpOf(const StoreOp op) noexcept
{
    switch (op) {
    case StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VN_BACKEND_VK_NS_END
