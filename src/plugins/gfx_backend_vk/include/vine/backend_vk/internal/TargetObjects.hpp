#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

#include <vine/backend_vk/internal/DynamicRendering.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/TargetPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The render-pass key every target of this backend states: one attachment set, no structure variants.
 *
 * @details
 * - This backend's passes each render into ONE attachment set in one instance, so every pair of its targets that agrees
 *   on formats/depth/samples is compatible - and one non-zero class states that. 0 is RenderPassKey's "not stated",
 *   which a plan that never learned the shape keeps; the two must not compare equal.
 * - THE KEY SURVIVES DYNAMIC RENDERING. It is a PIPELINE cache key fact (the attachment set a pipeline was built for),
 *   not a `VkRenderPass` handle: since Vulkan 1.3 states the attachments on the command buffer, the set is named by
 *   the compatibility the key belongs to (see `ContentCaches` and `DynamicRendering`).
 */
inline constexpr vn::graphics::RenderPassKey kSingleSubpassRenderPass{ 1U };

/**
 * @brief One off-screen target's Vulkan objects: the images a pass draws into and the layouts they are in.
 *
 * @details
 * - WHAT IT OWNS AND WHAT IT DOES NOT. It owns the images, their memory and their views. It does NOT own the
 *   `RenderTarget` it was built for (the host owns that) and it does not decide WHEN to rebuild: the device asks
 *   `planTarget` and calls @ref buildTarget / @ref resizeTarget accordingly.
 * - NO RENDER PASS, NO FRAMEBUFFER. Dynamic rendering (Vulkan 1.3) states the attachments on the command buffer, so
 *   the set is exactly its images plus the layout each one holds (see @ref beginPass, and `DynamicRendering`). A load
 *   op is no longer a structure variant at all: it is one field of the pass' `VkRenderingAttachmentInfo`, which is
 *   what lets a HUD pass keep the picture and clear its own depth without a fourth render pass.
 * - SHAPE VS EXTENT. The ATTACHMENT SET (colour formats, depth format, sample count) decides what a pipeline is
 *   compatible with; the EXTENT decides only the images. That split is what lets a resize keep every pipeline
 *   compiled for the shape alive (`TargetAction::ResizeInPlace`, see `planTarget`).
 * - MRT. Every colour attachment is built, cleared and read back on its own; the rendering info references them in
 *   order, which is the order a fragment's output locations write.
 * - MULTISAMPLING IS A SHAPE FACT, AND IT ADDS A SECOND IMAGE PER ATTACHMENT. A target whose shape says `samples > 1`
 *   is drawn into MULTISAMPLED colour images and RESOLVES them into single-sample ones when the rendering instance
 *   ends. `colors` / `color_views` are then the RESOLVE images - the picture this device hands out, which is what a
 *   full-screen draw samples and what `readColorBuffer` copies - while `multisample_colors` /
 *   `multisample_color_views` are the sample buffers, which nothing samples and nothing reads back. The DEPTH is
 *   resolved the same way into `depth_resolve` / `depth_resolve_view` (`sample0`, so multisampling cannot change the
 *   depth a later pass tests against): `depthPicture()` / `depthPictureView()` name whichever depth image is the
 *   PICTURE - the resolve one on a multisampled target, the attachment itself otherwise - and that is what a shader
 *   samples and what a borrower attaches.
 * - DEPTH-ONLY IS A SHAPE. A target with a depth attachment and no colour attachment is the engine's SHADOW MAP and
 *   builds like any other (the rendering info references no colour attachment, and a fragment's outputs are
 *   discarded); a target with neither attachment is not a target.
 * - MEMORY IS A SLICE OF THE DEVICE'S POOL. Each image asks the device's MemoryAllocator for a range (see MemorySlice)
 *   and binds it at that offset, so a scene with hundreds of targets reaches the driver a handful of times instead of
 *   once per image: `maxMemoryAllocationCount` is a per-device limit (the spec's floor is 4096) and the failure it
 *   produces reads like "out of memory" while the device may be nearly empty. This record owns the SLICES, not the
 *   memory - `destroyTarget` gives them back to the pool.
 * - WHO MAY TOUCH IT, AND UNDER WHAT (audit A3-01). `buildTarget` / `resizeTarget` / `destroyTarget` all run inside
 *   `TargetTable::ensure` (or the session-end drain) with the table's map lock held, so the SHAPE, the images and the
 *   views are written under that one lock - and a reader that wants them takes the same lock (`TargetTable::peek`)
 *   instead of borrowing this record for a field read: the record outlives its map entry (`release` parks it, the park
 *   destroys it), so a borrow has no synchronisation to point at.
 * - THE THREE LAYOUT BITS ARE THE FRAME BODY'S. @ref fresh, @ref color_sampled and @ref depth_sampled answer where the
 *   images are, and the recording path writes them where no lock may be taken (`C3` / `C5`: @ref beginPass,
 *   `Device::resolveShadow`, the full-screen source transition) - so the FRAME GATE serialises them, not the map lock:
 *   one frame records at a time, and the claim hand-off (writer to next claimer) plus the gate (writer to a host verb;
 *   the readback verbs hold it exclusively) are what make their values current. @ref fresh is atomic as well, because
 *   the pipeline planner reads it outside the gate as a hint.
 * - IT IS A VALUE-LIKE RECORD with explicit build/destroy calls, because Vulkan has no destructors: `destroyTarget()`
 *   is idempotent so an error path may call it unconditionally.
 */
