#include <vine/vsg/core/ClearPlan.hpp>

V_VSG_NS_BEGIN

namespace core
{

namespace
{

/// @brief Whether two clear values are the same (the four channels, bit for bit).
bool sameValue(const float* a, const float* b) noexcept
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

}  // namespace

bool AttachmentClear::operator==(const AttachmentClear& other) const noexcept
{
    return load == other.load && store == other.store && sameValue(clear, other.clear);
}

bool DepthClear::operator==(const DepthClear& other) const noexcept
{
    return load == other.load && store == other.store && clear == other.clear;
}

bool PassClearPlan::operator==(const PassClearPlan& other) const noexcept
{
    return colors == other.colors && has_depth == other.has_depth && depth == other.depth &&
           bootstrap == other.bootstrap;
}

PassClearPlan planClearValues(const TargetShape& shape, const ClearPolicy& policy, bool bootstrap,
                              bool depth_preserved) noexcept
{
    PassClearPlan plan;
    plan.bootstrap = bootstrap;
    plan.has_depth = shape.depth_format.has_value();

    for (std::size_t index = 0; index < shape.color_formats.size(); ++index) {
        AttachmentClear attachment;
        // Rule 4: a clear request clears every colour attachment; only attachment 0 receives the pass' colour.
        // The extras keep the transparent black they were constructed with, which is the value that means
        // "nothing here" - painting them the pass' colour is the wrong picture that looks plausible.
        if (bootstrap || policy.color) {
            attachment.load = LoadOp::Clear;
            if (index == 0U) {
                attachment.clear[0] = policy.color_value[0];
                attachment.clear[1] = policy.color_value[1];
                attachment.clear[2] = policy.color_value[2];
                attachment.clear[3] = policy.color_value[3];
            }
        }
        plan.colors.push_back(attachment);
    }

    if (plan.has_depth) {
        // Rule 3: a preserved depth is what a later pass LOADS; clearing it would make that pass read the far
        // plane and treat every fragment as visible. The rule outranks the bootstrap: a target whose depth was
        // written by an earlier pass is exactly the case it exists for.
        if (!depth_preserved && (bootstrap || policy.depth)) {
            plan.depth.load  = LoadOp::Clear;
            plan.depth.clear = policy.depth_value;
        }
    }

    return plan;
}

LoadOpVariantKey loadOpVariantOf(const PassClearPlan& plan, ImageLayout color_final,
                                 ImageLayout depth_final) noexcept
{
    LoadOpVariantKey variant;
    if (!plan.colors.empty()) {
        // The colour attachments move together (rule 4), so attachment 0 speaks for the set.
        const AttachmentClear& color = plan.colors.front();
        variant.color_load    = color.load;
        variant.color_store   = color.store;
        variant.color_initial = color.load == LoadOp::Clear ? ImageLayout::Undefined : color_final;
        variant.color_final   = color_final;
    }
    variant.has_depth = plan.has_depth;
    if (plan.has_depth) {
        variant.depth_load    = plan.depth.load;
        variant.depth_store   = plan.depth.store;
        variant.depth_initial = plan.depth.load == LoadOp::Clear ? ImageLayout::Undefined : depth_final;
        variant.depth_final   = depth_final;
    }
    return variant;
}

}  // namespace core

V_VSG_NS_END
