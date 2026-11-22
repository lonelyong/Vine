#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <volk.h>

#include <vine/graphics/MaterialValues.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/backend_vk/internal/ContentAbi.hpp>
#include <vine/backend_vk/internal/ContentRefusal.hpp>
#include <vine/backend_vk/internal/ProgramVariant.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief WHAT ONE FRAME RESOLVED: one `DrawPlan` per draw packet, and the frame-level texture facts every call binds.
 *
 * @details
 * - THE SAME NAME AND SHAPE AS THE vsg BACKEND'S `FrameHead` (`G6`; ADR-2026-11-16-redesign-seam-and-vocabulary).
 *   The two types are deliberately NOT shared: a common `DrawPlan` could only erase them (`G7`), and what the two
 *   backends really agree on is the vocabulary - "the head resolves, the recording reads" - not the representation.
 * - A HEAD IS A VALUE, AND IT OWNS WHAT IT NAMES (`G4` / `G10`). Everything here was true when the caches were asked
 *   (the buffers, the pipeline, the state, the slots); a release or a replacement that lands while the frame records
 *   cannot change what this frame already resolved, and the objects the values name are parked until the device's
 *   watermark (see `ContentCaches::releaseLater`), so they stay valid for the frame that copied them.
 * - WHAT IS FRAME-LEVEL AND WHAT IS PER CALL. The 1x1 white stand-in and the two samplers are the same for every call
 *   of one frame, so they live HERE (resolved once at the head); the slots a call binds are its own, so they live in
 *   its `DrawPlan`. Recording reads the head and the plan, and asks no cache (`C3`).
 * - THE CONTAINERS ARE REUSED ACROSS FRAMES. `beginFrame` / `beginPass` clear the slots and keep their capacity, so a
 *   steady frame resolves without growing the vectors - the vsg head's fixed arrays make the same promise with
 *   another spelling (its file note states the rule for that side).
 * - WHAT IT IS NOT. It is not a cache (see `ContentCaches`), and it is not the frame's scratch (see `FrameScratch`):
 *   a head lives for one frame, belongs to one frame driver, and is discarded with it.
 *
 * @see `ContentCaches::planDraw` (what fills a plan), `ContentDraw` (the recorder that reads it), and the vsg
 *      backend's `FrameHead` (the other backend's spelling of the same concept).
 */
class FrameHead final {
  public:
    /// @brief One texture slot as the head resolved it: the view and the sampler it is read with.
    struct SlotBinding {
        VkImageView view{ VK_NULL_HANDLE }; ///< The uploaded image's view, or the 1x1 stand-in.
        VkSampler   sampler{ VK_NULL_HANDLE };
        bool        present{ false }; ///< Whether the material really names this slot (the shading's bit).
    };

    /**
     * @brief ONE DRAW PACKET'S RESOLVED VALUE: everything recording it reads, and nothing recording it would ask.
     *
     * @details
     * - `pipeline == VK_NULL_HANDLE` means "this call cannot be recorded": the head already stated why in the drawing
     *   call's `ContentRefusal`, and the recorder skips exactly this plan.
     * - THE ONE PAYLOAD THAT IS NOT SHARED is the content call's: material, variant and streams describe a
     *   `DrawItem`, and a full-screen packet has none of them. The pipeline and the resolved state are what a screen
     *   call uses beside its slots, which is why they are the common half of this value.
     * - THE SLOTS ARE RESOLVED BEFORE THE RENDER PASS BEGINS, because a texture upload records a buffer-to-image copy
     *   and an image barrier, neither of which is legal inside a render pass instance (the same rule
     *   `ContentCaches::preparePass` states for the warm-up path). The copies land on the frame's own command buffer,
     *   ahead of every pass.
     */
    struct DrawPlan {
        VkPipeline                         pipeline{ VK_NULL_HANDLE };
        vn::graphics::ResolvedCommandState resolved{};

        /// The content call's payload (empty for a full-screen packet).
        vn::graphics::MaterialDescriptionPtr material{}; ///< The material it is shaded with, or null (defaults).
        ProgramVariant                       variant{};  ///< What the program was compiled under.
        std::vector<VkBuffer>                vertex_streams{}; ///< The geometry's non-empty channels, in binding order.
        std::uint32_t                        vertex_count{ 0 };
        VkBuffer                             instance_buffer{ VK_NULL_HANDLE }; ///< The LAST binding, or null.
        VkBuffer                             index_buffer{ VK_NULL_HANDLE };    ///< This command's primitive, or null.
        std::uint32_t                        index_count{ 0 };

        /// The texture slots this call binds (all of them absent for a full-screen packet, which binds its source instead).
        std::array<SlotBinding, detail::kTextureSlots> slots{};

        /** @brief Releases what this plan names; the stream vector keeps its capacity. */
        void clear() noexcept
        {
            pipeline        = VK_NULL_HANDLE;
            resolved        = {};
            material        = {};
            variant         = {};
            vertex_count    = 0;
            instance_buffer = VK_NULL_HANDLE;
            index_buffer    = VK_NULL_HANDLE;
            index_count     = 0;
            slots           = {};
            vertex_streams.clear();
        }
    };