struct TargetObjects {
    /// The extent and the shape (engine formats + device formats) these objects were built for.
    vn::graphics::TargetDesc desc{};

    /// THE PICTURE, one per colour attachment: the single-sample image a later pass samples, a readback copies and a
    /// single-sampled pass draws into - and, on a multisampled target, the RESOLVE image the rendering instance writes
    /// when it ends. Nothing here is ever drawn into directly while `isMultisampled()` is true.
    std::vector<VkImage>        colors{};
    std::vector<MemorySlice>    color_memory{};
    std::vector<VkImageView>    color_views{};

    /// THE SAMPLE BUFFERS, one per colour attachment, EMPTY on a single-sampled target: the images a multisampled
    /// pass draws into, whose content the rendering instance resolves into `colors`. They carry no sampled or transfer
    /// usage because nothing reads them; the resolve is the only way out of them.
    std::vector<VkImage>        multisample_colors{};
    std::vector<MemorySlice>    multisample_color_memory{};
    std::vector<VkImageView>    multisample_color_views{};

    /// The depth attachment, created with the target's sample count and NEVER resolved (see the class note).
    VkImage        depth{ VK_NULL_HANDLE };
    MemorySlice    depth_memory{};
    VkImageView    depth_view{ VK_NULL_HANDLE };

    /// The single-sample depth PICTURE of a multisampled target (see the class note): the rendering instance resolves
    /// the depth into it, a shader samples it and a borrower attaches it. Empty on a single-sampled target, where
    /// `depth` already is the picture.
    VkImage        depth_resolve{ VK_NULL_HANDLE };
    MemorySlice    depth_resolve_memory{};
    VkImageView    depth_resolve_view{ VK_NULL_HANDLE };

    /// True when the depth attachment is ANOTHER target's image (see RenderTarget::shareDepth): `depth`,
    /// `depth_memory` and `depth_view` stay null and the pass attaches the lender's picture instead. The shape still
    /// states a depth format - the LENDER's, which is what a pipeline is built with - and every pass LOADs that depth,
    /// because a pass may not clear an image an earlier pass wrote.
    bool depth_borrowed{ false };
    /// The lender's picture as it was when this one was built: the IMAGE a barrier names and the VIEW the pass attaches.
    /// NOT owned here - the lender's record owns both, and this target is rebuilt whenever that picture moves (see
    /// TargetTable::ensure), so neither value can outlive what it names. They are one pair because they are one image:
    /// a barrier over this image is what orders the LENDER's pass before this one.
    VkImage     borrowed_depth_image{ VK_NULL_HANDLE };
    VkImageView borrowed_depth_view{ VK_NULL_HANDLE };

