#pragma once

#include <cstdint>
#include <vector>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/graphics/RenderTarget.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief One off-screen target's Vulkan objects: the images a pass clears, the render pass it runs in, and the
 *        framebuffer that binds them.
 *
 * @details
 * - WHAT IT OWNS AND WHAT IT DOES NOT. It owns the images, their memory, their views, the render pass and the
 *   framebuffer. It does NOT own the `RenderTarget` it was built for (the host owns that) and it does not decide WHEN
 *   to rebuild: `matches()` answers whether it still fits the description, and the caller rebuilds when it does not
 *   (M3 turns that into `planTarget`'s answer; M1 compares directly).
 * - ONE SHAPE, EVERY PASS. The render pass is built once with `CLEAR`/`STORE` and `UNDEFINED` as the initial layout,
 *   which is M1's contract: a pass that draws into a target must state a clear policy (see the device's note). A pass
 *   that wants to LOAD what an earlier pass left needs a second render-pass variant, and that is M3's `LoadOpVariantKey`.
 * - MEMORY IS ONE ALLOCATION PER IMAGE. A suballocator is the right answer for a scene with hundreds of targets and the
 *   wrong one for the first clear; `vkAllocateMemory` per image keeps the failure modes visible (every call answers a
 *   result) and leaves the allocator as its own later step.
 * - IT IS A VALUE-LIKE RECORD with explicit build/destroy calls, because Vulkan has no destructors: `destroy()` is
 *   idempotent so an error path may call it unconditionally.
 */
struct TargetImages {
    int           width{ 0 };
    int           height{ 0 };
    VkFormat      color_format{ VK_FORMAT_UNDEFINED };
    VkFormat      depth_format{ VK_FORMAT_UNDEFINED };

    std::vector<VkImage>        colors{};
    std::vector<VkDeviceMemory> color_memories{};
    std::vector<VkImageView>    color_views{};

    VkImage         depth{ VK_NULL_HANDLE };
    VkDeviceMemory  depth_memory{ VK_NULL_HANDLE };
    VkImageView     depth_view{ VK_NULL_HANDLE };

    VkRenderPass  render_pass{ VK_NULL_HANDLE };
    VkFramebuffer framebuffer{ VK_NULL_HANDLE };

    /** @brief Answers whether these objects were built for exactly this description. */
    [[nodiscard]] bool matches(int w, int h, VkFormat color, VkFormat depth_format) const noexcept;

    /** @brief Gets whether every object a pass needs exists. */
    [[nodiscard]] bool built() const noexcept;
};

/**
 * @brief Gets the Vulkan format a target's colour attachment asks for.
 *
 * @details
 * - `RGBA8` maps to the linear `R8G8B8A8_UNORM` spelling: the SDL's enum is a PROJECTION (it covers the sRGB spelling
 *   too, see RenderTarget::ColorFormat), and an off-screen target that a shader samples and a probe reads is linear.
 *
 * @param format The target's colour format.
 * @return The Vulkan format.
 */
[[nodiscard]] VkFormat colorFormatOf(vn::graphics::RenderTarget::ColorFormat format) noexcept;

/**
 * @brief Gets the Vulkan format a target's depth attachment asks for (0/`UNDEFINED` for "no depth").
 *
 * @param format The target's depth format.
 * @return The Vulkan format.
 */
[[nodiscard]] VkFormat depthFormatOf(vn::graphics::RenderTarget::DepthFormat format) noexcept;

/**
 * @brief Creates everything `images` describes.
 *
 * @details
 * - `images.width` / `height` / `color_format` / `depth_format` must be set first; they are the description the objects
 *   are built for. `VK_FORMAT_UNDEFINED` as the depth format means "colour only".
 * - On failure the function destroys whatever it made and answers false, so the caller never holds a half-built target
 *   (the same rule the device's `initialize` follows).
 *
 * @param table          The device's entry points (volk table).
 * @param device         The logical device.
 * @param memory         The device's memory properties (for the memory type search).
 * @param images         The record to fill.
 * @return true when every object exists.
 */
[[nodiscard]] bool buildTarget(const VolkDeviceTable&           table,
                               VkDevice                         device,
                               const VkPhysicalDeviceMemoryProperties& memory,
                               TargetImages&                    images);

/**
 * @brief Destroys every object `images` holds (idempotent).
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param images The record to clear.
 */
void destroyTarget(const VolkDeviceTable& table, VkDevice device, TargetImages& images) noexcept;

VN_BACKEND_VK_NS_END