    /// @brief ONE DRAW PACKET of the frame: its plans (one per command; a full-screen packet has exactly one).
    struct Call {
        std::vector<DrawPlan> plans{};
        /// The head's first reason this call could not be resolved (reported where the draw is recorded).
        ContentRefusal refusal{};

        /** @brief Releases the plans and the refusal; the plans vector keeps its capacity. */
        void clear() noexcept
        {
            for (DrawPlan& plan : plans) {
                plan.clear();
            }
            plans.clear();
            refusal.clear();
        }
    };

    /// @brief ONE PASS of the frame, in the order the frame states them.
    struct Pass {
        std::vector<Call> draws{};

        /** @brief Releases every draw packet; the vector keeps its capacity. */
        void clear() noexcept
        {
            for (Call& call : draws) {
                call.clear();
            }
            draws.clear();
        }
    };

  public:
    FrameHead() = default;
    FrameHead(const FrameHead&)            = delete;
    FrameHead& operator=(const FrameHead&) = delete;

  public:
    /**
     * @brief Opens a frame: forgets every pass and the frame-level texture facts; the storage is kept.
     *
     * @param reserve_passes How many passes the frame may have (a hint; the storage only ever grows).
     */
    void beginFrame(const std::size_t reserve_passes)
    {
        if (passes_.capacity() < reserve_passes) {
            passes_.reserve(reserve_passes);
            ++storage_grows_;
        }
        for (Pass& slot : passes_) {
            slot.clear();
        }
        used_            = 0;
        fallback_view    = VK_NULL_HANDLE;
        fallback_sampler = VK_NULL_HANDLE;
        shadow_sampler   = VK_NULL_HANDLE;
    }

    /**
     * @brief Gets the slot for the frame's next pass, sized to @p draw_count draw packets.
     *
     * @param draw_count How many draw packets the pass states (a hint; the storage only ever grows).
     * @return The slot to resolve into; valid until the next beginPass or beginFrame.
     */
    [[nodiscard]] Pass& beginPass(const std::size_t draw_count)
    {
        if (used_ == passes_.size()) {
            passes_.emplace_back();
            ++storage_grows_;
        }
        Pass& slot = passes_[used_];
        for (Call& call : slot.draws) {
            call.clear();
        }
        if (draw_count > slot.draws.capacity()) {
            ++storage_grows_;
        }
        slot.draws.resize(draw_count);
        ++used_;
        return slot;
    }

    /**
     * @brief Gets how many times the reusable storage had to GROW (a warm-up count, not a per-frame one).
     *
     * @details
     * - THE WITNESS FOR THE FILE NOTE'S REUSE PROMISE (`G11`): "a steady frame resolves without growing the
     *   vectors" is a claim about capacity, and this is the number that moves when a vector is rebuilt instead of
     *   cleared. A steady frame must leave it unchanged; the first frames may grow it freely.
     * - WHAT IT DOES NOT COUNT: the plan storage's other growth (`DrawPlan::vertex_streams`, the pipeline key's
     *   channel list) happens outside this class and is not counted here - the counter proves the PASS/CALL storage,
     *   which is the part this class owns.
     *
     * @return The cumulative growth count (0 before the first frame).
     */
    [[nodiscard]] std::uint64_t storageGrows() const noexcept
    {
        return storage_grows_;
    }

    /** @brief Gets the passes resolved since beginFrame, in the frame's order. */
    [[nodiscard]] std::span<const Pass> passes() const noexcept
    {
        return std::span<const Pass>(passes_.data(), used_);
    }

    /** @brief Forgets every pass and every frame-level fact (a teardown, or a frame that will not record). */
    void clear() noexcept
    {
        for (Pass& slot : passes_) {
            slot.clear();
        }
        used_            = 0;
        fallback_view    = VK_NULL_HANDLE;
        fallback_sampler = VK_NULL_HANDLE;
        shadow_sampler   = VK_NULL_HANDLE;
    }

  public:
    /// The 1x1 white stand-in an absent texture slot binds (see ContentCaches::resolveTextures); null when it could
    /// not be made, which is the one condition under which a call must not record.
    VkImageView fallback_view{ VK_NULL_HANDLE };
    /// The sampler the stand-in is read with (it also serves a full-screen packet's source attachments).
    VkSampler fallback_sampler{ VK_NULL_HANDLE };
    /// The sampler a shadow map is read with; null when the image cache has none (the recorder falls back to the
    /// pair above).
    VkSampler shadow_sampler{ VK_NULL_HANDLE };

  private:
    std::vector<Pass> passes_{};             ///< Slots, reused across frames (see beginPass).
    std::size_t       used_{ 0 };            ///< Passes resolved since beginFrame.
    std::uint64_t     storage_grows_{ 0 };   ///< How many times the reusable storage had to grow (see storageGrows).
};

VN_BACKEND_VK_NS_END
