#include <vine/backend_vk/internal/DynamicRendering.hpp>

#include <vector>

#include <vine/backend_vk/internal/BarrierPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Fills one colour attachment's rendering info.
 *
 * @param view      The image rendering writes.
 * @param resolve   The picture it resolves into, or null.
 * @param load      The pass' colour load op (one value for every colour attachment).
 * @param clear     This attachment's clear value (read only when loading is off).
 * @return The attachment info.
 */
[[nodiscard]] VkRenderingAttachmentInfo colorAttachmentInfo(const VkImageView     view,
                                                            const VkImageView     resolve,
                                                            const VkAttachmentLoadOp load,
                                                            const VkClearValue&   clear) noexcept
{
    VkRenderingAttachmentInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    info.imageView           = view;
    info.imageLayout         = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // A RESOLVE IS THE PICTURE'S ONLY WRITER: `AVERAGE` is what a render pass' implicit colour resolve did, and it is
    // the only mode a single-sample picture can state without the sample position of every fragment.
    info.resolveMode         = resolve != VK_NULL_HANDLE ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE;
    info.resolveImageView    = resolve;
    info.resolveImageLayout  = resolve != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    info.loadOp              = load;
    info.storeOp             = VK_ATTACHMENT_STORE_OP_STORE;
    info.clearValue          = clear;
    return info;
}

} // namespace

void beginRendering(const VolkDeviceTable& table, const VkCommandBuffer commands, const PassAttachments& attachments, const PassLoad& load)
{
    // THE TRANSITIONS FIRST, one barrier per image. The destination is always the rendering layout; the source is what
    // the set states, which may be the same value - see the header's note on why that barrier is not a no-op.
    const ImageState color_to  = stateOf(ImageLayout::ColorAttachment);
    const ImageState depth_to  = stateOf(ImageLayout::DepthAttachment);
    const ImageState color_now = stateOf(attachments.color_from);
    for (const VkImage image : attachments.color_images) {
        recordBarrier(table, commands, barrierBetween(image, color_now, color_to, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    const ImageState picture_now = stateOf(attachments.picture_from);
    for (const VkImage image : attachments.picture_images) {
        recordBarrier(table, commands, barrierBetween(image, picture_now, color_to, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    // A BORROWED DEPTH BELONGS TO THE LENDER: it keeps the layout the lender's own pass left it in, and the borrower
    // was rebuilt whenever that picture moved - so there is no transition for this pass to record (see the header).
    if (!attachments.depth_borrowed) {
        const ImageState depth_now = stateOf(attachments.depth_from);
        if (attachments.depth_image != VK_NULL_HANDLE) {
            recordBarrier(table, commands, barrierBetween(attachments.depth_image, depth_now, depth_to, attachments.depth_aspect));
        }
        if (attachments.depth_resolve_image != VK_NULL_HANDLE) {
            recordBarrier(table, commands, barrierBetween(attachments.depth_resolve_image, depth_now, depth_to, attachments.depth_aspect));
        }
    }

    // THE INSTANCE: one attachment per colour output location, and the depth attachment when the set has one.
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(attachments.color_views.size());
    for (std::size_t index = 0; index < attachments.color_views.size(); ++index) {
        const VkClearValue clear = index == 0U ? load.color : (index - 1U < load.extra_colors.size() ? load.extra_colors[index - 1U] : VkClearValue{});
        colors.push_back(colorAttachmentInfo(attachments.color_views[index],
                                             index < attachments.picture_views.size() ? attachments.picture_views[index] : VK_NULL_HANDLE,
                                             load.load_color ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                             clear));
    }

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (attachments.depth_view != VK_NULL_HANDLE) {
        depth.imageView = attachments.depth_view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        // THE DEPTH RESOLVE IS ONE FIELD PAIR now (`SAMPLE_ZERO`, the only mode that cannot change the picture), where a
        // render pass needed `VkSubpassDescriptionDepthStencilResolve` and the 1.2 creation entry point.
        depth.resolveMode        = attachments.depth_resolve_view != VK_NULL_HANDLE ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        depth.resolveImageView   = attachments.depth_resolve_view;
        depth.resolveImageLayout = attachments.depth_resolve_view != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                                                     : VK_IMAGE_LAYOUT_UNDEFINED;
        depth.loadOp     = load.load_depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp    = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = load.depth;
    }

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.offset    = VkOffset2D{ 0, 0 };
    rendering.renderArea.extent    = attachments.extent;
    rendering.layerCount           = 1U;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    rendering.pColorAttachments    = colors.empty() ? nullptr : colors.data();
    rendering.pDepthAttachment     = attachments.depth_view != VK_NULL_HANDLE ? &depth : nullptr;
    table.vkCmdBeginRendering(commands, &rendering);
}

void endRendering(const VolkDeviceTable& table, const VkCommandBuffer commands) noexcept
{
    table.vkCmdEndRendering(commands);
}

VN_BACKEND_VK_NS_END
