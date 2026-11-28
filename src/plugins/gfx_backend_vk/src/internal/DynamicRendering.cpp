#include <vine/backend_vk/internal/DynamicRendering.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

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
 * @param store   What the written image's contents are worth (see `PassAttachmentOps`: `DontCare` on a resolved role).
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
    // THE LAYOUT IS THE TABLE'S ROW, NOT A SECOND SPELLING (see BarrierPlan): the barrier that reached this layout is
    // built from the same row, so the instance and the command before it cannot disagree about where the image is.
    info.imageLayout         = stateOf(ImageLayout::ColorAttachment).layout;
    // A RESOLVE IS THE PICTURE'S ONLY WRITER: `AVERAGE` is what a render pass' implicit colour resolve did, and it is
    // the only mode a single-sample picture can state without the sample position of every fragment.
    info.resolveMode         = resolve != VK_NULL_HANDLE ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE;
    info.resolveImageView    = resolve;
    info.resolveImageLayout  = resolve != VK_NULL_HANDLE ? info.imageLayout : stateOf(ImageLayout::Undefined).layout;
    info.loadOp              = load;
    info.storeOp             = store;
    info.clearValue          = clear;
    return info;
}

} // namespace

PassAttachmentOps resolveAttachmentOps(const bool                                  preserve_previous,
                                               const vn::graphics::AttachmentPolicy* const clear,
                                               const std::span<ResolvedAttachment>         color_storage,
                                               const std::size_t                           color_count) noexcept
{
    // THE GROUP SPELLING IS THE DEFAULT, and a stated per-attachment op overrides it for its own attachment.
    // `AttachmentPolicy`'s two group dimensions stay independent (a HUD keeps the colour and owns the depth).
    // NO POLICY AT ALL MEANS NO CLEAR: `RenderPass::attachmentPolicy()` answers nullopt for a pass that clears nothing,
    // so a later pass into a set that already holds a picture must LOAD it - treating nullopt as "clear both" wiped
    // the deferred-lighting result in the demo and left only the last overlay on a black window.
    const bool clear_color = clear != nullptr && clear->clear_color;
    const bool clear_depth = clear != nullptr && clear->clear_depth;

    PassAttachmentOps load{};
    const std::size_t filled = std::min(color_count, color_storage.size());
    for (std::size_t index = 0; index < filled; ++index) {
        const vn::graphics::AttachmentOps* stated = clear != nullptr && index < clear->color_ops.size() ? &clear->color_ops[index] : nullptr;
        ResolvedAttachment&                    slot   = color_storage[index];
        slot.load  = stated != nullptr ? stated->load : (preserve_previous && !clear_color ? vn::graphics::LoadOp::Load : vn::graphics::LoadOp::Clear);
        slot.store = stated != nullptr ? stated->store : vn::graphics::StoreOp::Store;
        // NOTHING TO KEEP IS NOT A LOAD: the group rule already answers Clear on a fresh set, and a stated `Load` gets
        // the same answer instead of reading contents the image never had.
        if (slot.load == vn::graphics::LoadOp::Load && !preserve_previous) {
            slot.load = vn::graphics::LoadOp::Clear;
        }
        // THE CLEAR VALUE, in output-location order: attachment 0 takes the policy's colour and 1..N-1 its extra
        // colours, an absent entry being transparent black; a value beside a Load or a DontCare is never read.
        const Colorf colour =
            index == 0 ? (clear != nullptr ? clear->color : Colorf(0.0F, 0.0F, 0.0F, 1.0F))
                       : (clear != nullptr && index - 1U < clear->extra_colors.size() ? clear->extra_colors[index - 1U] : Colorf(0.0F, 0.0F, 0.0F, 0.0F));
        slot.clear.color = {
            { colour.r, colour.g, colour.b, colour.a }
        };
    }
    load.color = color_storage.first(filled);

    load.depth              = ResolvedAttachment{};
    const bool depth_stated = clear != nullptr && clear->depth_op.has_value();
    load.depth.load =
        depth_stated ? clear->depth_op->load : (preserve_previous && !clear_depth ? vn::graphics::LoadOp::Load : vn::graphics::LoadOp::Clear);
    load.depth.store = depth_stated ? clear->depth_op->store : vn::graphics::StoreOp::Store;
    if (load.depth.load == vn::graphics::LoadOp::Load && !preserve_previous) {
        load.depth.load = vn::graphics::LoadOp::Clear;
    }
    // Reverse-Z: this camera convention's far plane is 0, and the policy states WHETHER, never to what.
    load.depth.clear.depthStencil = { 0.0F, 0U };
    return load;
}

