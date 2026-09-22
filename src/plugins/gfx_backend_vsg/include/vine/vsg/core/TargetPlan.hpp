#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What has to happen to a render target this frame, as a pure function of what it is and what
 * the frame wants - and the depth plan that goes with it.
 *
 * WHY A FUNCTION AND NOT A CODE PATH. "The target changed" hides three different events whose costs
 * differ by two orders of magnitude, and only one of them may lose anything:
 *
 *   * the SHAPE changed (attachment count / colour formats / depth format / samples / subpass): render
 *     pass compatibility really changed, so the pass graph, the framebuffer and the pipelines that
 *     were compiled against it must be rebuilt;
 *   * the EXTENT changed: the only thing that actually has to be replaced is the images and the views,
 *     plus whatever names those views (framebuffers, descriptor sets) - the render pass, the pass
 *     graph, the content slots and the full-screen program slots must NOT be rebuilt, because a
 *     viewport is dynamic state and every full-screen fragment stage samples by uv. Rebuilding them is
 *     what a resize used to cost (measured: ~26 ms per program slot, ~133 ms for a maximize);
 *   * nothing usable arrived yet (a target laid out at 0x0, or a target whose attachments were
 *     invalidated): it must not be drawn into, and the condition must be distinguishable from "the
 *     target is gone".
 *
 * Written as a decision function, the caller cannot accidentally take the expensive path (the value
 * has to say Rebuild), the "resize must not recompile" requirement becomes observable - a resize phase
 * asserts the rebuild counters do not move - and the whole table is testable without a device.
 *
 * DEPTH IS THE OTHER HALF, and it is derived rather than stored because three facts interact:
 * promotion (the depth may be sampled), borrowing (another target's depth image is used as this one's
 * attachment) and preservation (a pass LOADs the depth an earlier pass wrote). All three change the
 * attachment's initial layout, and a target whose promotion was revoked by any depth-preserving pass
 * must stop being bindable as a texture. One function decides all three, so the three cannot disagree.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief What to do with an off-screen target this frame. */
enum class TargetAction : std::uint8_t
{
    None,           ///< Usable as it is.
    Repair,         ///< Nothing may be drawn into it yet (see RepairReason); no GPU object changes.
    ResizeInPlace,  ///< Same shape, new extent: replace images/views/framebuffers and re-point descriptors.
    Rebuild,        ///< The shape changed: rebuild the pass graph, the framebuffer and the pipelines.
};

/** @brief Why a target has to be repaired before it can be used. */
enum class RepairReason : std::uint8_t
{
    None,          ///< Not a repair.
    SizeUnknown,   ///< The extent is not usable (0 in either dimension), so nothing can be built.
    Bootstrap,     ///< Freshly built or invalidated: the first pass in must clear (an UNDEFINED image cannot be loaded).
};

/**
 * @brief The target properties that are part of render-pass COMPATIBILITY, and therefore of a pipeline's key.
 *
 * What a resize may NOT be allowed to hide here is an extent: pixel size is a runtime resource, not an
 * identity. What it MUST contain is everything a rebuild really depends on: attachment formats, the
 * depth format (or its absence), the sample count and the subpass structure.
 *
 * The device formats are carried next to the engine's spelling because only the device layer can fill them
 * and only they can tell two render passes apart that the engine's vocabulary calls the same thing (an sRGB
 * surface and a linear RGBA8 image are both "RGBA8"; see RenderPassCompatibility). 0, the device's
 * UNDEFINED, means "no such attachment" - and, for a shape that never learned them, "not known".
 */
struct TargetShape
{
    std::vector<vine::graphics::RenderTarget::ColorFormat> color_formats;  ///< One per colour attachment.
    std::optional<vine::graphics::RenderTarget::DepthFormat> depth_format; ///< Absent for a colour-only target.
    std::vector<std::uint32_t> device_color_formats;  ///< The same attachments, as the device spells them.
    std::uint32_t              device_depth_format{0}; ///< The device's depth format; 0 = no depth.
    std::uint32_t              samples{1};             ///< Sample count.
    std::uint32_t              subpass{0};             ///< Subpass the pipeline targets.

