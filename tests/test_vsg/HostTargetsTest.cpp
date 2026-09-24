/**
 * @brief The host's off-screen targets: the description COPIED, and the facts the plan resolves from (see
 * `.ai/design/vsg-reimplementation.md` §11.16bf, `api/HostTargets.hpp`).
 *
 * Device-free by construction: with no device nothing is built - `ensure` keeps the description and answers
 * `NotBuilt` - so what is checked here is the half a device cannot prove: that no host object is
 * dereferenced after the call (the facts answer the SNAPSHOT, not what the object says later), that the
 * facts say "built" only while the objects exist, that a borrowed depth is the lender's policy and never the
 * borrower's to promise, and that a shadow statement travels as the host stated it. The built half
 * (creation, the resize / rebuild answers, the sampled round trip through a screen draw) is driven through
 * the SDK in `VsgBackendTest`, where a device and a window exist.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/api/HostTargets.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>

using vn::graphics::RenderTarget;
using vn::vsg::core::TargetFacts;
using vn::vsg::HostTargets;

TEST(HostTargetsTest, TheDescriptionIsASnapshotAndTheFactsSayWhatIsBuilt)
{
    HostTargets targets;

    const vn::intrusive_ptr<RenderTarget> sdk(new RenderTarget());
    sdk->attachColor(RenderTarget::ColorFormat::RGBA16F);
    sdk->attachDepth(RenderTarget::DepthFormat::D32F);
    sdk->setSize(64, 32);

    // With no device nothing can be built, and the description is still KEPT: a host configures a target
    // before a session exists (the engine announces sizes and builds pipelines before initialize()).
    const HostTargets::Ensured ensured = targets.ensure(*sdk, {});
    ASSERT_NE(ensured.entry, nullptr);
    EXPECT_EQ(ensured.state, HostTargets::State::NotBuilt);
    EXPECT_EQ(targets.live(), 1U);
    ASSERT_EQ(ensured.entry->target, nullptr);

    TargetFacts row;
    targets.facts(*ensured.entry, row);
    EXPECT_EQ(row.target, static_cast<const void*>(sdk.get()));
    EXPECT_EQ(row.wanted.width, 64);
    EXPECT_EQ(row.wanted.height, 32);
    ASSERT_EQ(row.wanted.shape.color_formats.size(), 1U);
    EXPECT_EQ(row.wanted.shape.color_formats[0], RenderTarget::ColorFormat::RGBA16F);
    ASSERT_TRUE(row.wanted.shape.depth_format.has_value());
    EXPECT_EQ(*row.wanted.shape.depth_format, RenderTarget::DepthFormat::D32F);
    EXPECT_TRUE(row.wanted.shape.device_color_formats.empty())
        << "no objects: the device's own spelling is unknown, and unknown must not read as absent";
    EXPECT_FALSE(row.current.built) << "nothing has been recorded into attachments that do not exist";
    EXPECT_TRUE(row.depth.has_depth);
    EXPECT_TRUE(row.depth.promotion) << "the host asked for a sampleable depth (the default)";
    EXPECT_FALSE(row.depth.borrowed);
    EXPECT_EQ(row.shadow.light, nullptr) << "a target that says nothing is not a shadow map";

    // A SNAPSHOT, not the host's object: what the object says later is the host's business until it announces
    // the target again - the plan runs on what was copied.
    sdk->setSize(128, 64);
    targets.facts(*ensured.entry, row);
    EXPECT_EQ(row.wanted.width, 64) << "the facts answer the description as it was announced";
    (void)targets.ensure(*sdk, {});
    targets.facts(*ensured.entry, row);
    EXPECT_EQ(row.wanted.width, 128) << "and a new announcement carries the new extent";

    // A description that cannot make a target yet (no colour attachment, or no extent).
    const vn::intrusive_ptr<RenderTarget> bare(new RenderTarget());
    EXPECT_EQ(targets.ensure(*bare, {}).state, HostTargets::State::NotBuilt);

    // The release forgets the identity, and answers once.
    EXPECT_TRUE(targets.release(sdk.get()));
    EXPECT_FALSE(targets.release(sdk.get()));
    EXPECT_EQ(targets.find(sdk.get()), nullptr);
    EXPECT_EQ(targets.live(), 1U) << "the bare target is still held";
}

TEST(HostTargetsTest, ABorrowedDepthAndAShadowStatementAreTheHostsOwnWords)
{
    HostTargets targets;

    const vn::intrusive_ptr<RenderTarget> lender(new RenderTarget());
    lender->attachColor(RenderTarget::ColorFormat::RGBA8);
    lender->attachDepth(RenderTarget::DepthFormat::D24);
    lender->setDepthPromotion(false);  // it will be depth-tested against, so it must not become a texture
    lender->setSize(32, 32);
    EXPECT_EQ(targets.ensure(*lender, {}).state, HostTargets::State::NotBuilt);

    const vn::intrusive_ptr<RenderTarget> borrower(new RenderTarget());
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->shareDepth(lender);
    borrower->setSize(32, 32);
    const HostTargets::Ensured ensured = targets.ensure(*borrower, {});

    // With NO DEVICE nothing is built at all, so that answers first - "NotBuilt" is not a diagnosis, it is
    // the state a description waits in. What a missing LENDER is answered with belongs to a build (`ensure`
    // with a device resolves the lender while it still holds the host's word - see VsgBackendTest's off-screen
    // case, which drives it).
    EXPECT_EQ(ensured.state, HostTargets::State::NotBuilt);

    TargetFacts borrower_row;
    targets.facts(*ensured.entry, borrower_row);
    EXPECT_TRUE(borrower_row.depth.borrowed);
    EXPECT_FALSE(borrower_row.depth.promotion) << "a borrowed depth is never this target's to promise";
    EXPECT_EQ(borrower_row.depth.source, static_cast<const void*>(lender.get()));

    // The shadow statement, as the host stated it: whose map it is, and the matrix the producer rendered
    // with. Both travel; neither is derived.
    const vn::intrusive_ptr<vn::graphics::Light> light(new vn::graphics::Light());
    vn::math::Mat4d                                view_projection;
    view_projection.makeIdentity();
    view_projection.data[0] = 0.5;
    lender->setShadowOf(light);
    lender->setProducerViewProjection(view_projection);

    (void)targets.observe(*lender);  // a new announcement: the snapshot is taken again
    TargetFacts lender_row;
    targets.facts(*targets.find(lender.get()), lender_row);
    EXPECT_EQ(lender_row.shadow.light, static_cast<const void*>(light.get()));
    EXPECT_TRUE(lender_row.shadow.has_view_projection);
    EXPECT_TRUE(lender_row.shadow.view_projection.isEqual(view_projection));
    EXPECT_FALSE(lender_row.depth.promotion) << "the lender's own word: promotion was turned off";

    // What was not re-announced keeps the snapshot it had: the borrower is still no map.
    targets.facts(*ensured.entry, borrower_row);
    EXPECT_EQ(borrower_row.shadow.light, nullptr);
}