void beginRendering(const VolkDeviceTable&       table,
                    const VkCommandBuffer        commands,
                    const PassAttachmentSet&     attachments,
                    const PassAttachmentOps& load,
                    RecordingTally* const        tally)
{
    // THE TRANSITIONS FIRST, ONE DEPENDENCY FOR THE WHOLE PASS (see the header). The destination is always the rendering
    // layout; the source is the record the owner keeps for THAT image, which may be the same value - `order` appends
    // that same-state barrier on purpose (see BarrierBatch). A role's three spans are the same length in the same
    // order (see PassRole), so one index names the image, its view and its record. The batch is a stack array sized by
    // the API's own attachment cap, and every role together fits: (colour + picture) attachments, plus depth and its
    // picture.
    std::array<VkImageMemoryBarrier2, kMaxColorAttachments * 2U + 2U> entry_storage{};
    BarrierBatch                                                      entry(table, commands, entry_storage, tally);
    for (std::size_t index = 0; index < attachments.color.size(); ++index) {
        entry.order(attachments.color.images[index], attachments.color.from[index], ImageLayout::ColorAttachment, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (std::size_t index = 0; index < attachments.picture.size(); ++index) {
        entry.order(attachments.picture.images[index], attachments.picture.from[index], ImageLayout::ColorAttachment, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // THE DEPTH PAIR: the attachment and the picture it resolves into are two images with two RECORDS - or, on a
    // single-sampled set, one image whose owner reports the same layout for both names, and then `depth_picture` names
    // nothing at all. A BORROWED depth is one more case of that rule rather than an exception to it: its image is the
    // LENDER's, the layout it states is the one the lender's own record holds, and the barrier that follows then
    // transitions nothing - what it adds is the EXECUTION DEPENDENCY between the lender's pass and this one, which is
    // the half dynamic rendering does not insert.
    if (attachments.depth.present()) {
        entry.order(attachments.depth.image, attachments.depth.from, ImageLayout::DepthAttachment, attachments.depth_aspect);
    }
    if (attachments.depth_picture.present()) {
        entry.order(attachments.depth_picture.image, attachments.depth_picture.from, ImageLayout::DepthAttachment, attachments.depth_aspect);
    }
    entry.flush();

    // THE INSTANCE: one attachment per colour output location, and the depth attachment when the set has one.
    //
    // THE STORE POLICY IS DERIVED, NOT REPEATED: a role that RESOLVES into another picture is content nobody may read
    // afterwards (a resolver is the only way out of a sample buffer), so keeping it is bandwidth that buys nothing -
    // `DontCare` is the honest store and the one a tiled renderer can use to skip the flush. The caller states what its
    // images are worth; this call knows whether a resolve consumes them, which is why the downgrade lives here.
    const bool                                                  colour_resolved = !attachments.picture.empty();
    const bool                                                  depth_resolved  = attachments.depth_picture.present();
    const ResolvedAttachment                                    fallback{};
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colors{};
    for (std::size_t index = 0; index < attachments.color.size(); ++index) {
        // ONE ENTRY PER ATTACHMENT (see PassAttachmentOps): the index names the image, its view and its own op. A caller that
        // states fewer entries than the set has attachments falls back to the defaults rather than reading past the
        // span - the recorder never trusts a count it did not take from the same owner.
        const ResolvedAttachment& op = index < load.color.size() ? load.color[index] : fallback;
        colors[index]                = colorAttachmentInfo(attachments.color.views[index],
                                                           attachments.picture.viewAt(index),
                                                           loadOpOf(op.load),
                                                           storeOpOf(colour_resolved ? vn::graphics::StoreOp::DontCare : op.store),
                                                           op.clear);
    }

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (attachments.depth.view != VK_NULL_HANDLE) {
        depth.imageView = attachments.depth.view;
        // THE SAME RULE AS THE COLOUR ROLES: the row the pass' barrier used is the row the instance states.
        depth.imageLayout = stateOf(ImageLayout::DepthAttachment).layout;
        // THE DEPTH RESOLVE IS ONE FIELD PAIR now (`SAMPLE_ZERO`, the only mode that cannot change the picture), where a
        // render pass needed `VkSubpassDescriptionDepthStencilResolve` and the 1.2 creation entry point. It also makes
        // the sample buffer disposable: the resolve is the only reader.
        depth.resolveMode        = attachments.depth_picture.view != VK_NULL_HANDLE ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;
        depth.resolveImageView   = attachments.depth_picture.view;
        depth.resolveImageLayout = attachments.depth_picture.view != VK_NULL_HANDLE ? depth.imageLayout
                                                                                     : stateOf(ImageLayout::Undefined).layout;
        depth.loadOp             = loadOpOf(load.depth.load);
        depth.storeOp            = storeOpOf(depth_resolved ? vn::graphics::StoreOp::DontCare : load.depth.store);
        depth.clearValue         = load.depth.clear;
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
