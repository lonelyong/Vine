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
using vine::vsg::core::depthFinalLayout;
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

TEST(CoreClearPlanTest, ABorrowedDepthIsNeverClearedAndAnOwnDepthIsNotPreservedFromTheClear)
{
    ClearPolicy policy;
    policy.color = true;
    policy.depth = true;

    // A BORROWED depth: the image is the lender's, and clearing it would erase the depth the lender's pass
    // wrote for every reader after it - so not even the bootstrap rule clears it.
    {
        const auto borrowed = planClearValues(multiTarget(), policy, /*bootstrap*/ true, /*depth_borrowed*/ true);
        EXPECT_EQ(borrowed.colors[0].load, LoadOp::Clear) << "the colour is still cleared: only the depth is spared";
        EXPECT_EQ(borrowed.depth.load, LoadOp::Load) << "the lender's image is not this target's to clear";
        EXPECT_EQ(borrowed.depth.store, vine::vsg::core::StoreOp::Store);
    }

    // An OWN depth whose image was just built is the opposite case, and it is the one that cost a picture: a
    // fresh depth image holds nothing, so a bootstrap pass that loaded it left the geometry testing against
    // values from a frame the images no longer belonged to - the deferred demo came out sky-only after its
    // first resize (see core::planClearValues' rule 3).
    {
        const auto fresh =
            planClearValues(multiTarget(), policy, /*bootstrap*/ true, /*depth_borrowed*/ false);
        EXPECT_EQ(fresh.depth.load, LoadOp::Clear) << "a fresh own depth must be cleared: nothing may be loaded";
    }
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
        loadOpVariantOf(clear_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment,
                                          ImageLayout::DepthAttachment);

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
    EXPECT_TRUE(loadOpVariantOf(other_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment,
                                          ImageLayout::DepthAttachment) ==
                clear_variant)
        << "the VARIANTS do not: one render pass object serves both";

    // A pass that clears nothing LOADS, and then the layouts it starts in are the ones the target leaves:
    // a LOAD has to name where the pixels it keeps really are.
    const auto load_plan = planClearValues(singleColorShapeWithDepth(), ClearPolicy{}, /*bootstrap*/ false,
                                           /*depth_preserved*/ false);
    const auto load_variant =
        loadOpVariantOf(load_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment,
                                          ImageLayout::DepthAttachment);

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
        loadOpVariantOf(borrowed_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment,
                                          ImageLayout::DepthAttachment);
    EXPECT_EQ(borrowed_variant.color_load, LoadOp::Clear);
    EXPECT_EQ(borrowed_variant.depth_load, LoadOp::Load);
    EXPECT_EQ(borrowed_variant.depth_initial, ImageLayout::DepthAttachment);

    // A colour-only target: no depth participation at all, and the depth fields stay at their defaults.
    const auto color_only_plan = planClearValues(singleColor(), ClearPolicy{}, /*bootstrap*/ false,
                                                /*depth_preserved*/ false);
    const auto color_only_variant =
        loadOpVariantOf(color_only_plan, ImageLayout::ShaderReadOnly, ImageLayout::DepthAttachment,
                                          ImageLayout::DepthAttachment);
    EXPECT_FALSE(color_only_variant.has_depth);
    EXPECT_EQ(color_only_variant.color_load, LoadOp::Load);
}

TEST(CoreClearPlanTest, OnlyASampleableDepthOnlyTargetEndsReadyToBeSampled)
{
    // Where a target's depth is LEFT between passes is the other half of the plan, and the rule is a function
    // of the shape alone (see core::depthFinalLayout) - which is why a phase can pin it with no device at all:
    //
    //   * a sampleable DEPTH-ONLY target is a shadow map. Its depth IS the picture, so it ends in the layout a
    //     sampler reads and the pass that samples it needs no transition of its own;
    //   * every other shape ends in the attachment layout. A depth behind colour attachments is depth-tested
    //     by the next pass, and "hand it to a sampler" is a different decision than "it is a texture".
    EXPECT_EQ(depthFinalLayout(depthOnly(), /*depth_sampleable*/ true), ImageLayout::ShaderReadOnly)
        << "the depth of a shadow map is the thing the next pass reads";
    EXPECT_EQ(depthFinalLayout(depthOnly(), /*depth_sampleable*/ false), ImageLayout::DepthAttachment)
        << "a depth nobody samples stays an attachment: a transition would be a cost for nothing";
    EXPECT_EQ(depthFinalLayout(singleColorShapeWithDepth(), /*depth_sampleable*/ true), ImageLayout::DepthAttachment)
        << "a sampleable request does not turn a colour target's depth into a texture: the NEXT pass still "
           "depth-tests against it in the attachment layout";
    EXPECT_EQ(depthFinalLayout(singleColor(), /*depth_sampleable*/ true), ImageLayout::DepthAttachment)
        << "a shape with no depth has no layout to choose";
}

TEST(CoreClearPlanTest, TheShadowMapVariantNamesTheSampledDepthOnTheWayInAndOut)
{
    // The variant the shadow map's own pass asks for: its depth ends where the declaration says
    // (core::depthFinalLayout), so a pass that LOADS it must name THAT layout as the one it starts in - and a
    // pass that clears it discards the contents and starts from UNDEFINED. The two are different render pass
    // objects, which is what makes "the second writer of a shadow map" a variant rather than a rebuild.
    const TargetShape shadow_shape = depthOnly();
    const ImageLayout steady       = depthFinalLayout(shadow_shape, /*depth_sampleable*/ true);
    ASSERT_EQ(steady, ImageLayout::ShaderReadOnly);

    ClearPolicy clear_depth;
    clear_depth.depth = true;
    const auto clearing = planClearValues(shadow_shape, clear_depth, /*bootstrap*/ true, /*depth_preserved*/ false);
    const auto clear_variant = loadOpVariantOf(clearing, ImageLayout::ShaderReadOnly, steady, steady);
    EXPECT_TRUE(clear_variant.has_depth);
    EXPECT_EQ(clear_variant.depth_load, LoadOp::Clear);
    EXPECT_EQ(clear_variant.depth_initial, ImageLayout::Undefined) << "a cleared attachment has no past";
    EXPECT_EQ(clear_variant.depth_final, steady) << "and it still ends where the sampler reads it";

    const auto loading =
        planClearValues(shadow_shape, ClearPolicy{}, /*bootstrap*/ false, /*depth_preserved*/ false);
    const auto load_variant = loadOpVariantOf(loading, ImageLayout::ShaderReadOnly, steady, steady);
    EXPECT_EQ(load_variant.depth_load, LoadOp::Load);
    EXPECT_EQ(load_variant.depth_initial, steady)
        << "a LOAD names where the pixels it keeps really are - the shadow map is a texture by then";
    EXPECT_EQ(load_variant.depth_final, steady);
    EXPECT_TRUE(load_variant != clear_variant) << "clearing and loading are two render pass objects";
}
