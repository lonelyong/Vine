#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <span>

#include <volk.h>

#include <vine/graphics/DrawState.hpp>
#include <vine/graphics/FrameDescription.hpp>

#include "BarrierPlan.hpp"
#include "ImageLayout.hpp"

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The most colour attachments a Vulkan device may present to one rendering instance: the bound every stack
 *        buffer on the recording path is sized by.
 *
 * @details
 * - `VkPhysicalDeviceLimits::maxColorAttachments` is capped at 8 by the specification, so a fixed array of this size
 *   holds every legal set - which is what lets `beginRendering` build its attachment-info array without allocating
 *   (see `PassRole`'s note on why the recording path allocates nothing).
 * - A SHAPE THAT ASKS FOR MORE IS REFUSED WHERE IT IS BUILT, not silently truncated here: `TargetTable::ensure`
 *   answers `vk.target.too_many_colors` and builds nothing, so the recorder can rely on this bound.
 */
inline constexpr std::size_t kMaxColorAttachments = 8U;

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

struct PassAttachmentSet {
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
 * @brief What ONE attachment does in this pass: its load op, its store op and the value a Clear writes.
 *
 * @details
 * - THE SEAM'S OWN VOCABULARY. A load is "keep what is there", "make a value" or "nothing to keep"; a store is
 *   "keep the result" or "nothing will read it" - `vn::graphics::LoadOp` / `StoreOp`, the neutral pair each family
 *   binds to its API. Stating them per attachment is what makes `BarrierPlan::loadOpOf` / `storeOpOf` the ONE place
 *   those facts become Vulkan enums, instead of a second conditional inside the recorder (`G1` / `G6`).
 * - WHY THE STORE IS STATED AND NOT ASSUMED. This backend keeps every attachment's contents by default, but a
 *   MULTISAMPLED pass resolves into a separate picture: the SAMPLE BUFFERS are then content nothing can read, so
 *   keeping them is pure bandwidth and, on a tiled renderer, a tile flush nobody consumes (`G10`). `beginRendering`
 *   therefore DOWNGRADES a resolved role to `DontCare` - the rule lives where the resolve is known, so a caller
 *   states what IT wants and cannot get the resolve case wrong.
 * - THE CLEAR VALUE IS PART OF THE ENTRY, but it is read only when `load` is `Clear`: a `Load`/`DontCare` attachment
 *   ignores the field beside it, and `AttachmentPolicy` is where the author states what a Clear writes.
 */
struct ResolvedAttachment {
    /// What this attachment loads before the first draw.
    vn::graphics::LoadOp load{ vn::graphics::LoadOp::Clear };
    /// What this attachment's contents are worth after the pass.
    vn::graphics::StoreOp store{ vn::graphics::StoreOp::Store };
    /// The value a `LoadOp::Clear` writes (ignored for `Load` and `DontCare`).
    VkClearValue clear{};
};

/**
 * @brief What one pass states about its attachments' LOAD and STORE: ONE VALUE PER ATTACHMENT.
 *
 * @details
 * - ONE ENTRY PER COLOUR ATTACHMENT, in output-location order (the order `PassAttachmentSet::color` names them), plus
 *   one for the depth attachment. A pass can therefore keep one image, reset another and discard a third in ONE
 *   rendering instance - the case a per-role pair could not express (`G4`).
 * - THE COLOUR AND DEPTH HALVES ARE INDEPENDENT (see `AttachmentPolicy`): a HUD pass keeps the picture and owns the depth
 *   inside its rectangle without a second attachment set.
 */
struct PassAttachmentOps {
    /// One entry per colour attachment, in output-location order (parallel to the `color` role).
    std::span<const ResolvedAttachment> color{};
    /// The depth attachment's entry (its clear value is the reverse-Z far plane).
    ResolvedAttachment depth{};
};

/**
 * @brief DECIDES each attachment's load and store from the pass' policy and ONE question: does the set hold a picture?
 *
 * @details
 * - THE RULE HAS ONE HOME (`G1`). The group spelling is the default: a pass loads when the set already holds a
 *   picture worth keeping AND the pass does not ask to reset that attachment. A stated per-attachment op overrides
 *   the group for ITS attachment, so "keep one, reset another, discard a third" needs no second mechanism (`G4`).
 * - NO POLICY MEANS NO CLEAR. `RenderPass::attachmentPolicy()` answers nullopt for a pass that clears NOTHING, so a
 *   later pass into a set that already holds a picture LOADs it; only a FRESH set (nothing to keep) falls back to
 *   `Clear`, which is the one safety net the input cannot express.
 * - WHAT THE INPUT IS NOT. It is not "is this target fresh": it is "may a later pass load what is there" - a
 *   window's acquired image answers false however many frames have presented through it (see
 *   `PassTarget::holdsPicture`).
 * - `Load` ON A SET WITH NO PICTURE IS NOT HONOURED: there is nothing to keep, so it becomes `Clear` with the value
 *   that attachment states. The group spelling already had that safety net; a stated op gets the same answer instead
 *   of reading undefined contents.
 * - THE CLEAR VALUES ARE FILLED IN OUTPUT-LOCATION ORDER: attachment 0 takes the policy's colour, 1..N-1 its extra
 *   colours, an absent entry is transparent black (an auxiliary attachment's "nothing here"), and a `Load` or
 *   `DontCare` op ignores the value beside it. A DEPTH-ONLY pass has no colour slot, which is why depth is its own
 *   entry.
 * - REVERSE-Z: depth is cleared to this camera convention's far plane, 0 - the policy states WHETHER, never to what.
 * - STORE IS `Store` unless the policy states otherwise; `beginRendering` alone may DOWNGRADE a resolved role to
 *   `DontCare`, because only it knows whether a resolve consumes the images.
 *
 * @param preserve_previous Whether an earlier pass (or frame) put a picture in these attachments that may be kept.
 * @param clear             The pass' policy, or null when the pass announced none (nothing is cleared; a fresh set
 *                          still falls back to `Clear`).
 * @param color_storage     Storage for one entry per colour attachment (the caller's array; untouched past
 *                          @p color_count).
 * @param color_count       How many colour attachments the set has (see `PassTarget::colorCount`).
 * @return The pass' per-attachment policy, with @p color_storage filled and `color` naming its prefix.
 */
[[nodiscard]] PassAttachmentOps resolveAttachmentOps(bool                                  preserve_previous,
                                                             const vn::graphics::AttachmentPolicy* clear,
                                                             std::span<ResolvedAttachment>         color_storage,
                                                             std::size_t                           color_count) noexcept;

/**
 * @brief Records the attachment transitions a pass needs, then `vkCmdBeginRendering`.
 *
 * @details
 * - TWO JOBS, ONE CALL SITE. A render pass did both of them implicitly: it moved every attachment from
 *   `initialLayout` to the subpass layout, and its subpass dependency ordered the pass' attachment accesses after what
 *   earlier submissions did. With dynamic rendering neither exists, so this function records the attachment barriers -
 *   from the layout the set states to the layout rendering requires - and then opens the instance. A barrier between two
 *   IDENTICAL states is part of the batch on purpose: it is the execution dependency between this pass and the previous
 *   one into the same image, which is the half Vulkan does not insert for dynamic rendering.
 * - ONE DEPENDENCY, NOT ONE PER ATTACHMENT. Every attachment barrier this pass needs is stated in ONE `VkDependencyInfo`
 *   (see `recordBarriers`): a seven-attachment multisampled set states one command, not seven. Vulkan's model is that a
 *   dependency orders two sets of accesses; splitting it per image says the same thing more expensively, and the count
 *   is a number this backend publishes (see `RecordingTally`).
 * - CALLER OWNS THE BOOKKEEPING. The `from` values a role carries are the facts the owner tracks, one per image (see
 *   `TargetObjects` and `WindowTarget`); after this call every attachment is in the rendering layout, so the owner
 *   stores exactly that.
 * - DRAWS FOLLOW, then @ref endRendering. Nothing may be recorded between the barrier set and the draws that needs the
 *   old layout.
 * - NO ALLOCATION (`G10`). The colour attachment info array and the barrier batch are stack values of
 *   `kMaxColorAttachments`: the specification caps `maxColorAttachments` at 8, and a shape asking for more is refused
 *   where it is built (`vk.target.too_many_colors`), so the bound is a fact the recorder may rely on instead of a
 *   silent clamp.
 *
 * @param table       The device's entry points (volk table).
 * @param commands    The command buffer being recorded (outside any rendering instance).
 * @param attachments The attachment set, as roles (borrowed for the call; a role's spans are its own images' records).
 * @param load        The pass' load policy and clear values.
 * @param tally       Receives what the entry's dependency cost (see `RecordingTally`), or null.
 */
void beginRendering(const VolkDeviceTable&       table,
                    VkCommandBuffer              commands,
                    const PassAttachmentSet&     attachments,
                    const PassAttachmentOps& load,
                    RecordingTally*              tally = nullptr);

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
