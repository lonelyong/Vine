/**
 * @brief The per-frame target lease: one driver at a time, every claim released by the destructor (see TargetClaims).
 *
 * WHY THIS FILE EXISTS. Step S5 of the sessions design is the rule "one target, one driver, one frame" - and it is a
 * lease before it is a wiring: the answers can all be pinned WITHOUT a GPU by naming two sessions with two ids. The
 * frame-level half (a lost claim skips exactly that pass and says so once) is `TargetClaimFrameTest`.
 *
 * THE WITNESSES ARE THE TABLE'S OWN BOOKS (`TargetClaims::numbers`): `claimed` / `refused` / `released` say which path
 * ran, and `live` says nothing was left behind.
 */

#include <gtest/gtest.h>

#include <utility>

#include <vine/backend_vk/internal/TargetClaims.hpp>

namespace
{

using vn::backend_vk::TargetClaims;

constexpr TargetClaims::SessionId kFirst  = 11U;
constexpr TargetClaims::SessionId kSecond = 22U;

} // namespace

TEST(TargetClaimsTest, TheFirstDriverToClaimASharedTargetKeepsItForItsFrame)
{
    TargetClaims claims;
    int          target = 0;

    auto lease = claims.begin(kFirst);
    EXPECT_TRUE(lease.claim(&target));
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_EQ(claims.holderOf(&target), kFirst);

    const TargetClaims::Numbers numbers = claims.numbers();
    EXPECT_EQ(numbers.claimed, 1U);
    EXPECT_EQ(numbers.live, 1U);
    EXPECT_EQ(numbers.refused, 0U);
}

TEST(TargetClaimsTest, ASecondDriverIsRefusedAndSkipsOnlyItsOwnPass)
{
    // C5/T5: the refusal is a LOCAL answer, not a lock: the winner keeps its claim and the loser learns it must skip
    // that one pass. Nothing waits and nothing is destroyed.
    TargetClaims claims;
    int          target = 0;

    auto first  = claims.begin(kFirst);
    auto second = claims.begin(kSecond);
    ASSERT_TRUE(first.claim(&target));

    EXPECT_FALSE(second.claim(&target)) << "two drivers claimed one target at the same time";
    EXPECT_EQ(second.size(), 0U) << "a refused claim was recorded as held";
    EXPECT_EQ(claims.holderOf(&target), kFirst) << "the loser displaced the holder";

    EXPECT_EQ(claims.numbers().claimed, 1U);
    EXPECT_EQ(claims.numbers().refused, 1U);
    EXPECT_EQ(claims.numbers().live, 1U);
}

TEST(TargetClaimsTest, TheLeaseGivesEveryTargetBackWhenItDies)
{
    // P3: a frame's early returns all end at the lease's destructor. Losing that release would hold the target until
    // the process exits, with every other driver skipping its pass.
    TargetClaims claims;
    int          first_target  = 0;
    int          second_target = 0;

    {
        auto lease = claims.begin(kFirst);
        ASSERT_TRUE(lease.claim(&first_target));
        ASSERT_TRUE(lease.claim(&second_target));
        ASSERT_EQ(lease.size(), 2U);
    }
    EXPECT_FALSE(claims.holderOf(&first_target).has_value()) << "the dead lease kept a target";
    EXPECT_FALSE(claims.holderOf(&second_target).has_value());
    EXPECT_EQ(claims.numbers().released, 2U);
    EXPECT_EQ(claims.numbers().live, 0U);

    // And the free targets are claimable by the other driver at once.
    auto other = claims.begin(kSecond);
    EXPECT_TRUE(other.claim(&first_target));
}

TEST(TargetClaimsTest, ReleasingOneTargetEarlyLeavesTheOther)
{
    TargetClaims claims;
    int          early  = 0;
    int          held   = 0;

    auto lease = claims.begin(kFirst);
    ASSERT_TRUE(lease.claim(&early));
    ASSERT_TRUE(lease.claim(&held));

    lease.release(&early);
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_FALSE(claims.holderOf(&early).has_value());
    EXPECT_EQ(claims.holderOf(&held), kFirst);

    auto other = claims.begin(kSecond);
    EXPECT_TRUE(other.claim(&early)) << "the early release did not free the target";
    EXPECT_FALSE(other.claim(&held)) << "the target still held was handed to a second driver";
}

TEST(TargetClaimsTest, TheHolderAskingAgainIsNotANewClaim)
{
    // One session, one lease (see the class note); a repeat is the SAME claim, not a second one that could be
    // released twice.
    TargetClaims claims;
    int          target = 0;

    auto lease = claims.begin(kFirst);
    ASSERT_TRUE(lease.claim(&target));
    EXPECT_TRUE(lease.claim(&target));
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_EQ(claims.numbers().claimed, 1U) << "a repeat by the holder was counted as a new claim";
}

TEST(TargetClaimsTest, MovingALeaseMovesItsObligations)
{
    TargetClaims claims;
    int          target = 0;

    auto original = claims.begin(kFirst);
    ASSERT_TRUE(original.claim(&target));

    TargetClaims::Lease moved = std::move(original);
    EXPECT_EQ(original.size(), 0U) << "the moved-from lease still reports its claim";
    EXPECT_EQ(moved.size(), 1U);
    EXPECT_EQ(claims.holderOf(&target), kFirst);

    moved = TargetClaims::Lease{};
    EXPECT_FALSE(claims.holderOf(&target).has_value()) << "assigning over a lease leaked its claim";
}

TEST(TargetClaimsTest, ClearingTheDeviceMakesLiveLeasesHarmless)
{
    // Teardown: the table goes with the device, and a lease still alive must not resurrect or free anything it no
    // longer owns.
    TargetClaims claims;
    int          target = 0;

    auto lease = claims.begin(kFirst);
    ASSERT_TRUE(lease.claim(&target));

    claims.clear();
    EXPECT_FALSE(claims.holderOf(&target).has_value());
    EXPECT_EQ(claims.numbers().live, 0U);

    auto other = claims.begin(kSecond);
    EXPECT_TRUE(other.claim(&target));
    EXPECT_TRUE(claims.holderOf(&target).has_value());

    // The stale lease's release must not free the NEW owner's claim.
    lease.release(&target);
    EXPECT_EQ(claims.holderOf(&target), kSecond) << "a stale lease released another session's claim";
}
