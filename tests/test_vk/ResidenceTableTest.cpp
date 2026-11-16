/**
 * @brief The shared residency state machine: one publisher per key, publish-or-withdraw, and a copy instead of a wait
 *        (see ResidenceTable and `.ai/design/graphics-sessions.md` §2.4).
 *
 * WHY THIS FILE EXISTS. Step S4 of the sessions design is a state machine before it is a wiring: the rules it must
 * make true - exactly one driver builds a key, a second driver copies instead of waiting, a claim that leaves the
 * frame publishes or withdraws but never lingers - can all be pinned WITHOUT a GPU, by naming two "sessions" with two
 * ids. The device-level wiring lands with `createSession()` (S6); these cases are what it will lean on.
 *
 * THE WITNESSES ARE THE TABLE'S OWN BOOKS (`ResidenceTable::numbers`), not a test-side guess about what should have
 * happened: `claims` / `published` / `withdrawn` / `copies` / `stale` say which path ran, and `uploading` says nothing
 * was left behind.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <vine/backend_vk/internal/ResidenceTable.hpp>

namespace
{

using vn::backend_vk::ResidenceTable;

/// Two driver ids stand in for two sessions: the table only ever compares them.
constexpr ResidenceTable::FrameDriverId kFirst  = 11U;
constexpr ResidenceTable::FrameDriverId kSecond = 22U;

} // namespace

TEST(ResidenceTableTest, TheFirstDriverToAskOwnsTheKeyAndPublishingMakesItUsable)
{
    ResidenceTable table;
    int            content = 0;

    auto first = table.admit(&content, kFirst);
    EXPECT_EQ(first.kind, ResidenceTable::Decision::Kind::Build);
    ASSERT_TRUE(static_cast<bool>(first.claim)) << "a Build decision handed out no claim";
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Uploading);

    EXPECT_TRUE(first.claim.publish()) << "the owner's token was refused";
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Resident);

    auto second = table.admit(&content, kSecond);
    EXPECT_EQ(second.kind, ResidenceTable::Decision::Kind::Use) << "a published key was not usable by the other driver";
    EXPECT_FALSE(static_cast<bool>(second.claim));

    const ResidenceTable::Numbers numbers = table.numbers();
    EXPECT_EQ(numbers.claims, 1U);
    EXPECT_EQ(numbers.published, 1U);
    EXPECT_EQ(numbers.uploading, 0U);
    EXPECT_EQ(numbers.resident, 1U);
}

TEST(ResidenceTableTest, ADriverThatSeesAnUploadInFlightBuildsACopyInsteadOfWaiting)
{
    // P2: waiting is the only shape that can deadlock two drivers, so the loser is told to copy - and copying does
    // NOT reserve the key (the entry keeps its single owner).
    ResidenceTable table;
    int            content = 0;

    auto owner = table.admit(&content, kFirst);
    ASSERT_EQ(owner.kind, ResidenceTable::Decision::Kind::Build);

    auto competitor = table.admit(&content, kSecond);
    EXPECT_EQ(competitor.kind, ResidenceTable::Decision::Kind::Copy);
    EXPECT_EQ(competitor.owner, kFirst) << "the copy did not name the driver filling the key";
    EXPECT_FALSE(static_cast<bool>(competitor.claim)) << "a competitor was handed a publishing token";

    // The competitor's temporary copy is its own business and leaves the table alone: the owner's token still works.
    EXPECT_TRUE(owner.claim.publish());
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Resident);

    auto afterwards = table.admit(&content, kSecond);
    EXPECT_EQ(afterwards.kind, ResidenceTable::Decision::Kind::Use);
    EXPECT_EQ(table.numbers().copies, 1U);
}

TEST(ResidenceTableTest, AnAbandonedClaimWithdrawsItselfWhenTheGuardDies)
{
    // P1: an entry stuck in Uploading makes EVERY driver miss that key forever. The guard is the prevention: an early
    // return that never reaches publish() still releases the key.
    ResidenceTable table;
    int            content = 0;

    {
        auto attempt = table.admit(&content, kFirst);
        ASSERT_EQ(attempt.kind, ResidenceTable::Decision::Kind::Build);
        EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Uploading);
    } // the claim dies unresolved: it must withdraw

    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Absent) << "the abandoned claim left the key Uploading";
    EXPECT_EQ(table.numbers().uploading, 0U);
    EXPECT_EQ(table.numbers().withdrawn, 1U);

    // And the key is buildable again by anybody - including the driver that gave up.
    auto retry = table.admit(&content, kSecond);
    EXPECT_EQ(retry.kind, ResidenceTable::Decision::Kind::Build);
}

TEST(ResidenceTableTest, AStaleTokenCannotPublishOverANewerClaim)
{
    // RULE 1 FROM THE OTHER SIDE: the token, not the caller's memory, decides who may publish. A key released while
    // its fill was in flight and then re-claimed leaves the first claim holding a number nobody honours.
    ResidenceTable table;
    int            content = 0;

    auto first = table.admit(&content, kFirst);
    ASSERT_EQ(first.kind, ResidenceTable::Decision::Kind::Build);
    ASSERT_TRUE(table.forget(&content)); // the host released the content mid-upload

    auto second = table.admit(&content, kSecond);
    ASSERT_EQ(second.kind, ResidenceTable::Decision::Kind::Build);

    EXPECT_FALSE(first.claim.publish()) << "a stale token published over a newer claim";
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Uploading) << "the newer claim was disturbed";
    EXPECT_TRUE(second.claim.publish());
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Resident);
    EXPECT_EQ(table.numbers().stale, 1U);
    EXPECT_EQ(table.numbers().claims, 2U);
    EXPECT_EQ(table.numbers().published, 1U);
}

TEST(ResidenceTableTest, ReleasingAKeyForgetsItAtOnceEvenMidUpload)
{
    // `forget` is what `releaseContent` does: the key leaves NOW (residency answers the question it is asked), and the
    // objects behind it are the caller's to park. A fill that finishes afterwards cannot bring the entry back.
    ResidenceTable table;
    int            content = 0;

    auto owner = table.admit(&content, kFirst);
    ASSERT_EQ(owner.kind, ResidenceTable::Decision::Kind::Build);

    EXPECT_TRUE(table.forget(&content));
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Absent);
    EXPECT_FALSE(owner.claim.publish()) << "a released key was resurrected by its in-flight fill";
    EXPECT_FALSE(table.forget(&content)) << "a second release reported an entry that was already gone";
    EXPECT_EQ(table.numbers().released, 1U);
    EXPECT_EQ(table.numbers().uploading, 0U);
}

TEST(ResidenceTableTest, TheUploadingCountReturnsToZeroOnEveryPath)
{
    // R4: an Uploading entry must not exist between frames. Every way out of a claim is exercised here, and the count
    // is the single witness that none of them leaves one behind.
    ResidenceTable table;
    int            content = 0;

    {
        auto published = table.admit(&content, kFirst);
        ASSERT_EQ(published.kind, ResidenceTable::Decision::Kind::Build);
        EXPECT_TRUE(published.claim.publish());
    }
    EXPECT_EQ(table.numbers().uploading, 0U);
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Resident);

    ASSERT_TRUE(table.forget(&content));
    {
        auto withdrawn = table.admit(&content, kFirst);
        ASSERT_EQ(withdrawn.kind, ResidenceTable::Decision::Kind::Build);
        withdrawn.claim.withdraw();
        EXPECT_FALSE(static_cast<bool>(withdrawn.claim));
    }
    EXPECT_EQ(table.numbers().uploading, 0U);

    {
        auto abandoned = table.admit(&content, kFirst);
        ASSERT_EQ(abandoned.kind, ResidenceTable::Decision::Kind::Build);
    }
    EXPECT_EQ(table.numbers().uploading, 0U);

    {
        auto released_mid_flight = table.admit(&content, kFirst);
        ASSERT_EQ(released_mid_flight.kind, ResidenceTable::Decision::Kind::Build);
        ASSERT_TRUE(table.forget(&content));
    }
    EXPECT_EQ(table.numbers().uploading, 0U);
    EXPECT_EQ(table.numbers().withdrawn, 2U)
        << "the explicit withdrawal and the abandoned claim are the two that withdrew; "
           "the already-forgotten key is not one of them";
}

TEST(ResidenceTableTest, ClearingTheTableMakesEveryOutstandingClaimStale)
{
    // Teardown: the table goes away with the device, and a claim that was still open must not publish into the next
    // session's table.
    ResidenceTable table;
    int            content = 0;

    auto owner = table.admit(&content, kFirst);
    ASSERT_EQ(owner.kind, ResidenceTable::Decision::Kind::Build);

    table.clear();
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Absent);
    EXPECT_FALSE(owner.claim.publish());
    EXPECT_EQ(table.numbers().stale, 1U);
}

TEST(ResidenceTableTest, OnlyOneOfManyRacingDriversWinsTheKey)
{
    // THE COMPARE-AND-SET ITSELF. Every driver asks before anybody publishes, so the answer is deterministic in
    // OUTCOME (one Build, the rest Copy) even though the order is not.
    ResidenceTable table;
    int            content = 0;

    constexpr int                                kDrivers = 8;
    std::vector<std::optional<ResidenceTable::Decision>> decisions(kDrivers);
    std::atomic<int>                             arrived{ 0 };
    std::atomic<bool>                            published{ false };
    std::vector<std::thread>                     drivers;
    drivers.reserve(kDrivers);

    for (int index = 0; index < kDrivers; ++index) {
        drivers.emplace_back([&table, &content, &decisions, &arrived, &published, index]() {
            decisions[static_cast<std::size_t>(index)].emplace(
                table.admit(&content, static_cast<ResidenceTable::FrameDriverId>(index + 1U)));
            arrived.fetch_add(1);
            while (!published.load()) {
                std::this_thread::yield();
            }
        });
    }
    while (arrived.load() != kDrivers) {
        std::this_thread::yield();
    }
    published.store(true);
    for (std::thread& driver : drivers) {
        driver.join();
    }

    int builds = 0;
    int copies = 0;
    int owner  = -1;
    for (std::size_t index = 0; index < decisions.size(); ++index) {
        ASSERT_TRUE(decisions[index].has_value());
        if (decisions[index]->kind == ResidenceTable::Decision::Kind::Build) {
            ++builds;
            owner = static_cast<int>(index);
        }
        else {
            EXPECT_EQ(decisions[index]->kind, ResidenceTable::Decision::Kind::Copy);
            ++copies;
        }
    }
    EXPECT_EQ(builds, 1) << "more than one driver was told to build the same key";
    EXPECT_EQ(copies, kDrivers - 1);

    ASSERT_GE(owner, 0);
    EXPECT_TRUE(decisions[static_cast<std::size_t>(owner)]->claim.publish());
    EXPECT_EQ(table.stateOf(&content), ResidenceTable::State::Resident);
}
