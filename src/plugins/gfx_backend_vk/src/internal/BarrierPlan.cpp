#include <vine/backend_vk/internal/BarrierPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

using vn::graphics::backend::ImageLayout;
using vn::graphics::backend::LoadOp;
using vn::graphics::backend::StoreOp;

ImageState stateOf(const ImageLayout usage) noexcept
{
    // THE TABLE, one row per usage. Every row states all three spellings together, because they are one fact: a layout
    // without its stages waits for nothing, and an access mask under the wrong layout makes writes visible to a reader
    // that is not allowed to read yet.
    switch (usage) {
    case ImageLayout::Undefined:
        return ImageState{ VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           0 };
    case ImageLayout::ColorAttachment:
        return ImageState{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    case ImageLayout::DepthAttachment:
        return ImageState{ VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    case ImageLayout::ShaderReadOnly:
        return ImageState{ VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_SHADER_READ_BIT };
    case ImageLayout::Present:
        // The one layout the platform owns rather than a queue operation: nothing is synchronised beyond handing the
        // image over, which is why it carries the bottom-of-pipe stage and no access mask.
        return ImageState{ VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
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

ImageState transferReadState() noexcept
{
    return ImageState{ VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT };
}

VkImageMemoryBarrier barrierBetween(const VkImage            image,
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

VkImageMemoryBarrier barrierBetween(const VkImage               image,
                                    const ImageState&            from,
                                    const ImageState&            to,
                                    const VkImageSubresourceRange& range) noexcept
{
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask       = from.access;
    barrier.dstAccessMask       = to.access;
    barrier.oldLayout           = from.layout;
    barrier.newLayout           = to.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = range;
    return barrier;
}

VkImageMemoryBarrier barrierFor(const VkImage              image,
                                const ImageLayout          from,
                                const ImageLayout          to,
                                const VkImageAspectFlags   aspect) noexcept
{
    return barrierBetween(image, stateOf(from), stateOf(to), aspect);
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