    /// True until a pass has drawn into this attachment set: fresh images are UNDEFINED, so a pass that asked to LOAD
    /// an attachment is given CLEAR instead (loading an image nobody wrote is meaningless and invalid).
    /// ATOMIC because the frame body is its only writer and takes no lock (see the class note); relaxed is enough -
    /// every decision a reader outside the gate takes from it is a hint that the gate re-checks.
    std::atomic<bool> fresh{ true };
    /// True after a full-screen draw sampled this target: its colour images (the PICTURE - the resolve images on a
    /// multisampled target) are then in `SHADER_READ_ONLY_OPTIMAL`, and the next pass moves them back before it writes
    /// them (or before a readback reads them).
    bool color_sampled{ false };
    /// True after a pass sampled this target as a SHADOW MAP: its depth PICTURE (see `depthPicture`) is then in
    /// `SHADER_READ_ONLY_OPTIMAL`, and the next pass moves it back before it writes the depth (the same rule
    /// `color_sampled` follows, on the other attachment).
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
        return index < desc.shape.device_color_formats.size()
                   ? static_cast<VkFormat>(desc.shape.device_color_formats[index].code)
                   : VK_FORMAT_UNDEFINED;
    }

    /** @brief Answers whether the target owns a depth attachment. */
    [[nodiscard]] bool hasDepth() const noexcept
    {
        return desc.shape.device_depth_format.code != vn::graphics::DeviceFormat::kUndefined;
    }

    /** @brief Gets the depth attachment's Vulkan format (`VK_FORMAT_UNDEFINED` when there is none). */
    [[nodiscard]] VkFormat depthFormat() const noexcept
    {
        return static_cast<VkFormat>(desc.shape.device_depth_format.code);
    }

    /** @brief Answers whether this target's SHAPE resolves its depth (multisampled AND depth-attached). */
    [[nodiscard]] bool resolvesDepth() const noexcept
    {
        return isMultisampled() && hasDepth();
    }

    /** @brief Answers whether this target owns a resolved (single-sample) copy of its multisampled depth. */
    [[nodiscard]] bool hasDepthResolve() const noexcept
    {
        return depth_resolve_view != VK_NULL_HANDLE;
    }

    /** @brief Answers whether this target's depth attachment is ANOTHER target's image (see RenderTarget::shareDepth). */
    [[nodiscard]] bool borrowsDepth() const noexcept
    {
        return depth_borrowed;
    }

    /** @brief Gets the DEPTH VIEW a pass ATTACHES: the owned one, or the lender's when this target borrows. */
    [[nodiscard]] VkImageView depthAttachmentView() const noexcept
    {
        return depth_borrowed ? borrowed_depth_view : depth_view;
    }

    /** @brief Gets the DEPTH IMAGE a pass ATTACHES: the owned one, or the lender's picture when this target borrows. */
    [[nodiscard]] VkImage depthAttachmentImage() const noexcept
    {
        return depth_borrowed ? borrowed_depth_image : depth;
    }

    /**
     * @brief Gets the depth image a READER sees: the resolve image on a multisampled target, the attachment else.
     *
     * @note
     * - A BORROWING TARGET OWNS NO PICTURE and answers `VK_NULL_HANDLE`: the image belongs to the lender, which is
     *   also the target a readback reads (see RenderBackend::readDepthBuffer) and the one a shader samples.
     */
    [[nodiscard]] VkImage depthPicture() const noexcept
    {
        return depth_borrowed ? VK_NULL_HANDLE : (hasDepthResolve() ? depth_resolve : depth);
    }

    /** @brief Gets the view of @ref depthPicture (what a shader samples and what a LENDER hands to a borrower). */
    [[nodiscard]] VkImageView depthPictureView() const noexcept
    {
        return depth_borrowed ? VK_NULL_HANDLE : (hasDepthResolve() ? depth_resolve_view : depth_view);
    }

    /**
     * @brief Records this set's attachment transitions and opens the rendering instance a pass draws into.
     *
     * @details
     * - THE 1.3 REPLACEMENT for the render pass variant this record used to own: the attachments and their load ops are
     *   stated on the command buffer (see `DynamicRendering::beginRendering`), and the layouts it transitions FROM are
     *   this record's own bookkeeping - `fresh` (nothing has been written yet), `color_sampled` / `depth_sampled` (a
     *   reader sampled the picture last time, so it is in `SHADER_READ_ONLY_OPTIMAL`), the attachment layout otherwise.
     * - IT UPDATES THAT BOOKKEEPING: after the call every attachment is in the rendering layout and has been written,
     *   so `fresh` becomes false and both sampled bits clear. The pass is expected to end with @ref endPass.
     * - TWO DEPTH IMAGES, TWO RECORDS. The depth attachment and the picture it resolves into are stated separately (see
     *   `PassAttachments`): `depth_sampled` moves the PICTURE to `SHADER_READ_ONLY_OPTIMAL` while the sample buffer stays
     *   an attachment, because a multisampled image may not be sampled at all. Stating one layout for both is an illegal
     *   barrier on whichever of them never had it.
     * - A BORROWED DEPTH IS NEVER "FRESH" HERE: the image holds the LENDER's pass' depth, so it starts from the depth
     *   attachment layout whatever this set's own `fresh` says - and a pass may not clear it (see
     *   RenderTarget::shareDepth).
     *
     * @param table    The device's entry points (volk table).
     * @param commands The command buffer being recorded (outside any rendering instance).
     * @param load     The pass' load policy - one answer per attachment (see ClearPolicy).
     */
    void beginPass(const VolkDeviceTable& table, VkCommandBuffer commands, const PassLoad& load);

    /**
     * @brief Closes the rendering instance @ref beginPass opened (see `DynamicRendering::endRendering`).
     *
     * @param table    The device's entry points (volk table).
     * @param commands The command buffer the pass was recorded into.
     */
    void endPass(const VolkDeviceTable& table, VkCommandBuffer commands) const noexcept;

    /** @brief Gets the pipeline compatibility of this target (the value its description states). */
    [[nodiscard]] vn::graphics::RenderPassCompatibility compatibility() const noexcept
    {
        return desc.shape;
    }

    /** @brief Answers whether every object a pass needs exists for @ref desc. */
    [[nodiscard]] bool built() const noexcept;

    /** @brief Answers whether these objects were built for exactly @p wanted (extent and shape). */
    [[nodiscard]] bool matches(const vn::graphics::TargetDesc& wanted) const noexcept
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
 * @brief Creates everything `images.desc` describes: the images and their views.
 *
 * @details
 * - `desc.width` / `height` and the shape must be set first; they are the description the objects are built for.
 * - A shape whose `samples` is above 1 builds the multisampled attachments AND their single-sample resolve images (see
 *   the class note): the rendering instance resolves the colour into `colors`, and states the resolve on each sample
 *   buffer's attachment info.
 * - On failure the function destroys whatever it made and answers false, so the caller never holds a half-built target
 *   (the same rule the device's `initialize` follows).
 *
 * @param table     The device's entry points (volk table).
 * @param device    The logical device.
 * @param allocator The device's memory pool (every attachment asks it for a slice).
 * @param images    The record to fill.
 * @return true when every object exists.
 */
[[nodiscard]] bool buildTarget(const VolkDeviceTable& table, VkDevice device, MemoryAllocator& allocator, TargetObjects& images);

/**
 * @brief Rebuilds the images and views for a new EXTENT, keeping the shape.
 *
 * @details
 * - This is `TargetAction::ResizeInPlace`: the shape did not change, so every pipeline compiled for it stays valid;
 *   only what the extent decides is replaced.
 * - On failure the target is left with no attachments (and no stale objects), so the caller reports and retries later.
 *
 * @param table     The device's entry points (volk table).
 * @param device    The logical device.
 * @param allocator The device's memory pool.
 * @param images    The record whose `desc` already states the new extent and the unchanged shape.
 * @return true when the new attachments exist.
 */
[[nodiscard]] bool resizeTarget(const VolkDeviceTable& table, VkDevice device, MemoryAllocator& allocator, TargetObjects& images);

/**
 * @brief Destroys every object `images` holds (idempotent), including the images and the description.
 *
 * @param table     The device's entry points (volk table).
 * @param device    The logical device.
 * @param allocator The device's memory pool (the slices are given back to it).
 * @param images    The record to clear.
 */
void destroyTarget(const VolkDeviceTable& table, VkDevice device, MemoryAllocator& allocator, TargetObjects& images) noexcept;

VN_BACKEND_VK_NS_END
