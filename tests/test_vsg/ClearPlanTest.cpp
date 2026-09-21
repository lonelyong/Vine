/**
 * @brief The clear/load policy of a pass' attachments, as a table (see `.ai/design/vsg-reimplementation.md`
 * D6 and milestone M3).
 *
 * Every row here is a picture on the other side of it, and each one has been wrong in this backend's family
 * before:
 *
 *   * a fresh (UNDEFINED) image cannot be loaded - the bootstrap pass must clear;
 *   * only attachment 0 gets the pass' colour, the extras get transparent black (painting them the pass'
 *     colour looks plausible and is wrong);
 *   * depth clears to the REVERSE-Z far plane, which is 0.0 - a depth buffer cleared to 1.0 rejects every
 *     fragment under a GREATER comparison, i.e. an empty picture;
 *   * a depth a later pass depends on is never cleared, not even by the bootstrap rule.
 *
 * Device-free by construction: the plan is a function of three inputs, so the whole table is pinned here.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/vsg/core/ClearPlan.hpp>

using vine::graphics::RenderTarget;
using vine::vsg::core::AttachmentClear;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::DepthClear;
using vine::vsg::core::ImageLayout;
using vine::vsg::core::kReverseZFarDepth;
using vine::vsg::core::LoadOp;
using vine::vsg::core::loadOpVariantOf;
using vine::vsg::core::PassClearPlan;
using vine::vsg::core::planClearValues;
using vine::vsg::core::TargetShape;

namespace
{

/// @brief A one-colour-attachment target without depth.
TargetShape singleColor()
{
    TargetShape shape;
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    return shape;
}

/// @brief A three-colour-attachment (MRT) target with depth.
TargetShape multiTarget()
{
    TargetShape shape;
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA16F);
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA32F);
    shape.depth_format = RenderTarget::DepthFormat::D32;
    return shape;
}

/// @brief A one-colour-attachment target WITH a depth attachment.
TargetShape singleColorShapeWithDepth()
{
    TargetShape shape = singleColor();
    shape.depth_format = RenderTarget::DepthFormat::D32;
    return shape;
}

/// @brief A depth-only target (no colour attachments at all).
TargetShape depthOnly()
{
    TargetShape shape;
    shape.depth_format = RenderTarget::DepthFormat::D24;
    return shape;
}

}  // namespace

TEST(CoreClearPlanTest, ABootstrapPassClearsEverythingItHas)
{
    ClearPolicy policy;  // the pass itself asked for nothing
    policy.color_value[0] = 0.25F;
    policy.color_value[3] = 1.0F;

    const auto plan = planClearValues(multiTarget(), policy, /*bootstrap*/ true, /*depth_preserved*/ false);

    ASSERT_EQ(plan.colors.size(), 3U);
    EXPECT_TRUE(plan.bootstrap);
    for (const AttachmentClear& attachment : plan.colors) {
        EXPECT_EQ(attachment.load, LoadOp::Clear) << "an UNDEFINED image cannot be loaded";
    }
    EXPECT_FLOAT_EQ(plan.colors[0].clear[0], 0.25F) << "attachment 0 receives the pass' colour";
    EXPECT_FLOAT_EQ(plan.colors[0].clear[3], 1.0F);
    for (std::size_t index = 1; index < plan.colors.size(); ++index) {
        EXPECT_FLOAT_EQ(plan.colors[index].clear[0], 0.0F) << "an extra attachment gets transparent black";
        EXPECT_FLOAT_EQ(plan.colors[index].clear[3], 0.0F);
    }
    EXPECT_TRUE(plan.has_depth);
    EXPECT_EQ(plan.depth.load, LoadOp::Clear);
    EXPECT_FLOAT_EQ(plan.depth.clear, kReverseZFarDepth) << "reverse-Z: the far plane is 0.0, and 1.0 would reject everything";
}

