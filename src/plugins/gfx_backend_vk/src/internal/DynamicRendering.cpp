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
    // THE TRANSITIONS FIRST, one barrier per image. The destination is always the rendering layout; the source is the
    // record the owner keeps for THAT image, which may be the same value - see the header's note on why that barrier is
    // not a no-op. A span shorter than the image list states `Undefined` for the images it does not name (see the
    // header's precondition: both producers pass their own records).
    const ImageState color_to = stateOf(ImageLayout::ColorAttachment);
    const ImageState depth_to = stateOf(ImageLayout::DepthAttachment);
    const auto       layoutAt = [](const std::span<const ImageLayout> layouts, const std::size_t index) {
        return index < layouts.size() ? layouts[index] : ImageLayout::Undefined;
    };
    for (std::size_t index = 0; index < attachments.color_images.size(); ++index) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.color_images[index], stateOf(layoutAt(attachments.color_from, index)), color_to, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    for (std::size_t index = 0; index < attachments.picture_images.size(); ++index) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.picture_images[index], stateOf(layoutAt(attachments.picture_from, index)), color_to, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    // THE DEPTH PAIR: the attachment and the picture it resolves into are two images with two RECORDS - or, on a
    // single-sampled set, one image whose owner reports the same layout for both names. A BORROWED depth is one more
    // case of that rule rather than an exception to it: its image is the LENDER's, the layout it states is the one the
    // lender's own record holds, and the barrier that follows then transitions nothing - what it adds is the EXECUTION
    // DEPENDENCY between the lender's pass and this one, which is the half dynamic rendering does not insert.
    if (attachments.depth_image != VK_NULL_HANDLE) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.depth_image, stateOf(attachments.depth_from), depth_to, attachments.depth_aspect));
    }
    if (attachments.depth_resolve_image != VK_NULL_HANDLE) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.depth_resolve_image,
                                     stateOf(attachments.depth_picture_from),
                                     depth_to,
                                     attachments.depth_aspect));
    }

    // THE INSTANCE: one attachment per colour output location, and the depth attachment when the set has one.
    std::vector<VkRenderingAttachmentInfo> colors;
    colors.reserve(attachments.color_views.size());
    for (std::size_t index = 0; index < attachments.color_views.size(); ++index) {
        const VkClearValue clear = index < load.color_clears.size() ? load.color_clears[index] : VkClearValue{};
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
