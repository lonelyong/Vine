/**
 * @brief Render-pass variant selection tests (§28, device-free).
 *
 * The §28 restructure replaces "one render pass per target" with "one render
 * pass per pass". The variant each pass needs is pure policy, so it is factored
 * into planPassRenderPass() and pinned here without a Vulkan device. The
 * invariants these tests lock down are the ones that are easy to get subtly
 * wrong and that no validation layer reports:
 *
 *  - a colour LOAD is what lets a non-clearing pass composite over earlier
 *    passes instead of wiping them;
 *  - LOAD and depth promotion are mutually exclusive: once one pass LOADs the
 *    depth, no pass may leave it in SHADER_READ_ONLY or the LOAD attaches a
 *    layout it cannot use;
 *  - a LOAD needs a DEFINED image, so an unseeded target requests a one-frame
 *    seed (the CLEAR variant) first;
 *  - a borrowed depth belongs to its source's policy: LOAD, unpromoted, unseeded.
 */

#include <gtest/gtest.h>

#include "VsgPipelineFactory.hpp"

using vine::vsg::detail::planPassRenderPass;

TEST(PassRenderPassPlanTest, ColorOnlyPassFollowsColorClear)
{
    const auto loading =
        planPassRenderPass(/*color_clear*/ false, true, /*has_depth*/ false, false, false, false, false);
    EXPECT_EQ(loading.color_load, VK_ATTACHMENT_LOAD_OP_LOAD);

    const auto clearing = planPassRenderPass(true, true, false, false, false, false, false);
    EXPECT_EQ(clearing.color_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
}

TEST(PassRenderPassPlanTest, ClearingPassPromotesWhenNoLoadPassExists)
{
    const auto plan = planPassRenderPass(true, /*want_depth_clear*/ true, true,
                                         /*promote_requested*/ true, /*any_load_pass*/ false,
                                         /*depth_seeded*/ false, /*borrowed*/ false);
    EXPECT_EQ(plan.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_EQ(plan.depth_initial, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_TRUE(plan.promote_depth);
    EXPECT_FALSE(plan.seed_required);
}

TEST(PassRenderPassPlanTest, LoadPassBlocksPromotionInvariant)
{
    const auto plan = planPassRenderPass(true, true, true, /*promote_requested*/ true,
                                         /*any_load_pass*/ true, false, false);
    EXPECT_FALSE(plan.promote_depth);
}

TEST(PassRenderPassPlanTest, SeededPreservingPassLoadsAttachment)
{
    const auto plan = planPassRenderPass(true, /*want_depth_clear*/ false, true, true, false,
                                         /*depth_seeded*/ true, false);
    EXPECT_EQ(plan.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(plan.depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_FALSE(plan.promote_depth);
    EXPECT_FALSE(plan.seed_required);
}

TEST(PassRenderPassPlanTest, UnseededPreservingPassNeedsASeed)
{
    const auto plan = planPassRenderPass(true, false, true, true, false, /*depth_seeded*/ false, false);
    EXPECT_EQ(plan.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_TRUE(plan.seed_required);
}

TEST(PassRenderPassPlanTest, BorrowedDepthAlwaysLoadsAndNeverPromotes)
{
    const auto plan = planPassRenderPass(/*color_clear*/ false, /*want_depth_clear*/ false, true,
                                         /*promote_requested*/ true, false, false, /*borrowed*/ true);
    EXPECT_EQ(plan.color_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(plan.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(plan.depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_FALSE(plan.promote_depth);
    EXPECT_FALSE(plan.seed_required);
}
