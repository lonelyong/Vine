/**
 * @brief The per-frame target lease: one driver at a time, every claim released by the destructor (see TargetTable).
 *
 * WHY THIS FILE EXISTS. Step S5 of the sessions design is the rule "one target, one driver, one frame" - and it is a
 * lease before it is a wiring: the answers can all be pinned WITHOUT a GPU by naming two drivers with two ids. The
 * frame-level half (a lost claim skips exactly that pass and says so once) is `TargetClaimFrameTest`.
 *
 * THE WITNESSES ARE THE TABLE'S OWN BOOKS (`TargetTable::claimNumbers`): `claimed` / `refused` / `released` say which
 * path ran, and `live` says nothing was left behind. The claims are NOT a third table: the same object owns the map a
 * target is built into, so teardown clears both with one call (the last case here).
 */

#include <gtest/gtest.h>

#include <utility>

#include <vine/backend_vk/internal/TargetTable.hpp>

namespace
{

using vn::backend_vk::TargetTable;

constexpr TargetTable::FrameDriverId kFirst  = 11U;
constexpr TargetTable::FrameDriverId kSecond = 22U;

} // namespace

TEST(TargetTableTest, TheFirstDriverToClaimASharedTargetKeepsItForItsFrame)
{
    TargetTable table;
    int         target = 0;

    auto lease = table.lease(kFirst);
    EXPECT_TRUE(lease.claim(&target));
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_EQ(table.holderOf(&target), kFirst);

    const TargetTable::Numbers numbers = table.claimNumbers();
    EXPECT_EQ(numbers.claimed, 1U);
    EXPECT_EQ(numbers.live, 1U);
    EXPECT_EQ(numbers.refused, 0U);
}

TEST(TargetTableTest, ASecondDriverIsRefusedAndSkipsOnlyItsOwnPass)
{
    // C5/T5: the refusal is a LOCAL answer, not a lock: the winner keeps its claim and the loser learns it must skip
    // that one pass. Nothing waits and nothing is destroyed.
    TargetTable table;
    int         target = 0;

    auto first  = table.lease(kFirst);
    auto second = table.lease(kSecond);
    ASSERT_TRUE(first.claim(&target));

    EXPECT_FALSE(second.claim(&target)) << "two drivers claimed one target at the same time";
    EXPECT_EQ(second.size(), 0U) << "a refused claim was recorded as held";
    EXPECT_EQ(table.holderOf(&target), kFirst) << "the loser displaced the holder";

    EXPECT_EQ(table.claimNumbers().claimed, 1U);
    EXPECT_EQ(table.claimNumbers().refused, 1U);
    EXPECT_EQ(table.claimNumbers().live, 1U);
}

TEST(TargetTableTest, TheLeaseGivesEveryTargetBackWhenItDies)
{
    // P3: a frame's early returns all end at the lease's destructor. Losing that release would hold the target until
    // the process exits, with every other driver skipping its pass.
    TargetTable table;
    int         first_target  = 0;
    int         second_target = 0;

    {
        auto lease = table.lease(kFirst);
        ASSERT_TRUE(lease.claim(&first_target));
        ASSERT_TRUE(lease.claim(&second_target));
        ASSERT_EQ(lease.size(), 2U);
    }
    EXPECT_FALSE(table.holderOf(&first_target).has_value()) << "the dead lease kept a target";
    EXPECT_FALSE(table.holderOf(&second_target).has_value());
    EXPECT_EQ(table.claimNumbers().released, 2U);
    EXPECT_EQ(table.claimNumbers().live, 0U);

    // And the free targets are claimable by the other driver at once.
    auto other = table.lease(kSecond);
    EXPECT_TRUE(other.claim(&first_target));
}

TEST(TargetTableTest, ReleasingOneTargetEarlyLeavesTheOther)
{
    TargetTable table;
    int         early = 0;
    int         held  = 0;

    auto lease = table.lease(kFirst);
    ASSERT_TRUE(lease.claim(&early));
    ASSERT_TRUE(lease.claim(&held));

    lease.release(&early);
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_FALSE(table.holderOf(&early).has_value());
    EXPECT_EQ(table.holderOf(&held), kFirst);

    auto other = table.lease(kSecond);
    EXPECT_TRUE(other.claim(&early)) << "the early release did not free the target";
    EXPECT_FALSE(other.claim(&held)) << "the target still held was handed to a second driver";
}

TEST(TargetTableTest, TheHolderAskingAgainIsNotANewClaim)
{
    // One driver, one lease (see the class note); a repeat is the SAME claim, not a second one that could be
    // released twice.
    TargetTable table;
    int         target = 0;

    auto lease = table.lease(kFirst);
    ASSERT_TRUE(lease.claim(&target));
    EXPECT_TRUE(lease.claim(&target));
    EXPECT_EQ(lease.size(), 1U);
    EXPECT_EQ(table.claimNumbers().claimed, 1U) << "a repeat by the holder was counted as a new claim";
}

TEST(TargetTableTest, MovingALeaseMovesItsObligations)
{
    TargetTable table;
    int         target = 0;

    auto original = table.lease(kFirst);
    ASSERT_TRUE(original.claim(&target));

    TargetTable::Lease moved = std::move(original);
    EXPECT_EQ(original.size(), 0U) << "the moved-from lease still reports its claim";
    EXPECT_EQ(moved.size(), 1U);
    EXPECT_EQ(table.holderOf(&target), kFirst);

    moved = TargetTable::Lease{};
    EXPECT_FALSE(table.holderOf(&target).has_value()) << "assigning over a lease leaked its claim";
}

TEST(TargetTableTest, ClearingTheDeviceMakesLiveLeasesHarmless)
{
    // Teardown: the table goes with the device, and a lease still alive must not resurrect or free anything it no
    // longer owns.
    TargetTable table;
    int         target = 0;

    auto lease = table.lease(kFirst);
    ASSERT_TRUE(lease.claim(&target));

    table.close();
    EXPECT_FALSE(table.holderOf(&target).has_value());
    EXPECT_EQ(table.claimNumbers().live, 0U);

    auto other = table.lease(kSecond);
    EXPECT_TRUE(other.claim(&target));
    EXPECT_TRUE(table.holderOf(&target).has_value());

    // The stale lease's release must not free the NEW owner's claim.
    lease.release(&target);
    EXPECT_EQ(table.holderOf(&target), kSecond) << "a stale lease released another driver's claim";
}

TEST(TargetTableTest, TakingTheTablesRecordsAwayAlsoFreesTheirClaims)
{
    // ONE TABLE, ONE TEARDOWN (see TargetTable): `drain()` is the session ending, so it takes the records AND the
    // claims. A teardown that emptied the map but left the books saying "live" would make the next session's first
    // claim a conflict with a driver that no longer exists.
    TargetTable table;
    int         target = 0;

    auto lease = table.lease(kFirst);
    ASSERT_TRUE(lease.claim(&target));

    (void)table.drain();
    EXPECT_EQ(table.claimNumbers().live, 0U) << "drain left a claim behind";
    EXPECT_EQ(table.claimNumbers().released, 0U) << "teardown counted as a release";

    // The stale lease dies harmlessly: its target is no longer held, so nothing is given back a second time.
    lease = TargetTable::Lease{};
    EXPECT_EQ(table.claimNumbers().released, 0U);

    // And the table is usable again (a new session opens on it): the target is free.
    auto next = table.lease(kSecond);
    EXPECT_TRUE(next.claim(&target));
}
