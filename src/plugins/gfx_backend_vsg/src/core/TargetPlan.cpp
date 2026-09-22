#include <vine/vsg/core/TargetPlan.hpp>

V_VSG_NS_BEGIN

namespace core
{

bool TargetShape::operator==(const TargetShape& other) const noexcept
{
    return color_formats == other.color_formats && depth_format == other.depth_format &&
           device_color_formats == other.device_color_formats &&
           device_depth_format == other.device_depth_format && samples == other.samples &&
           subpass == other.subpass;
}

RenderPassCompatibility TargetShape::compatibility() const noexcept
{
    RenderPassCompatibility compatibility;
    compatibility.color_formats        = color_formats;
    compatibility.depth_format         = depth_format;
    compatibility.device_color_formats = device_color_formats;
    compatibility.device_depth_format  = device_depth_format;
    compatibility.samples              = samples;
    compatibility.subpass              = subpass;
    return compatibility;
}

bool TargetDesc::operator==(const TargetDesc& other) const noexcept
{
    return width == other.width && height == other.height && shape == other.shape;
}

TargetDecision planTarget(const TargetInstance& current, const TargetDesc& wanted) noexcept
{
    // 1. An extent that cannot be built decides first: there is nothing to rebuild for a 0x0 target,
    //    and "resized to nothing" must stay distinguishable from "attachments invalidated".
    if (wanted.width <= 0 || wanted.height <= 0)
    {
        return {TargetAction::Repair, RepairReason::SizeUnknown};
    }

    // 2. Compatibility changed: render pass, framebuffers and every pipeline compiled against them are
    //    no longer valid. This OUTRANKS the load-op repairs below, because those repairs are instructions
    //    about attachments: "no GPU object changes, the first pass in clears" is a lie once the pass
    //    itself has to be rebuilt - and the rebuild's fresh attachments answer Repair(Bootstrap) on the
    //    very next plan anyway, so nothing is lost by deciding it first. Rebuild also wins over a
    //    simultaneous extent change.
    if (!(current.desc.shape == wanted.shape))
    {
        return {TargetAction::Rebuild, RepairReason::None};
    }

    // 3. Nothing usable yet (never built, or invalidated): the first pass in has to clear, because an
    //    UNDEFINED colour image cannot be loaded.
    if (!current.built || current.attachments_invalidated)
    {
        return {TargetAction::Repair, RepairReason::Bootstrap};
    }

    // 4. Same shape, new extent: replace the images and views, re-point what names them, and keep the
    //    render pass, the pass graph, the content slots and the full-screen program slots.
    if (current.desc.width != wanted.width || current.desc.height != wanted.height)
    {
        return {TargetAction::ResizeInPlace, RepairReason::None};
    }

    return {TargetAction::None, RepairReason::None};
}

DepthPlan depthPlan(const DepthFacts& facts) noexcept
{
    DepthPlan plan;
    plan.has_depth = facts.has_depth;
    if (!facts.has_depth)
    {
        return plan;
    }

    if (facts.borrowed)
    {
        // The image is the lender's: this target does not own the policy (no depth-LOAD variant of its
        // own) and must not promise the depth to a shader - revoking promotion is the lender's call.
        plan.borrowed   = true;
        plan.source     = facts.source;
        plan.sampleable = false;
        plan.preserve   = false;
        return plan;
    }

    plan.preserve = facts.any_pass_preserves_depth;
    // Sampleable only while the promotion survives: a depth-preserving pass keeps the image in the
    // attachment layout, so "sample it as a texture" stops being possible for the whole target.
    plan.sampleable = facts.promotion && !facts.any_pass_preserves_depth;
    return plan;
}

}  // namespace core

V_VSG_NS_END