TEST(CoreClearPlanTest, APassThatClearsNothingLoadsEverything)
{
    const auto plan = planClearValues(singleColor(), ClearPolicy{}, /*bootstrap*/ false, /*depth_preserved*/ false);

    ASSERT_EQ(plan.colors.size(), 1U);
    EXPECT_EQ(plan.colors[0].load, LoadOp::Load);
    EXPECT_EQ(plan.colors[0].store, vine::vsg::core::StoreOp::Store);
    EXPECT_FALSE(plan.has_depth);
    EXPECT_FALSE(plan.bootstrap);
}

TEST(CoreClearPlanTest, AClearRequestPutsTheColourOnAttachmentZeroOnly)
{
    ClearPolicy policy;
    policy.color           = true;
    policy.color_value[0]  = 1.0F;
    policy.color_value[1]  = 0.5F;
    policy.color_value[2]  = 0.0F;
    policy.color_value[3]  = 1.0F;

    const auto plan = planClearValues(multiTarget(), policy, /*bootstrap*/ false, /*depth_preserved*/ false);

    ASSERT_EQ(plan.colors.size(), 3U);
    EXPECT_EQ(plan.colors[0].load, LoadOp::Clear);
    EXPECT_FLOAT_EQ(plan.colors[0].clear[1], 0.5F);
    for (std::size_t index = 1; index < plan.colors.size(); ++index) {
        EXPECT_EQ(plan.colors[index].load, LoadOp::Clear) << "the extras clear too";
        EXPECT_FLOAT_EQ(plan.colors[index].clear[0], 0.0F) << "but never to the pass' colour";
        EXPECT_FLOAT_EQ(plan.colors[index].clear[3], 0.0F);
    }
    EXPECT_EQ(plan.depth.load, LoadOp::Load) << "the pass did not ask for a depth clear";
}

TEST(CoreClearPlanTest, ADepthOnlyTargetHasNoColourEntries)
{
    ClearPolicy policy;
    policy.depth = true;

    const auto plan = planClearValues(depthOnly(), policy, /*bootstrap*/ false, /*depth_preserved*/ false);

    EXPECT_TRUE(plan.colors.empty()) << "a depth-only pass has no colour attachment to clear";
    EXPECT_TRUE(plan.has_depth);
    EXPECT_EQ(plan.depth.load, LoadOp::Clear);
    EXPECT_FLOAT_EQ(plan.depth.clear, 0.0F);
}

TEST(CoreClearPlanTest, APreservedDepthIsNeverCleared)
{
    ClearPolicy policy;
    policy.color = true;
    policy.depth = true;

    // The bootstrap row is the interesting one: preservation outranks it.
    const auto plan = planClearValues(multiTarget(), policy, /*bootstrap*/ true, /*depth_preserved*/ true);

    EXPECT_EQ(plan.colors[0].load, LoadOp::Clear) << "the colour is still cleared: only the depth is preserved";
    EXPECT_EQ(plan.depth.load, LoadOp::Load)
        << "clearing it would make the dependent pass read the far plane and treat every fragment as visible";
    EXPECT_EQ(plan.depth.store, vine::vsg::core::StoreOp::Store);
}

TEST(CoreClearPlanTest, TheDepthClearValueIsThePolicyValueWhenItAsksForOne)
{
    ClearPolicy policy;
    policy.depth       = true;
    policy.depth_value = 0.75F;  // a pass that wants a specific depth to start from

    const auto plan = planClearValues(singleColorShapeWithDepth(), policy, /*bootstrap*/ false,
                                      /*depth_preserved*/ false);

    EXPECT_EQ(plan.depth.load, LoadOp::Clear);
    EXPECT_FLOAT_EQ(plan.depth.clear, 0.75F);
}

