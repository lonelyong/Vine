#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <span>

#include <volk.h>

#include "ImageLayout.hpp"

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief What ONE dynamic-rendering pass draws into: the images of an attachment set, and where each of them is now.
 *
 * @details
 * - THE 1.3 REPLACEMENT FOR A RENDER PASS AND A FRAMEBUFFER. Dynamic rendering states the attachments on the command
 *   buffer (`vkCmdBeginRendering`) instead of in an object created ahead of time, so nothing has to be built per
 *   attachment set - but nothing is TRANSITIONED ahead of the pass either: the implicit `initialLayout` / `finalLayout`
 *   a render pass gave is the recorder's job now, and the execution dependency between two passes into one attachment
 *   is one dynamic rendering does NOT insert. This value is what those two facts need: the images, in the order the
 *   rendering info states them, and the layout each one holds.
 * - THE FIELDS SPLIT BY WHAT DECIDES THEM. `*_images` / `*_views` are the attachment set (who owns them decides the
 *   values); `*_from` are the layouts (the owner's bookkeeping decides them); nothing here allocates or survives the
 *   call.
 * - A RESOLVE TARGET IS NOT AN ATTACHMENT. A multisampled set draws into sample buffers and resolves into the
 *   pictures: the picture is stated on the sample buffer's attachment info (`resolveMode` / `resolveImageView`), which
 *   is why `picture_*` are parallel to `color_*` and empty on a single-sampled set. The same holds for the depth
 *   resolve, which is one `VkRenderingAttachmentInfo` whose `resolveMode` is `SAMPLE_ZERO` - the mode a render pass
 *   could only state through `VkSubpassDescriptionDepthStencilResolve` and `vkCreateRenderPass2`.
 * - TWO IMAGES, TWO LAYOUTS, ON THE DEPTH SIDE TOO. A multisampled depth attachment and the picture it resolves into
 *   part company the moment a reader samples the picture: the sample buffer stays a depth attachment (it may not be
 *   sampled at all), while the picture moves to `SHADER_READ_ONLY`. That is why `depth_from` and
 *   `depth_picture_from` are two records, exactly as `color_from` and `picture_from` are - deriving one from the other
 *   states a layout for an image that never had it.
 * - A BORROWED DEPTH IS AN IMAGE, NOT AN EXCEPTION. `RenderTarget::shareDepth` gives one image to two targets, so a
 *   borrowing set states the LENDER's picture and the layout it is in (the depth attachment layout, because that is
 *   where the lender's own pass left it). The barrier that follows transitions nothing - what it adds is the execution
 *   dependency between the lender's pass and this one, which is the half dynamic rendering does not insert, and the
 *   one thing a borrowing pass must not skip.
 */
struct PassAttachments {
    /// The extent the rendering instance covers (the attachment set's own; there is no framebuffer to disagree with).
    VkExtent2D extent{};

    /// The images rendering WRITES, one per colour attachment: the sample buffers on a multisampled set, else the
    /// pictures themselves. `color_views` is parallel to it.
    std::span<const VkImage>     color_images{};
    std::span<const VkImageView> color_views{};

    /// The single-sample RESOLVE targets, parallel to `color_*`; empty on a single-sampled set.
    std::span<const VkImage>     picture_images{};
    std::span<const VkImageView> picture_views{};

    /// The depth attachment and, on a set that resolves depth, its single-sample picture (see `TargetObjects`). A
    /// BORROWING set names the LENDER's picture here, so these values belong to ANOTHER record: this set states them and
    /// does not own them.
    VkImage            depth_image{ VK_NULL_HANDLE };
    VkImageView        depth_view{ VK_NULL_HANDLE };
    VkImage            depth_resolve_image{ VK_NULL_HANDLE };
    VkImageView        depth_resolve_view{ VK_NULL_HANDLE };
    /// The aspect a depth barrier over these images must name (`DEPTH|STENCIL` for a combined format).
    VkImageAspectFlags depth_aspect{ VK_IMAGE_ASPECT_DEPTH_BIT };

    /// The layout the images rendering writes are in (colour; meaningless for an empty set).
    ImageLayout color_from{ ImageLayout::Undefined };
    /// The layout the resolve pictures are in (ignored when `picture_images` is empty).
    ImageLayout picture_from{ ImageLayout::Undefined };
    /// The layout the DEPTH ATTACHMENT is in. Never `ShaderReadOnly`: a multisampled depth may not be sampled at all.
    ImageLayout depth_from{ ImageLayout::Undefined };
    /// The layout the depth RESOLVE picture is in (ignored when `depth_resolve_image` is empty). This is the record a
    /// reader's sampling moves.
    ImageLayout depth_picture_from{ ImageLayout::Undefined };
};

/**
 * @brief What one pass states about LOADING and CLEARING its attachments, one value per attachment.
 *
 * @details
 * - THE PASS' TWO INDEPENDENT DIMENSIONS (see `ClearPolicy`): colour and depth are decided separately, which is what
 *   lets a HUD pass keep the picture and own the depth inside its rectangle without a second attachment set.
 * - A CLEAR VALUE IS ONLY READ WHEN THE MATCHING LOAD OP IS `CLEAR`; a loading attachment keeps what an earlier pass
 *   wrote and its value is ignored. `color_clears` is parallel to the colour attachments - one entry per output
 *   location, in the order a fragment's outputs are written - and an absent entry is transparent black, which is
 *   `ClearPolicy`'s rule for an attachment a pass does not state.
 */
struct PassLoad {
    /// Whether the colour attachments LOAD what an earlier pass wrote (false CLEARs them).
    bool load_color{ false };
    /// Whether the depth attachment LOADs what an earlier pass wrote (false CLEARs it).
    bool load_depth{ false };
    /// One clear value per colour attachment, in output-location order (parallel to `color_images`).
    std::span<const VkClearValue> color_clears{};
    /// The clear value of the depth attachment.
    VkClearValue depth{};
};

/**
 * @brief Records the attachment transitions a pass needs, then `vkCmdBeginRendering`.
 *
 * @details
 * - TWO JOBS, ONE CALL SITE. A render pass did both of them implicitly: it moved every attachment from
 *   `initialLayout` to the subpass layout, and its subpass dependency ordered the pass' attachment accesses after what
 *   earlier submissions did. With dynamic rendering neither exists, so this function records one barrier per
 *   attachment - from the layout the set states to the layout rendering requires - and then opens the instance. A
 *   barrier between two IDENTICAL states is recorded on purpose: it is the execution dependency between this pass and
 *   the previous one into the same image, which is the half Vulkan does not insert for dynamic rendering.
 * - CALLER OWNS THE BOOKKEEPING. `color_from` / `picture_from` / `depth_from` / `depth_picture_from` are the facts the
 *   owner tracks (see `TargetObjects` and `WindowTarget`); after this call every attachment is in the rendering layout,
 *   so the owner stores exactly that.
 * - DRAWS FOLLOW, then @ref endRendering. Nothing may be recorded between the barrier set and the draws that needs the
 *   old layout.
 * - ONE ALLOCATION, AND WHY IT STAYS: the colour attachment info array is sized by the set's colour attachment COUNT,
 *   which is a runtime property (`maxColorAttachments` varies by device), so it is a vector - one small allocation per
 *   pass, the same order as the per-draw arrays the content recorder builds. A fixed array would have to cap the count,
 *   and a silent cap is a worse defect than one allocation.
 *
 * @param table       The device's entry points (volk table).
 * @param commands    The command buffer being recorded (outside any rendering instance).
 * @param attachments The attachment set and the layouts it is in (borrowed for the call).
 * @param load        The pass' load policy and clear values.
 */
void beginRendering(const VolkDeviceTable& table, VkCommandBuffer commands, const PassAttachments& attachments, const PassLoad& load);

/**
 * @brief Records `vkCmdEndRendering`, closing the instance @ref beginRendering opened.
 *
 * @details
 * - THE ATTACHMENTS STAY in the layout rendering used (colour attachment / depth attachment): there is no
 *   `finalLayout` to state, and every later reader - another pass, a sampling transition, a readback, the present
 *   transition - names that layout as the one it starts from.
 *
 * @param table    The device's entry points (volk table).
 * @param commands The command buffer the pass was recorded into.
 */
void endRendering(const VolkDeviceTable& table, VkCommandBuffer commands) noexcept;

VN_BACKEND_VK_NS_END
