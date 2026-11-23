#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/graphics/DrawState.hpp>

#include "ImageLayout.hpp"

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief THE ONE PLACE where a neutral "what is this image being used for" becomes a Vulkan layout, stage and access
 *        mask.
 *
 * @details
 * - WHY A TABLE. This backend's `ImageLayout` states what an image is FOR (a colour attachment, a depth
 *   attachment, something sampled, something presented) and Vulkan wants three separate spellings for the same
 *   fact - the layout, the stages that must wait, and the access mask that must be visible. Writing those three
 *   inline wherever a barrier is needed is how a backend ends up with two answers for one pass; one function per
 *   direction keeps the answer single.
 * - WHY THE INPUT IS THIS MODULE'S AND NOT THE SDK'S. The SDK states only what every family can answer
 *   (`LoadOp` / `StoreOp`, and what a pass declares); a LAYOUT is one family's spelling of "this image is being
 *   written as colour", so it lives here with the table that consumes it (backlog T15, review 8 §4.7/§4.8).
 *   A second API family replaces THIS TABLE and this input, not the seam.
 * - SYNCHRONISATION 2, NOT THE VULKAN-1.0 FORM. The floor is Vulkan 1.3, so a barrier is a
 *   `VkImageMemoryBarrier2` inside a `VkDependencyInfo` and is recorded by `vkCmdPipelineBarrier2`: the stage masks live
 *   ON the barrier (they are the two rows of this table), which is why no call site passes a stage pair of its own any
 *   more. `recordBarrier` is the one place that names the 1.3 command.
 * - THE TABLE IS PURE. No device, no allocation, no state: the table answers the same thing on a machine with no Vulkan
 *   loader, which is what makes it testable before anything can be drawn. `recordBarrier` takes the device table
 *   because a recording call needs one; it keeps no state of its own.
 *
 * @note Not thread-safe only in the sense that every function here is stateless; they may be called from any thread.
 */
struct ImageState {
    VkImageLayout         layout{ VK_IMAGE_LAYOUT_UNDEFINED };         // The layout that usage requires.
    VkPipelineStageFlags2 stages{ VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT }; // The stages a barrier must synchronise.
    VkAccessFlags2        access{ 0 };                                 // The memory a barrier must make visible.
};

/**
 * @brief Gets the Vulkan state that serves @p usage.
 *
 * @details
 * - `Undefined` is the "contents do not matter" state (a fresh image, or one whose contents a pass will overwrite), which
 *   is why it carries no access mask and only the top-of-pipe stage.
 * - `Present` is the only value whose layout belongs to the platform rather than to a queue operation, so it is the
 *   only one that names an extension-provided layout.
 * - AN ATTACHMENT ROW STATES BOTH HALVES OF ITS ACCESS, read and write: under dynamic rendering there is no subpass
 *   dependency to order a pass' LOAD (or its blend read) after the previous write, so a barrier that named only the
 *   write would leave the read unordered (see `DynamicRendering`).
 *
 * @param usage What the image is used for (see `ImageLayout`).
 * @return The layout, stages and access mask for @p usage.
 */
[[nodiscard]] ImageState stateOf(ImageLayout usage) noexcept;

/**
 * @brief Answers whether moving from @p from to @p to also TRANSITIONS the layout, i.e. whether the two usages differ.
 *
 * @details
 * - WHAT IT ANSWERS IS THE LAYOUT HALF, NEVER THE DEPENDENCY HALF. Two different usages need a barrier that changes the
 *   layout; two IDENTICAL usages may still need one, because something has to be ordered against an earlier access - and
 *   under dynamic rendering that is the common case: `DynamicRendering::beginRendering` records a barrier between two
 *   identical states on purpose, because it IS the execution dependency between two passes into one image. A caller that
 *   skips a barrier because this answered false has therefore ordered nothing.
 * - Same usage means the same layout, stage and access, which is what makes the LAYOUT half skippable at all - it is not
 *   a statement that the barrier has no consequences.
 *
 * @param from Usage the image is in now.
 * @param to   Usage the image is about to be in.
 * @return true when the two usages differ (the barrier also transitions the layout); false when they are identical.
 */
[[nodiscard]] bool requiresTransition(ImageLayout from, ImageLayout to) noexcept;

