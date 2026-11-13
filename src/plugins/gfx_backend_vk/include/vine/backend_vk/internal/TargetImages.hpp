#pragma once

#include <cstdint>
#include <vector>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/backend/TargetPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief One off-screen target's Vulkan objects: the images a pass draws into, the render passes it can run in, and
 *        the framebuffer that binds them.
 *
 * @details
 * - WHAT IT OWNS AND WHAT IT DOES NOT. It owns the images, their memory, their views, TWO render-pass variants (depth
 *   CLEAR and depth LOAD - a load op is not part of render-pass compatibility, so one framebuffer serves both) and
 *   the framebuffer. It does NOT own the `RenderTarget` it was built for (the host owns that) and it does not decide
 *   WHEN to rebuild: the device asks `planTarget` and calls @ref buildTarget / @ref resizeTarget accordingly.
 * - SHAPE VS EXTENT. The ATTACHMENT SET (colour formats, depth format, sample count) decides what the render pass is
 *   and therefore what a pipeline is compatible with; the EXTENT decides only the images and the framebuffer. That
 *   split is what lets a resize keep the render pass - and every pipeline compiled against it - alive
 *   (`TargetAction::ResizeInPlace`, see `planTarget`).
 * - MRT. Every colour attachment is built, cleared and read back on its own; the render pass' subpass references them
 *   in order, which is the order a fragment's output locations write.
 * - MEMORY IS ONE ALLOCATION PER IMAGE. A suballocator is the right answer for a scene with hundreds of targets and the
 *   wrong one for the first clear; `vkAllocateMemory` per image keeps the failure modes visible (every call answers a
 *   result) and leaves the allocator as its own later step.
 * - IT IS A VALUE-LIKE RECORD with explicit build/destroy calls, because Vulkan has no destructors: `destroyTarget()`
 *   is idempotent so an error path may call it unconditionally.
 */
struct TargetImages {
    /// The extent and the shape (engine formats + device formats) these objects were built for.
    vn::graphics::backend::TargetDesc desc{};

    std::vector<VkImage>        colors{};
    std::vector<VkDeviceMemory> color_memories{};
    std::vector<VkImageView>    color_views{};

    VkImage        depth{ VK_NULL_HANDLE };
    VkDeviceMemory depth_memory{ VK_NULL_HANDLE };
    VkImageView    depth_view{ VK_NULL_HANDLE };

    /// The render pass whose depth attachment is CLEARed (the first pass into a fresh target).
    VkRenderPass render_pass{ VK_NULL_HANDLE };
    /// The variant whose depth attachment is LOADed (a pass that preserves the depth an earlier pass wrote); null for
    /// a colour-only target, where the two variants are the same thing.
    VkRenderPass render_pass_load_depth{ VK_NULL_HANDLE };
    /// Binds every colour view and, when present, the depth view; compatible with BOTH render-pass variants.
    VkFramebuffer framebuffer{ VK_NULL_HANDLE };
    /// True until a pass has drawn into this attachment set: fresh images are UNDEFINED, so a pass that asked to LOAD
    /// the depth is given the CLEAR variant instead (loading an image nobody wrote is meaningless and invalid).
    bool fresh{ true };

    /** @brief Gets the target width in pixels. */
    [[nodiscard]] int width() const noexcept
    {
        return desc.width;
    }

    /** @brief Gets the target height in pixels. */
    [[nodiscard]] int height() const noexcept
    {
        return desc.height;
    }

    /** @brief Gets how many colour attachments the target has. */
    [[nodiscard]] std::size_t colorCount() const noexcept
    {
        return desc.shape.device_color_formats.size();
    }

    /** @brief Gets one colour attachment's Vulkan format (`VK_FORMAT_UNDEFINED` for an index out of range). */
    [[nodiscard]] VkFormat colorFormat(std::size_t index) const noexcept
    {
        return index < desc.shape.device_color_formats.size() ? static_cast<VkFormat>(desc.shape.device_color_formats[index]) : VK_FORMAT_UNDEFINED;
    }

    /** @brief Answers whether the target owns a depth attachment. */
    [[nodiscard]] bool hasDepth() const noexcept
    {
        return desc.shape.device_depth_format != 0U;
    }

    /** @brief Gets the depth attachment's Vulkan format (`VK_FORMAT_UNDEFINED` when there is none). */
    [[nodiscard]] VkFormat depthFormat() const noexcept
    {
        return static_cast<VkFormat>(desc.shape.device_depth_format);
    }

    /** @brief Gets the render pass that serves a pass' depth load op. */
    [[nodiscard]] VkRenderPass renderPassFor(bool load_depth) const noexcept
    {
        return load_depth && render_pass_load_depth != VK_NULL_HANDLE ? render_pass_load_depth : render_pass;
    }

    /** @brief Gets the pipeline compatibility of this target's shape. */
    [[nodiscard]] vn::graphics::backend::RenderPassCompatibility compatibility() const noexcept
    {
        return desc.shape.compatibility();
    }

    /** @brief Answers whether every object a pass needs exists for @ref desc. */
    [[nodiscard]] bool built() const noexcept;

    /** @brief Answers whether these objects were built for exactly @p wanted (extent and shape). */
    [[nodiscard]] bool matches(const vn::graphics::backend::TargetDesc& wanted) const noexcept
    {
        return desc == wanted;
    }
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
 * @brief Creates everything `images.desc` describes: render passes, images, views and the framebuffer.
 *
 * @details
 * - `desc.width` / `height` and the shape must be set first; they are the description the objects are built for.
 * - On failure the function destroys whatever it made and answers false, so the caller never holds a half-built target
 *   (the same rule the device's `initialize` follows).
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param memory The device's memory properties (for the memory type search).
 * @param images The record to fill.
 * @return true when every object exists.
 */
[[nodiscard]] bool buildTarget(const VolkDeviceTable&                  table,
                               VkDevice                                device,
                               const VkPhysicalDeviceMemoryProperties& memory,
                               TargetImages&                           images);

/**
 * @brief Rebuilds the images, views and framebuffer for a new EXTENT, keeping the render passes.
 *
 * @details
 * - This is `TargetAction::ResizeInPlace`: the shape did not change, so the render passes (and every pipeline
 *   compiled against them) stay; only what the extent decides is replaced.
 * - On failure the target is left with no attachments (and no stale objects), so the caller reports and retries later.
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param memory The device's memory properties.
 * @param images The record whose `desc` already states the new extent and the unchanged shape.
 * @return true when the new attachments exist.
 */
[[nodiscard]] bool resizeTarget(const VolkDeviceTable&                  table,
                                VkDevice                                device,
                                const VkPhysicalDeviceMemoryProperties& memory,
                                TargetImages&                           images);

/**
 * @brief Destroys every object `images` holds (idempotent), including the render passes and the description.
 *
 * @param table  The device's entry points (volk table).
 * @param device The logical device.
 * @param images The record to clear.
 */
void destroyTarget(const VolkDeviceTable& table, VkDevice device, TargetImages& images) noexcept;

VN_BACKEND_VK_NS_END