    /** @brief Compares the compatibility-relevant properties. */
    [[nodiscard]] bool operator==(const TargetShape& other) const noexcept;

    /** @brief Gets the compatibility half of a pipeline key for a target that has this shape.
     *
     * One spelling for one fact: a pipeline is compiled against exactly the render pass its target owns, so
     * the key's compatibility half IS this shape's compatibility-relevant half - built here rather than by
     * every caller, because a caller that built it by hand could drop the device formats and silently hand
     * one compiled pipeline to two render passes that are not compatible (see RenderPassCompatibility).
     *
     * @return The shape's compatibility, device formats included.
     */
    [[nodiscard]] RenderPassCompatibility compatibility() const noexcept;
};

/** @brief A target as the frame wants it: an extent plus a shape. */
struct TargetDesc
{
    int         width{0};   ///< Extent in device pixels (0 = not known yet).
    int         height{0};  ///< Extent in device pixels (0 = not known yet).
    TargetShape shape;      ///< Compatibility-relevant shape.

    /** @brief Compares extent and shape. */
    [[nodiscard]] bool operator==(const TargetDesc& other) const noexcept;
};

/** @brief What the backend currently has for a target. */
struct TargetInstance
{
    TargetDesc    desc;                     ///< The description the current attachments were built for.
    std::uint64_t generation{0};            ///< Bumped every time the attachments were replaced.
    bool          built{false};             ///< Attachments exist for desc.
    bool          attachments_invalidated{false};  ///< They exist but no longer describe anything usable.
};

/** @brief The plan for one target. */
struct TargetDecision
{
    TargetAction action{TargetAction::None};      ///< What to do.
    RepairReason reason{RepairReason::None};      ///< Why, when action is Repair.
};

/** @brief Decides what has to happen to @p current so it can serve @p wanted.
 *
 * Precedence is deliberate: an unusable extent decides before a shape change (there is nothing to
 * rebuild for a 0x0 target), a shape change decides before a resize (compatibility wins over
 * extent), and a shape change also decides before the load-op repairs - "the first pass in clears"
 * is an instruction about attachments, and a rebuild replaces the pass those attachments belong
 * to (its fresh attachments answer Bootstrap on the next plan).
 *
 * @param current What the backend has now.
 * @param wanted What this frame wants.
 * @return The action to take, and its reason when it is a repair.
 */
[[nodiscard]] TargetDecision planTarget(const TargetInstance& current, const TargetDesc& wanted) noexcept;

/** @brief The depth facts a target's plan is derived from. */
struct DepthFacts
{
    bool        has_depth{false};                 ///< The target has a depth attachment.
    bool        promotion{false};                 ///< The host asked for a sampleable depth.
    bool        borrowed{false};                  ///< The depth attachment is another target's image.
    const void* source{nullptr};                  ///< The lender, when borrowed.
    bool        any_pass_preserves_depth{false};  ///< Some pass of this target depth-LOADs what an earlier pass wrote.
};

/** @brief The depth plan for one target. */
struct DepthPlan
{
    bool        has_depth{false};    ///< There is a depth attachment.
    bool        sampleable{false};   ///< Shaders may sample it (promotion survived, nothing preserves it).
    bool        borrowed{false};     ///< It is the lender's image; the policy is the lender's.
    bool        preserve{false};     ///< A pass depends on the depth an earlier pass wrote.
    const void* source{nullptr};     ///< The lender, when borrowed.
};

/** @brief Derives the depth plan from the facts that interact (see the file note).
 *
 * @param facts Depth facts of the target and of this frame's passes.
 * @return The plan: a borrowed depth is never sampleable and never preserved by the borrower; an own
 *         depth is sampleable only while promotion survives - any depth-preserving pass revokes it.
 */
[[nodiscard]] DepthPlan depthPlan(const DepthFacts& facts) noexcept;

}  // namespace core

V_VSG_NS_END