/**
 * @brief Builds the barrier that moves @p image from one usage to another.
 *
 * @details
 * - `oldLayout` / `newLayout` come from @p from and @p to, and the access masks from the same table, so a caller cannot
 *   pair a layout with the wrong access mask.
 * - Queue-family ownership is ignored (the backend records everything on one queue family).
 *
 * @param image  Image to transition (may be `VK_NULL_HANDLE`: the value is still well formed).
 * @param from   Usage the image is in now.
 * @param to     Usage the image is about to be in.
 * @param aspect Aspect mask (colour or depth).
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierFor(VkImage           image,
                                              ImageLayout from,
                                              ImageLayout to,
                                               VkImageAspectFlags aspect) noexcept;

/**
 * @brief Gets the state a copy READS from: `TRANSFER_SRC_OPTIMAL` with the transfer stage and read access.
 *
 * @details
 * - `ImageLayout` states what a PASS uses an image for; a readback is a device verb, not a pass, so this end of
 *   a barrier has no SDK spelling. It is kept here (next to the table) rather than in the SDK so the seam does not grow a
 *   Vulkan-shaped member for one backend's readback (C6).
 *
 * @return The transfer-source state.
 */
[[nodiscard]] ImageState transferReadState() noexcept;

/**
 * @brief Builds a barrier between two states, each of which may come from either table.
 *
 * @details
 * - THE TWO QUEUE-FAMILY INDICES ARE `VK_QUEUE_FAMILY_IGNORED`, which is a statement about this backend's model rather
 *   than an omission: every command it records goes on ONE family (the graphics queue that also presents), so there is
 *   no ownership to transfer - a swapchain image included, because the family that draws it is the family that presents
 *   it. An explicit release/acquire pair would describe a transfer that never happens; a second family would be the
 *   feature that makes this line change.
 * - The same `oldLayout` / `newLayout` + stage/access pair is used for every caller, so a caller cannot pair a layout
 *   with another row's stages or access mask (see `stateOf`).
 *
 * @param image  Image to transition.
 * @param from   State the image is in.
 * @param to     State the image is about to be in.
 * @param aspect Aspect mask (colour or depth).
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierBetween(VkImage            image,
                                                  const ImageState&  from,
                                                  const ImageState&  to,
                                                  VkImageAspectFlags aspect) noexcept;

/**
 * @brief Builds the barrier that moves a RANGE of @p image's subresources from one usage to another.
 *
 * @details
 * - A texture image is not one subresource: a mip chain and an array/cube face set are several, and a barrier that
 *   names only layer 0 leaves the rest in the layout they had - the copies that follow then write UNDEFINED
 *   subresources (measured in the app: a cube map's faces 1..5 at submit time). The aspect-only overload above is the
 *   single-subresource case, which is what every attachment barrier wants.
 *
 * @param image  Image to transition (may be `VK_NULL_HANDLE`: the value is still well formed).
 * @param from   Usage the image is in now.
 * @param to     Usage the image is about to be in.
 * @param range  The subresources the transition covers.
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierBetween(VkImage                        image,
                                                  const ImageState&              from,
                                                  const ImageState&              to,
                                                  const VkImageSubresourceRange& range) noexcept;

/**
 * @brief Records one image barrier on @p commands through synchronisation 2.
 *
 * @details
 * - THE ONE PLACE the 1.3 command is named: every barrier this backend records goes through here, so there is no
 *   second spelling of "submit a dependency" to drift. Nothing else is set - the barrier already carries both stage
 *   masks and both access masks (see @ref ImageState).
 * - A barrier that names no image is still legal to record (see @ref barrierBetween); the caller decides whether it is
 *   worth recording at all - and a barrier between two IDENTICAL states is worth recording whenever something has to be
 *   ordered (see @ref requiresTransition).
 *
 * @param table    The device table to record through.
 * @param commands The command buffer being recorded.
 * @param barrier  The barrier to record.
 */
void recordBarrier(const VolkDeviceTable& table, VkCommandBuffer commands, const VkImageMemoryBarrier2& barrier) noexcept;

/**
 * @brief Gets the attachment load operation for @p op.
 *
 * @param op The pass' neutral load operation (see ClearPlan).
 * @return The Vulkan attachment load operation.
 */
[[nodiscard]] VkAttachmentLoadOp loadOpOf(vn::graphics::LoadOp op) noexcept;

/**
 * @brief Gets the attachment store operation for @p op.
 *
 * @param op The pass' neutral store operation (see ClearPlan).
 * @return The Vulkan attachment store operation.
 */
[[nodiscard]] VkAttachmentStoreOp storeOpOf(vn::graphics::StoreOp op) noexcept;

VN_BACKEND_VK_NS_END
