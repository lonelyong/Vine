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
using vine::vsg::detail::planPassVariant;
using vine::vsg::detail::passVariantIsStale;

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

/**
 * The materialised variant adds what the caller has to get right on top of the
 * pure depth policy: the one-frame COLOUR bootstrap, the one-frame depth SEED and
 * the one-frame promotion REVOKE, plus the layout a target keeps its depth in
 * between passes. Each of these was a real validation error (or a wiped frame)
 * before it was pinned here.
 */
TEST(PassRenderPassPlanTest, ColorBootstrapClearsOnceAndTheSteadyVariantLoads)
{
    // First pass into a fresh target: the colour image is UNDEFINED, so it has to
    // be CLEARed — as a TRANSIENT variant, so the pass stops clearing afterwards.
    const auto first = planPassVariant(/*has_color*/ true, /*has_depth*/ false, /*borrowed*/ false,
                                       /*promote_requested*/ false, /*any_load_pass*/ false, /*depth_seeded*/ false,
                                       /*color_seeded*/ false, /*want_color_clear*/ false, /*want_depth_clear*/ true,
                                       /*depth_left_promoted*/ false);
    EXPECT_EQ(first.color_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_TRUE(first.transient);
    EXPECT_EQ(first.steady_color_load, VK_ATTACHMENT_LOAD_OP_LOAD);

    // The frame after it: the image exists, so the pass LOADs and is steady. A
    // pass that kept clearing would wipe what its siblings drew, every frame.
    const auto steady = planPassVariant(true, false, false, false, false, false, /*color_seeded*/ true, false, true,
                                        false);
    EXPECT_EQ(steady.color_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_FALSE(steady.transient);
}

TEST(PassRenderPassPlanTest, PreservingPassSeedsThenLoadsTheAttachmentLayout)
{
    // Unseeded depth AND unseeded colour: the same frame carries the depth seed
    // and the colour bootstrap, both as one-frame variants.
    const auto seed = planPassVariant(true, true, false, /*promote_requested*/ true, false, /*depth_seeded*/ false,
                                      /*color_seeded*/ false, /*want_color_clear*/ false, /*want_depth_clear*/ false,
                                      /*depth_left_promoted*/ false);
    EXPECT_EQ(seed.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(seed.depth_initial, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_TRUE(seed.transient);
    EXPECT_FALSE(seed.promote_depth);
    EXPECT_EQ(seed.steady_depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(seed.color_load, VK_ATTACHMENT_LOAD_OP_CLEAR); // the colour bootstrap, same frame

    // Seeded and steady: the pass LOADs the layout its target keeps.
    const auto steady = planPassVariant(true, true, false, true, true, /*depth_seeded*/ true, true, false, false,
                                        /*depth_left_promoted*/ false);
    EXPECT_EQ(steady.depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(steady.color_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_FALSE(steady.transient);
}

TEST(PassRenderPassPlanTest, PromotionRevokeNamesThePromotedLayoutForOneFrame)
{
    // The depth is still in SHADER_READ_ONLY (a promoting pass left it there and
    // no pass that records earlier has run this frame): the preserving pass has to
    // NAME that layout, for one frame, and then hand the image back attached.
    const auto revoke = planPassVariant(true, true, false, true, false, /*depth_seeded*/ true, true, false, false,
                                        /*depth_left_promoted*/ true);
    EXPECT_EQ(revoke.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(revoke.depth_initial, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_TRUE(revoke.transient);
    EXPECT_EQ(revoke.steady_depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
}

TEST(PassRenderPassPlanTest, DepthOnlyTargetKeepsItsDepthSampleable)
{
    // A depth-only target's depth always ends sampleable, so a preserving pass
    // takes the SHADER_READ_ONLY layout as its STEADY one — no revoke frame — and
    // a clearing pass still clears.
    const auto preserve = planPassVariant(/*has_color*/ false, /*has_depth*/ true, false, /*promote_requested*/ true,
                                          true, /*depth_seeded*/ true, /*color_seeded*/ false, false,
                                          /*want_depth_clear*/ false, /*depth_left_promoted*/ true);
    EXPECT_EQ(preserve.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(preserve.depth_initial, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(preserve.steady_depth_initial, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_FALSE(preserve.transient);

    // The very same target asked to CLEAR (D51: a depth-only pass has to honour
    // it too, rather than always clearing).
    const auto clear = planPassVariant(false, true, false, true, true, true, false, false, /*want_depth_clear*/ true,
                                       true);
    EXPECT_EQ(clear.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_EQ(clear.depth_initial, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_FALSE(clear.transient);
}

TEST(PassRenderPassPlanTest, ClearingPassPromotesOnlyWhileNothingLoads)
{
    const auto promoting = planPassVariant(true, true, false, /*promote_requested*/ true, /*any_load_pass*/ false,
                                           true, true, /*want_color_clear*/ true, /*want_depth_clear*/ true, false);
    EXPECT_TRUE(promoting.promote_depth);
    EXPECT_EQ(promoting.depth_initial, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_FALSE(promoting.transient);

    const auto revoked = planPassVariant(true, true, false, true, /*any_load_pass*/ true, true, true, true, true,
                                         false);
    EXPECT_FALSE(revoked.promote_depth);
}

TEST(PassRenderPassPlanTest, BorrowedDepthIsLoadedUnpromotedAndNeverSeeded)
{
    const auto variant = planPassVariant(true, true, /*borrowed*/ true, /*promote_requested*/ true,
                                         /*any_load_pass*/ false, /*depth_seeded*/ false, /*color_seeded*/ true,
                                         /*want_color_clear*/ false, /*want_depth_clear*/ false,
                                         /*depth_left_promoted*/ false);
    EXPECT_EQ(variant.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(variant.depth_initial, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_FALSE(variant.promote_depth);
    EXPECT_FALSE(variant.transient);
}

TEST(PassRenderPassPlanTest, StaleComparesRequestsNotMaterialisedLoadOps)
{
    // The same frame builds one pass twice (setupContentSlot + render). The
    // bootstrap/seed flags flip in between, but the REQUESTS did not, so the
    // second build must keep the variant the first one chose.
    EXPECT_FALSE(passVariantIsStale(/*recorded*/ false, false, /*now*/ false, false));
    // A run-time policy change IS a request change, and has to rebuild.
    EXPECT_TRUE(passVariantIsStale(false, false, true, false));
    EXPECT_TRUE(passVariantIsStale(true, false, true, true));
}
