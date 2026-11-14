#pragma once

#include <vine/backend_vk/internal/Handles.hpp>

namespace vn::graphics
{
class RenderDevice;
} // namespace vn::graphics

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief Answers the Vulkan handles behind a `vk` device, or an empty `Handles` for anything else.
 *
 * @details
 * - THE ONLY PLACE VULKAN MAY LEAVE THIS PLUGIN, and the shape the boundary document fixes for it
 *   (`.ai/research/graphics-2026-11-13-vk-exposure-boundary.md` §4.5): a VALUE whose name says which backend answered
 *   it, never a verb on `RenderDevice`, and never a member of either SDK face.
 * - THE SUPPORT HALF, NOT THE SHELL: a test or a tool includes this header, the plugin's frame path does not - it
 *   reaches the same handles through `internal/Device.hpp` - and this header is not installed. The value itself is
 *   declared in `internal/Handles.hpp`, so the dependency points from this door to the internals and never back.
 * - WHY IT IS A FREE FUNCTION: the SDK's device verbs are what every backend must answer, and no other backend can
 *   answer a `VkDevice` - so this is named after the backend that can, takes the neutral device, and leaves
 *   `RenderDevice` unchanged (see the boundary document §4.5 and its rejected option D1).
 * - THE ANSWER IS TYPED, NOT GUESSED. Only this plugin's `Device` carries these handles, so the question is a
 *   `dynamic_cast`: a device of another backend, or one that has not come up, answers the EMPTY struct rather than a
 *   handle that is not there.
 * - WHY THE NEUTRAL API IS NOT ENOUGH (the one sentence a caller owes this door, §5.2): a validation experiment, a
 *   RenderDoc capture or a hand-written Vulkan probe needs the real objects, and no SDK value names them.
 *
 * @param device Device to ask; a `vk` device answers its own handles, any other device answers `{}`.
 * @return The handles, all `VK_NULL_HANDLE` (family 0) before `initialize()` and after `shutdown()`.
 */
[[nodiscard]] Handles vkHandles(const vn::graphics::RenderDevice& device) noexcept;

VN_BACKEND_VK_NS_END
