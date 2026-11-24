/**
 * @brief `resolveAttachmentOps`'s per-attachment rule, tested with no device.
 *
 * What each case pins:
 *   - the GROUP spelling still answers every attachment (a pass that states nothing keeps working);
 *   - a stated per-attachment op overrides the group for ITS attachment only, which is the capability the per-role
 *     pair this replaced could not express;
 *   - `Load` on a set with no picture falls back to `Clear` (the same safety net the group rule had);
 *   - NO policy (`clear == nullopt`) clears nothing: a later pass loads the picture, and only a fresh set clears;
 *   - the depth entry is independent of the colour entries, and the clear values follow output-location order.
 *
 * The function is pure, so this file is a build-time witness: drift in the rule is caught here instead of by a pixel
 * that "looks fine".
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include <vine/backend_vk/internal/DynamicRendering.hpp>
#include <vine/graphics/DrawState.hpp>
#include <vine/graphics/FrameDescription.hpp>

using vn::backend_vk::PassAttachmentOps;
using vn::backend_vk::resolveAttachmentOps;
using vn::backend_vk::ResolvedAttachment;
using vn::graphics::AttachmentOps;
using vn::graphics::AttachmentPolicy;
using vn::graphics::LoadOp;
using vn::graphics::StoreOp;

namespace
{

constexpr std::size_t kColors = 3U;

/// @brief Calls the rule with the storage a recorder owns (see `resolveAttachmentOps`).
[[nodiscard]] PassAttachmentOps decide(const bool preserve, const AttachmentPolicy* policy, std::array<ResolvedAttachment, kColors>& storage)
{
    return resolveAttachmentOps(preserve, policy, storage, storage.size());
}

} // namespace

TEST(AttachmentOpsTest, TheGroupSpellingAnswersEveryAttachment)
{
    std::array<ResolvedAttachment, kColors> storage{};
    const AttachmentPolicy                  policy; // clears both dimensions, the ordinary frame start

    const PassAttachmentOps load = decide(true, &policy, storage);

    ASSERT_EQ(load.color.size(), kColors);
    for (const ResolvedAttachment& op : load.color) {
        EXPECT_EQ(op.load, LoadOp::Clear);
        EXPECT_EQ(op.store, StoreOp::Store);
    }
    EXPECT_EQ(load.depth.load, LoadOp::Clear);
    EXPECT_EQ(load.depth.store, StoreOp::Store);
}

TEST(AttachmentOpsTest, APerAttachmentOpOverridesItsOwnAttachment)
{
    std::array<ResolvedAttachment, kColors> storage{};
    AttachmentPolicy                        policy;
    policy.color_ops = {
        AttachmentOps{ LoadOp::Load,     StoreOp::Store    },
        AttachmentOps{ LoadOp::DontCare, StoreOp::DontCare }
    };

    const PassAttachmentOps load = decide(true, &policy, storage);

    ASSERT_EQ(load.color.size(), kColors);
    EXPECT_EQ(load.color[0].load, LoadOp::Load) << "attachment 0 keeps what it holds";
    EXPECT_EQ(load.color[0].store, StoreOp::Store);
    EXPECT_EQ(load.color[1].load, LoadOp::DontCare) << "attachment 1's contents are not worth reading";
    EXPECT_EQ(load.color[1].store, StoreOp::DontCare);
    EXPECT_EQ(load.color[2].load, LoadOp::Clear) << "an attachment the pass did not state follows the group";
    EXPECT_EQ(load.color[2].store, StoreOp::Store);
}

TEST(AttachmentOpsTest, NoAnnouncementKeepsThePicture)
{
    std::array<ResolvedAttachment, kColors> kept_storage{};
    std::array<ResolvedAttachment, kColors> fresh_storage{};

    // A PASS THAT ANNOUNCES NOTHING CLEARS NOTHING: a later pass into a set that already holds a picture loads it.
    const PassAttachmentOps kept = decide(true, nullptr, kept_storage);
    for (const ResolvedAttachment& op : kept.color) {
        EXPECT_EQ(op.load, LoadOp::Load) << "no policy is not a request to clear";
        EXPECT_EQ(op.store, StoreOp::Store);
    }
    EXPECT_EQ(kept.depth.load, LoadOp::Load);

    // ...but a FRESH set has nothing to keep, so the same announcement still answers Clear (the safety net).
    const PassAttachmentOps fresh = decide(false, nullptr, fresh_storage);
    for (const ResolvedAttachment& op : fresh.color) {
        EXPECT_EQ(op.load, LoadOp::Clear) << "an attachment with no picture cannot be loaded";
    }
    EXPECT_EQ(fresh.depth.load, LoadOp::Clear);
}

TEST(AttachmentOpsTest, AStatedLoadOnAFreshSetFallsBackToClear)
{
    std::array<ResolvedAttachment, kColors> storage{};
    AttachmentPolicy                        policy;
    policy.color_ops = {
        AttachmentOps{ LoadOp::Load, StoreOp::Store }
    };
    policy.depth_op = AttachmentOps{ LoadOp::Load, StoreOp::DontCare };

    const PassAttachmentOps load = decide(false, &policy, storage);

    EXPECT_EQ(load.color[0].load, LoadOp::Clear) << "there is no picture to keep, so Load cannot be honoured";
    EXPECT_EQ(load.depth.load, LoadOp::Clear) << "the depth gets the same safety net";
    EXPECT_EQ(load.depth.store, StoreOp::DontCare) << "the store is the pass' own and the fallback does not touch it";
}

TEST(AttachmentOpsTest, ADepthOpIsIndependentOfTheColours)
{
    std::array<ResolvedAttachment, kColors> storage{};
    AttachmentPolicy                        policy;
    policy.clear_color  = false; // the HUD case: keep the picture, own the depth
    policy.depth_op = AttachmentOps{ LoadOp::Clear, StoreOp::DontCare };

    const PassAttachmentOps load = decide(true, &policy, storage);

    for (const ResolvedAttachment& op : load.color) {
        EXPECT_EQ(op.load, LoadOp::Load) << "the colour group keeps what it holds";
    }
    EXPECT_EQ(load.depth.load, LoadOp::Clear) << "the depth op overrides the group on its own attachment";
    EXPECT_EQ(load.depth.store, StoreOp::DontCare);
}

TEST(AttachmentOpsTest, TheClearValuesFollowTheAttachmentOrder)
{
    std::array<ResolvedAttachment, kColors> storage{};
    AttachmentPolicy                        policy;
    policy.color        = vn::Colorf(0.25F, 0.5F, 0.75F, 1.0F);
    policy.extra_colors = { vn::Colorf(0.1F, 0.2F, 0.3F, 0.4F) };

    const PassAttachmentOps load = decide(false, &policy, storage);

    EXPECT_FLOAT_EQ(load.color[0].clear.color.float32[0], 0.25F);
    EXPECT_FLOAT_EQ(load.color[1].clear.color.float32[1], 0.2F);
    EXPECT_FLOAT_EQ(load.color[2].clear.color.float32[0], 0.0F) << "an unstated attachment is transparent black";
    EXPECT_FLOAT_EQ(load.depth.clear.depthStencil.depth, 0.0F) << "reverse-Z: the far plane is 0";
}
