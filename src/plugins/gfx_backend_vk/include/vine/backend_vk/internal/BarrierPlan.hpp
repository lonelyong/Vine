#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/graphics/backend/Keys.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief THE ONE PLACE where a neutral "what is this image being used for" becomes a Vulkan layout, stage and access
 *        mask.
 *
 * @details
 * - WHY A TABLE. The SDK states what an image is FOR (`ImageLayout`: a colour attachment, a depth attachment, something
 *   sampled, something presented) and Vulkan wants three separate spellings for the same fact - the layout, the stages
 *   that must wait, and the access mask that must be visible. Writing those three inline wherever a barrier is needed
 *   is how a backend ends up with two answers for one pass; one function per direction keeps the answer single.
 * - IT IS THE SEAM'S VULKAN HALF, and deliberately no more: a second API family (D3D12 states, Metal stages) replaces
 *   THIS TABLE, not its input. That is why the input is the SDK enum and the output is Vulkan - and why nothing here
 *   invents a "generic barrier type" for the SDK (see
 *   `.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §9.3).
 * - LEGACY BARRIERS, NOT SYNCHRONIZATION2. The first render pass of this backend needs one transition and a clear; the
 *   Vulkan-1.0 form (`VkImageMemoryBarrier` + `vkCmdPipelineBarrier`) needs no device feature and no extension, so M1
 *   uses it and the table stays honest about what it emits.
 * - IT IS PURE. No device, no allocation, no state: the table answers the same thing on a machine with no Vulkan
 *   loader, which is what makes it testable before anything can be drawn.
 *
 * @note Not thread-safe only in the sense that every function here is stateless; they may be called from any thread.
 */
struct ImageState {
    VkImageLayout        layout{ VK_IMAGE_LAYOUT_UNDEFINED };            // The layout that usage requires.
    VkPipelineStageFlags stages{ VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };    // The stages a barrier must synchronise.
    VkAccessFlags        access{ 0 };                                    // The memory a barrier must make visible.
};

/**
 * @brief Gets the Vulkan state that serves @p usage.
 *
 * @details
 * - `Undefined` is the "contents do not matter" state (a fresh image, or one whose contents a pass will overwrite), which
 *   is why it carries no access mask and only the top-of-pipe stage.
 * - `Present` is the only value whose layout belongs to the platform rather than to a queue operation, so it is the
 *   only one that names an extension-provided layout.
 *
 * @param usage What the image is used for (see `vn::graphics::backend::ImageLayout`).
 * @return The layout, stages and access mask for @p usage.
 */
[[nodiscard]] ImageState stateOf(vn::graphics::backend::ImageLayout usage) noexcept;

/**
 * @brief Answers whether a barrier is needed to go from @p from to @p to.
 *
 * @details
 * - Same usage means same layout, stage and access - a barrier would be a no-op that costs a pipeline flush point.
 *
 * @param from Usage the image is in now.
 * @param to   Usage the image is about to be in.
 * @return true when a barrier must be recorded; false when the two usages are identical.
 */
[[nodiscard]] bool requiresTransition(vn::graphics::backend::ImageLayout from, vn::graphics::backend::ImageLayout to) noexcept;

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
 * @return The barrier, ready to be recorded.
 */
[[nodiscard]] VkImageMemoryBarrier barrierFor(VkImage           image,
                                              vn::graphics::backend::ImageLayout from,
                                              vn::graphics::backend::ImageLayout to,
                                              VkImageAspectFlags aspect) noexcept;

/**
 * @brief Gets the state a copy READS from: `TRANSFER_SRC_OPTIMAL` with the transfer stage and read access.
 *
 * @details
 * - The SDK's `ImageLayout` states what a PASS uses an image for; a readback is a device verb, not a pass, so this end of
 *   a barrier has no SDK spelling. It is kept here (next to the table) rather than in the SDK so the seam does not grow a
 *   Vulkan-shaped member for one backend's readback (C6).
 *
 * @return The transfer-source state.
 */
[[nodiscard]] ImageState transferReadState() noexcept;

/**
 * @brief Builds a barrier between two states, each of which may come from either table.
 *
 * @param image  Image to transition.
 * @param from   State the image is in.
 * @param to     State the image is about to be in.
 * @param aspect Aspect mask (colour or depth).
 * @return The barrier, ready to be recorded.
 */
[[nodiscard]] VkImageMemoryBarrier barrierBetween(VkImage            image,
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
 * @return The barrier, ready to be recorded.
 */
[[nodiscard]] VkImageMemoryBarrier barrierBetween(VkImage                        image,
                                                  const ImageState&              from,
                                                  const ImageState&              to,
                                                  const VkImageSubresourceRange& range) noexcept;

/**
 * @brief Gets the attachment load operation for @p op.
 *
 * @param op The pass' neutral load operation (see ClearPlan).
 * @return The Vulkan attachment load operation.
 */
[[nodiscard]] VkAttachmentLoadOp loadOpOf(vn::graphics::backend::LoadOp op) noexcept;

/**
 * @brief Gets the attachment store operation for @p op.
 *
 * @param op The pass' neutral store operation (see ClearPlan).
 * @return The Vulkan attachment store operation.
 */
[[nodiscard]] VkAttachmentStoreOp storeOpOf(vn::graphics::backend::StoreOp op) noexcept;

VN_BACKEND_VK_NS_END
