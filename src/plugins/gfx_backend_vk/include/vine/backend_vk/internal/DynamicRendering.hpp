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
 * - FOUR ROLES, ONE VALUE EACH. A pass states the images it WRITES (`color`), the pictures it resolves them into
 *   (`picture`), the depth attachment (`depth`) and the depth resolve picture (`depth_picture`). A role with one image is
 *   a value (@ref PassImage); a role with one image per colour attachment is three parallel spans (@ref PassRole) - see
 *   there for why it is not an array of structs.
 * - ONE RECORD PER IMAGE, WHICH IS WHY A ROLE'S STARTING LAYOUTS ARE PARALLEL TO ITS IMAGES. Two colour images of one
 *   set may be in different places (a readback touches one, a screen draw samples all), so the value that fits is one
 *   entry per image, not one value for the set. The owner passes its own records and stores them back after the call.
 * - A RESOLVE TARGET IS NOT AN ATTACHMENT. A multisampled set draws into sample buffers and resolves into the
 *   pictures: the picture is stated on the sample buffer's attachment info (`resolveMode` / `resolveImageView`), which
 *   is why `picture` is parallel to `color` and empty on a single-sampled set. The same holds for the depth
 *   resolve, which is one `VkRenderingAttachmentInfo` whose `resolveMode` is `SAMPLE_ZERO` - the mode a render pass
 *   could only state through `VkSubpassDescriptionDepthStencilResolve` and `vkCreateRenderPass2`.
 * - TWO IMAGES, TWO LAYOUTS, ON THE DEPTH SIDE TOO. A multisampled depth attachment and the picture it resolves into
 *   part company the moment a reader samples the picture: the sample buffer stays a depth attachment (it may not be
 *   sampled at all), while the picture moves to `SHADER_READ_ONLY`. That is why `depth` and `depth_picture` are two
 *   roles - and on a SINGLE-SAMPLED set the two names resolve to ONE image, which has ONE record (see
 *   `TargetObjects::depthPictureLayout`), so `depth_picture` is simply absent.
 * - A BORROWED DEPTH IS AN IMAGE, NOT AN EXCEPTION. `RenderTarget::shareDepth` gives one image to two targets, so a
 *   borrowing set states the LENDER's picture in `depth` and the layout the LENDER records for it - not an assumed depth
 *   attachment layout. The barrier that follows may then transition nothing - what it adds is the execution dependency
 *   between the lender's pass and this one, which is the half dynamic rendering does not insert, and the one thing a
 *   borrowing pass must not skip.
 */
struct PassImage {
    /// The image a barrier names (null when this role names nothing).
    VkImage     image{ VK_NULL_HANDLE };
    /// The view the rendering info states for it.
    VkImageView view{ VK_NULL_HANDLE };
    /// Where the image is now: what a barrier that starts at it must state as `oldLayout`.
    ImageLayout from{ ImageLayout::Undefined };

    /** @brief Answers whether this role names an image at all. */
    [[nodiscard]] bool present() const noexcept
    {
        return image != VK_NULL_HANDLE;
    }
};

/**
 * @brief ONE attachment role with several images: the images, the views the rendering info names, and where each is.
 *
 * @details
 * - WHY THREE SPANS AND NOT ONE `std::span<const PassImage>` (`G10`). The three are written together by their owner and
 *   read together here, but a full-screen draw binds the views as ONE contiguous `VkImageView` span (see
 *   `ContentDraw::ScreenSource`), and an array of structs would have to be copied into scratch storage on every pass -
 *   an allocation on the recording path, which this backend counts and forbids.
 * - THE THREE ARE THE SAME LENGTH, in the same order, for the same attachments: the owner sizes them together
 *   (`TargetObjects::createAttachments`) and `beginPass` hands over its own vectors, so `size()` is the one number and
 *   an index means the same attachment in all three. Nothing here re-checks that - a producer that broke it would have
 *   to break the owner's own assignment first.
 * - EMPTY MEANS "NO IMAGE OF THIS ROLE": a single-sampled set has no sample buffers and no resolve picture.
 */
struct PassRole {
    std::span<const VkImage>     images{};
    std::span<const VkImageView> views{};
    std::span<const ImageLayout> from{};

    /** @brief Gets how many images this role has (the number all three spans state). */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return images.size();
    }

    /** @brief Answers whether this role names any image. */
    [[nodiscard]] bool empty() const noexcept
    {
        return images.empty();
    }

    /**
     * @brief Gets the view at @p index, or null when this role has no image there.
     *
     * @details
     * - ABSENCE IS THE NORMAL CASE, not a mismatch: a single-sampled set has no resolve picture, so `picture.viewAt(i)`
     *   answers null for every i and the rendering info states no resolve.
     *
     * @param index Position of the attachment (the same index in `images` and `from`).
     * @return The view, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkImageView viewAt(std::size_t index) const noexcept
    {
        return index < views.size() ? views[index] : VK_NULL_HANDLE;
    }
};

struct PassAttachments {
    /// The extent the rendering instance covers (the attachment set's own; there is no framebuffer to disagree with).
    VkExtent2D extent{};

    /// The images rendering WRITES, one per colour attachment: the sample buffers on a multisampled set, else the
    /// pictures themselves.
    PassRole color{};
    /// The single-sample RESOLVE targets, parallel to `color`; empty on a single-sampled set.
    PassRole picture{};
    /// The depth ATTACHMENT. A BORROWING set names the LENDER's picture here, so the values belong to ANOTHER record:
    /// this set states them and does not own them.
    PassImage depth{};
    /// The depth RESOLVE picture - a DIFFERENT image, which is why it is its own role; absent on a single-sampled set,
    /// where the attachment is already the picture.
    PassImage depth_picture{};
    /// The aspect a depth barrier over those images must name (`DEPTH|STENCIL` for a combined format).
    VkImageAspectFlags depth_aspect{ VK_IMAGE_ASPECT_DEPTH_BIT };
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
    /// One clear value per colour attachment, in output-location order (parallel to the `color` role).
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
 * - CALLER OWNS THE BOOKKEEPING. The `from` values a role carries are the facts the owner tracks, one per image (see
 *   `TargetObjects` and `WindowTarget`); after this call every attachment is in the rendering layout, so the owner
 *   stores exactly that.
 * - DRAWS FOLLOW, then @ref endRendering. Nothing may be recorded between the barrier set and the draws that needs the
 *   old layout.
 * - ONE ALLOCATION, AND WHY IT STAYS: the colour attachment info array is sized by the set's colour attachment COUNT,
 *   which is a runtime property (`maxColorAttachments` varies by device), so it is a vector - one small allocation per
 *   pass, the same order as the per-draw arrays the content recorder builds. A fixed array would have to cap the count,
 *   and a silent cap is a worse defect than one allocation.
 *
 * @param table       The device's entry points (volk table).
 * @param commands    The command buffer being recorded (outside any rendering instance).
 * @param attachments The attachment set, as roles (borrowed for the call; a role's spans are its own images' records).
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
