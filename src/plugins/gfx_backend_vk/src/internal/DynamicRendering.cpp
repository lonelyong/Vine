#include <vine/backend_vk/internal/DynamicRendering.hpp>

#include <array>

#include <vine/backend_vk/internal/BarrierPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Fills one colour attachment's rendering info.
 *
 * @param view    The image rendering writes.
 * @param resolve The picture it resolves into, or null.
 * @param load    The pass' colour load op (one value for every colour attachment).
 * @param store   What the written image's contents are worth (see `PassLoad`: `DontCare` on a resolved role).
 * @param clear   This attachment's clear value (read only when loading is off).
 * @return The attachment info.
 */
[[nodiscard]] VkRenderingAttachmentInfo colorAttachmentInfo(const VkImageView            view,
                                                            const VkImageView            resolve,
                                                            const VkAttachmentLoadOp     load,
                                                            const VkAttachmentStoreOp    store,
                                                            const VkClearValue&          clear) noexcept
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
    info.storeOp             = store;
    info.clearValue          = clear;
    return info;
}

} // namespace

void beginRendering(const VolkDeviceTable& table, const VkCommandBuffer commands, const PassAttachments& attachments, const PassLoad& load)
{
    // THE TRANSITIONS FIRST, one barrier per image. The destination is always the rendering layout; the source is the
    // record the owner keeps for THAT image, which may be the same value - see the header's note on why that barrier is
    // not a no-op. A role's three spans are the same length in the same order (see PassRole), so one index names the
    // image, its view and its record.
    const ImageState color_to = stateOf(ImageLayout::ColorAttachment);
    const ImageState depth_to = stateOf(ImageLayout::DepthAttachment);
    for (std::size_t index = 0; index < attachments.color.size(); ++index) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.color.images[index], stateOf(attachments.color.from[index]), color_to, VK_IMAGE_ASPECT_COLOR_BIT));
    }
    for (std::size_t index = 0; index < attachments.picture.size(); ++index) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.picture.images[index],
                                     stateOf(attachments.picture.from[index]),
                                     color_to,
                                     VK_IMAGE_ASPECT_COLOR_BIT));
    }
    // THE DEPTH PAIR: the attachment and the picture it resolves into are two images with two RECORDS - or, on a
    // single-sampled set, one image whose owner reports the same layout for both names, and then `depth_picture` names
    // nothing at all. A BORROWED depth is one more case of that rule rather than an exception to it: its image is the
    // LENDER's, the layout it states is the one the lender's own record holds, and the barrier that follows then
    // transitions nothing - what it adds is the EXECUTION DEPENDENCY between the lender's pass and this one, which is
    // the half dynamic rendering does not insert.
    if (attachments.depth.present()) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.depth.image, stateOf(attachments.depth.from), depth_to, attachments.depth_aspect));
    }
    if (attachments.depth_picture.present()) {
        recordBarrier(table,
                      commands,
                      barrierBetween(attachments.depth_picture.image,
                                     stateOf(attachments.depth_picture.from),
                                     depth_to,
                                     attachments.depth_aspect));
    }

    // THE INSTANCE: one attachment per colour output location, and the depth attachment when the set has one.
    //
    // THE STORE POLICY IS DERIVED, NOT REPEATED: a role that RESOLVES into another picture is content nobody may read
    // afterwards (a resolver is the only way out of a sample buffer), so keeping it is bandwidth that buys nothing -
    // `DontCare` is the honest store and the one a tiled renderer can use to skip the flush. The caller states what its
    // images are worth; this call knows whether a resolve consumes them, which is why the downgrade lives here.
    const bool                       colour_resolved = !attachments.picture.empty();
    const bool                       depth_resolved  = attachments.depth_picture.present();
    const VkAttachmentLoadOp         colour_load     = loadOpOf(load.load_color);
    const VkAttachmentLoadOp         depth_load      = loadOpOf(load.load_depth);
    const VkAttachmentStoreOp        colour_store    = storeOpOf(colour_resolved ? vn::graphics::StoreOp::DontCare : load.store_color);
    const VkAttachmentStoreOp        depth_store     = storeOpOf(depth_resolved ? vn::graphics::StoreOp::DontCare : load.store_depth);
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colors{};
    for (std::size_t index = 0; index < attachments.color.size(); ++index) {
        const VkClearValue clear = index < load.color_clears.size() ? load.color_clears[index] : VkClearValue{};
        colors[index] = colorAttachmentInfo(attachments.color.views[index], attachments.picture.viewAt(index), colour_load, colour_store, clear);
    }

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (attachments.depth.view != VK_NULL_HANDLE) {
        depth.imageView = attachments.depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        // THE DEPTH RESOLVE IS ONE FIELD PAIR now (`SAMPLE_ZERO`, the only mode that cannot change the picture), where a
        // render pass needed `VkSubpassDescriptionDepthStencilResolve` and the 1.2 creation entry point. It also makes
        // the sample buffer disposable: the resolve is the only reader.
        depth.resolveMode        = attachments.depth_picture.view != VK_NULL_HANDLE ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        depth.resolveImageView   = attachments.depth_picture.view;
        depth.resolveImageLayout = attachments.depth_picture.view != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                                                     : VK_IMAGE_LAYOUT_UNDEFINED;
        depth.loadOp     = depth_load;
        depth.storeOp    = depth_store;
        depth.clearValue = load.depth;
    }

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.offset    = VkOffset2D{ 0, 0 };
    rendering.renderArea.extent    = attachments.extent;
    rendering.layerCount           = 1U;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(attachments.color.size());
    rendering.pColorAttachments    = colors.data();
    rendering.pDepthAttachment     = attachments.depth.view != VK_NULL_HANDLE ? &depth : nullptr;
    table.vkCmdBeginRendering(commands, &rendering);
}

void endRendering(const VolkDeviceTable& table, const VkCommandBuffer commands) noexcept
{
    table.vkCmdEndRendering(commands);
}

VN_BACKEND_VK_NS_END
