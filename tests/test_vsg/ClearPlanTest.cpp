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
using vine::vsg::core::kReverseZFarDepth;
using vine::vsg::core::LoadOp;
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
