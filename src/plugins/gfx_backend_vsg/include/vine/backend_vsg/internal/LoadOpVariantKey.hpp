#pragma once

#include <cstdint>

#include <vine/graphics/DrawState.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

VN_BACKEND_VSG_CORE_NS_BEGIN

/**
 * @brief Layout an attachment is in at a point in time (the thing a barrier changes).
 *
 * @details
 * - THIS BACKEND'S OWN TYPE, not the seam's. A layout is one family's spelling of "this image is being written
 *   as colour"; D3D12 states a resource state, Metal tracks hazards itself, so the SDK states only `LoadOp` /
 *   `StoreOp` and what a pass declares, and each backend derives its transitions. The type was the last
 *   Vulkan-shaped entry on the public surface (backlog T15) and moved here with its only producer - review 8
 *   §4.7/§4.8 records the move and the rule behind it.
 */
enum class ImageLayout : std::uint8_t
{
    Undefined,       // No layout; an image being used for the first time.
    ColorAttachment, // Being written as a colour attachment.
    DepthAttachment, // Being written as a depth attachment.
    ShaderReadOnly,  // Being sampled.
    Present,         // Handed to the presentation engine.
};

/**
 * @brief The KEY of one render-pass variant: what a pass asks of its attachments before it draws.
 *
 * @details
 * - Kept apart from `RenderPassCompatibility` because the two have different lifetimes: this one changes with a
 *   pass' clear policy (and is what a bootstrap swaps for one frame), while the compiled pipeline only depends
 *   on the compatibility half. The API draws the line in the same place: load/store operations and layouts are
 *   NOT part of render pass compatibility, so several variants of one attachment set MAY share one compiled
 *   pipeline - which is what lets a target serve a clearing pass and a loading pass without recompiling.
 * - WHAT IT CARRIES is every property of the render pass OBJECT a graph is built with: whether each attachment
 *   clears or keeps what is there, whether its result survives, and which layout it is in at the start and at
 *   the end of the pass. The colour attachments move together (a pass clears all of them or none - see
 *   `planClearValues`), so one entry describes the set; the DEPTH is independent, because a later pass may load
 *   the depth an earlier one wrote while the colour is cleared, and because a borrowed depth starts in the
 *   layout its lender left it in.
 * - IT IS THIS PLUGIN'S KEY. `TargetObjects` builds its render passes by exactly this type and the engine never
 *   names it; a D3D12 or Metal backend keys its own variants (PSO compatibility / no pass object at all), so the
 *   type must not re-enter the SDK (review 8 §4.8).
 */
struct LoadOpVariantKey {
    vn::graphics::LoadOp      color_load{ vn::graphics::LoadOp::Load };   // Every colour attachment: keep what is there, or clear it.
    vn::graphics::StoreOp     color_store{ vn::graphics::StoreOp::Store }; // Whether the colour survives the pass.
    ImageLayout               color_initial{ ImageLayout::Undefined };     // Layout the colour starts in.
    ImageLayout               color_final{ ImageLayout::ColorAttachment }; // Layout it is left in.
    bool                      has_depth{ false };                          // Whether the target has a depth attachment at all.
    vn::graphics::LoadOp      depth_load{ vn::graphics::LoadOp::Load };    // Keep the depth, or clear it.
    vn::graphics::StoreOp     depth_store{ vn::graphics::StoreOp::Store }; // Whether the depth survives the pass.
    ImageLayout               depth_initial{ ImageLayout::Undefined };     // Layout the depth starts in.
    ImageLayout               depth_final{ ImageLayout::DepthAttachment }; // Layout it is left in.

    /** @brief Compares the whole key. */
    [[nodiscard]] bool operator==(const LoadOpVariantKey& other) const noexcept
    {
        return color_load == other.color_load && color_store == other.color_store && color_initial == other.color_initial &&
               color_final == other.color_final && has_depth == other.has_depth && depth_load == other.depth_load &&
               depth_store == other.depth_store && depth_initial == other.depth_initial && depth_final == other.depth_final;
    }
};

VN_BACKEND_VSG_CORE_NS_END
