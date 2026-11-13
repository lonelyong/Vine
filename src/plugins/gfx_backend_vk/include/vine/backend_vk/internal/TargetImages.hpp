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
 * - MULTISAMPLING IS A SHAPE FACT, AND IT ADDS A SECOND IMAGE PER ATTACHMENT. A target whose shape says `samples > 1`
 *   is drawn into MULTISAMPLED colour images and the render pass RESOLVES them into single-sample ones at the end of
 *   its subpass. `colors` / `color_views` are then the RESOLVE images - the picture this device hands out, which is
 *   what a screen draw samples and what `readColorBuffer` copies - while `multisample_colors` /
 *   `multisample_color_views` are the sample buffers, which nothing samples and nothing reads back. The DEPTH is
 *   multisampled with the same count and NOT resolved: nothing on this path samples or reads it back (a target whose
 *   depth is read is served single-sampled, see `Device::ensureTarget`), so a resolve there would be an image nobody
 *   asked for.
 * - DEPTH-ONLY IS A SHAPE. A target with a depth attachment and no colour attachment is the engine's SHADOW MAP and
 *   builds like any other (the render pass simply references no colour attachment, and a fragment's outputs are
 *   discarded); a target with neither attachment is not a target.
 * - MEMORY IS ONE ALLOCATION PER IMAGE. A suballocator is the right answer for a scene with hundreds of targets and the
 *   wrong one for the first clear; `vkAllocateMemory` per image keeps the failure modes visible (every call answers a
 *   result) and leaves the allocator as its own later step.
 * - IT IS A VALUE-LIKE RECORD with explicit build/destroy calls, because Vulkan has no destructors: `destroyTarget()`
 *   is idempotent so an error path may call it unconditionally.
 */
struct TargetImages {
    /// The extent and the shape (engine formats + device formats) these objects were built for.
    vn::graphics::backend::TargetDesc desc{};

    /// THE PICTURE, one per colour attachment: the single-sample image a later pass samples, a readback copies and a
    /// single-sampled pass draws into - and, on a multisampled target, the RESOLVE image the render pass writes at the
    /// end of its subpass. Nothing here is ever drawn into directly while `isMultisampled()` is true.
    std::vector<VkImage>        colors{};
    std::vector<VkDeviceMemory> color_memories{};
    std::vector<VkImageView>    color_views{};

    /// THE SAMPLE BUFFERS, one per colour attachment, EMPTY on a single-sampled target: the images a multisampled
    /// pass draws into, whose content the render pass resolves into `colors`. They carry no sampled or transfer usage
    /// because nothing reads them; the resolve is the only way out of them.
    std::vector<VkImage>        multisample_colors{};
    std::vector<VkDeviceMemory> multisample_color_memories{};
    std::vector<VkImageView>    multisample_color_views{};

    /// The depth attachment, created with the target's sample count and NEVER resolved (see the class note).
    VkImage        depth{ VK_NULL_HANDLE };
    VkDeviceMemory depth_memory{ VK_NULL_HANDLE };
    VkImageView    depth_view{ VK_NULL_HANDLE };

    /// The render pass whose depth attachment is CLEARed (the first pass into a fresh target).
    VkRenderPass render_pass{ VK_NULL_HANDLE };
    /// The variant whose depth attachment is LOADed (a pass that preserves the depth an earlier pass wrote); null for
    /// a colour-only target, where the two variants are the same thing.
    VkRenderPass render_pass_load_depth{ VK_NULL_HANDLE };
    /// The variant that LOADs every attachment, for a pass that announces no clear at all (a screen pass): colour and
    /// depth keep what earlier passes wrote. Only valid on a NON-fresh attachment set.
    VkRenderPass render_pass_load{ VK_NULL_HANDLE };
    /// Binds the attachments IN THE ORDER the render pass declares them: every sample buffer, then (on a multisampled
    /// target) every resolve image, then the depth view when present. Compatible with ALL render-pass variants.
    VkFramebuffer framebuffer{ VK_NULL_HANDLE };
    /// True until a pass has drawn into this attachment set: fresh images are UNDEFINED, so a pass that asked to LOAD
    /// the depth is given the CLEAR variant instead (loading an image nobody wrote is meaningless and invalid).
    bool fresh{ true };
    /// True after a screen draw sampled this target: its colour images (the PICTURE - the resolve images on a
    /// multisampled target) are then in `SHADER_READ_ONLY_OPTIMAL`, and the device transitions them back before the
    /// next pass writes them (or before a readback reads them).
    bool color_sampled{ false };
    /// True after a pass sampled this target as a SHADOW MAP: its depth image is then in `SHADER_READ_ONLY_OPTIMAL`,
    /// and the device transitions it back before the next pass writes the depth (the same rule `color_sampled`
    /// follows, on the other attachment).
    bool depth_sampled{ false };

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

    /** @brief Gets how many samples each pixel of this target is drawn with (always at least 1). */
    [[nodiscard]] std::uint32_t samples() const noexcept
    {
        return desc.shape.samples > 1U ? desc.shape.samples : 1U;
    }

    /** @brief Answers whether the attachments are multisampled, i.e. whether `colors` are RESOLVE images. */
    [[nodiscard]] bool isMultisampled() const noexcept
    {
        return samples() > 1U;
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

    /**
     * @brief Gets the ATTACHMENT INDEX the depth occupies in the render pass and in the framebuffer.
     *
     * @details
     * - The attachment array is [sample buffers..., resolve images..., depth], so the depth's index is the number of
     *   COLOUR slots - and a clear-value array (indexed by attachment, see `VkRenderPassBeginInfo`) has to agree with
     *   it, or a multisampled pass would clear the depth with the resolve's slot.
     * - The colour slots' own count is this index: `colorCount()` on a single-sampled target, twice that when the
     *   attachments are resolved.
     *
     * @return The depth attachment's index, or the colour slot count when the target has no depth.
     */
    [[nodiscard]] std::size_t depthAttachmentIndex() const noexcept
    {
        return colorCount() * (isMultisampled() ? 2U : 1U);
    }

    /** @brief Gets the render pass that serves a pass' load ops (see the three variants above). */
    [[nodiscard]] VkRenderPass renderPassFor(bool load_depth, bool load_color) const noexcept
    {
        if (load_color && render_pass_load != VK_NULL_HANDLE) {
            return render_pass_load;
        }
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
 * @brief Gets the aspect mask a barrier on a depth image of @p format must name.
 *
 * @details
 * - A COMBINED DEPTH+STENCIL FORMAT NEEDS BOTH ASPECTS in a barrier unless the device enables
 *   `separateDepthStencilLayouts`, even though nothing here ever writes stencil (VUID-VkImageMemoryBarrier-image-03320;
 *   the validation layer refuses a depth-only aspect on such an image). This backend's `D24` is Vulkan's
 *   `D24_UNORM_S8_UINT`, so the shadow map is exactly that case.
 * - A VIEW is not a barrier: it may name the depth aspect alone, which is what `createView` does.
 *
 * @param format The depth image's format.
 * @return `DEPTH|STENCIL` for a combined format, `DEPTH` otherwise.
 */
[[nodiscard]] VkImageAspectFlags depthAspectMask(VkFormat format) noexcept;

/**
 * @brief Creates everything `images.desc` describes: render passes, images, views and the framebuffer.
 *
 * @details
 * - `desc.width` / `height` and the shape must be set first; they are the description the objects are built for.
 * - A shape whose `samples` is above 1 builds the multisampled attachments AND their single-sample resolve images (see
 *   the class note): the render pass resolves the colour into `colors`, and the framebuffer lists both sets.
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