TEST(CoreClearPlanTest, AVariantNamesWhatAPassDoesAndNotWhatItClearsWith)
{
    // The render pass VARIANT a pass asks for is the swappable half of its graph: which attachments clear and
    // which keep what is there, and the layouts they start and end in. Two things this case pins, both of
    // which a target's render pass table depends on:
    //
    //   * a CLEAR starts from UNDEFINED (the contents are about to be discarded - naming the layout the
    //     previous pass left would be a transition for nothing) while a LOAD names exactly the layout the
    //     target leaves its attachments in;
    //   * the clear VALUES are no part of it: the value belongs to the pass instance, so two passes that
    //     clear to different colours share one render pass object.
    ClearPolicy first;
    first.color          = true;
    first.color_value[0] = 0.25F;
    first.depth          = true;

    const auto clear_plan = planClearValues(singleColorShapeWithDepth(), first, /*bootstrap*/ false,
                                            /*depth_preserved*/ false);
    const auto clear_variant =
        loadOpVariantOf(clear_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment);

    EXPECT_EQ(clear_variant.color_load, LoadOp::Clear);
    EXPECT_EQ(clear_variant.color_initial, ImageLayout::Undefined) << "a cleared attachment has no past";
    EXPECT_EQ(clear_variant.color_final, ImageLayout::ShaderReadOnly)
        << "a colour target is a texture for the next pass";
    EXPECT_TRUE(clear_variant.has_depth);
    EXPECT_EQ(clear_variant.depth_load, LoadOp::Clear);
    EXPECT_EQ(clear_variant.depth_initial, ImageLayout::Undefined);
    EXPECT_EQ(clear_variant.depth_final, ImageLayout::DepthAttachment);

    // The same variant, a different colour: the value is not identity.
    ClearPolicy second = first;
    second.color_value[0] = 0.75F;
    const auto other_plan = planClearValues(singleColorShapeWithDepth(), second, /*bootstrap*/ false,
                                           /*depth_preserved*/ false);
    EXPECT_TRUE(other_plan != clear_plan) << "the PLANS differ (the colours do)";
    EXPECT_TRUE(loadOpVariantOf(other_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment) ==
                clear_variant)
        << "the VARIANTS do not: one render pass object serves both";

    // A pass that clears nothing LOADS, and then the layouts it starts in are the ones the target leaves:
    // a LOAD has to name where the pixels it keeps really are.
    const auto load_plan = planClearValues(singleColorShapeWithDepth(), ClearPolicy{}, /*bootstrap*/ false,
                                           /*depth_preserved*/ false);
    const auto load_variant =
        loadOpVariantOf(load_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment);

    EXPECT_EQ(load_variant.color_load, LoadOp::Load);
    EXPECT_EQ(load_variant.color_initial, ImageLayout::ShaderReadOnly);
    EXPECT_EQ(load_variant.depth_load, LoadOp::Load);
    EXPECT_EQ(load_variant.depth_initial, ImageLayout::DepthAttachment);
    EXPECT_TRUE(load_variant != clear_variant) << "a loading pass is a DIFFERENT render pass object";

    // A preserved depth on a bootstrapping pass is the borrowed/promoted case: the colour is cleared, the
    // depth is loaded from where its lender left it.
    ClearPolicy borrowing;
    borrowing.color = true;
    const auto borrowed_plan = planClearValues(singleColorShapeWithDepth(), borrowing, /*bootstrap*/ true,
                                               /*depth_preserved*/ true);
    const auto borrowed_variant =
        loadOpVariantOf(borrowed_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment);
    EXPECT_EQ(borrowed_variant.color_load, LoadOp::Clear);
    EXPECT_EQ(borrowed_variant.depth_load, LoadOp::Load);
    EXPECT_EQ(borrowed_variant.depth_initial, ImageLayout::DepthAttachment);

    // A colour-only target: no depth participation at all, and the depth fields stay at their defaults.
    const auto color_only_plan = planClearValues(singleColor(), ClearPolicy{}, /*bootstrap*/ false,
                                                /*depth_preserved*/ false);
    const auto color_only_variant =
        loadOpVariantOf(color_only_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment);
    EXPECT_FALSE(color_only_variant.has_depth);
    EXPECT_EQ(color_only_variant.color_load, LoadOp::Load);
}
