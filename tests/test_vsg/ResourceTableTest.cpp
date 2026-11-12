/**
 * @brief The residency table's own contract: build on miss, keep while pinned, park on departure.
 *
 * `ResourceTable` is the one shape every residency kind migrates to (see the ADR's migration section). Its
 * contract is small enough to be pinned without a device, and it has three facts a caller depends on:
 *
 *   * a HIT builds nothing and returns the same value - the steady frame pays one lookup;
 *   * a REFUSED build leaves the table exactly as it was - a stream that may not be shared, a half that cannot
 *     compile, are not entries that "count" against anything;
 *   * a DEPARTURE is PARKED, never destroyed in place - and when the queue has no window yet the entry is kept
 *     and counted, because memory is cheaper than freeing something a submitted frame may still name.
 *
 * It is also where "one key, several generations" is pinned: a key built again while its previous value sits in
 * the parking window gets a fresh generation, and the old value stays alive until the queue's window is past.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>

#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/ResourceTable.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>

using vn::graphics::backend::forgetUnpinned;
using vn::graphics::backend::FrameTimeline;
using vn::graphics::backend::ResourceTable;
using vn::graphics::backend::RetirementQueue;

namespace
{

/// @brief A value that counts its live instances: "parked" and "destroyed" are different numbers.
struct Counted {
    int id{ 0 };

    Counted()
    {
        ++alive;
    }

    explicit Counted(int value)
      : id(value)
    {
        ++alive;
    }

    Counted(const Counted& other)
      : id(other.id)
    {
        ++alive;
    }

    Counted(Counted&& other) noexcept
      : id(other.id)
    {
        ++alive;
    }

    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;

    ~Counted()
    {
        --alive;
    }

    inline static int alive = 0;
};

/// @brief Lets a park made now come due (the queue's own window decides when that is).
void letParksComeDue(FrameTimeline& timeline, RetirementQueue& retirement)
{
    timeline.completeUpTo(FrameTimeline::retirePoint(timeline.submittedFrame(), retirement.slots()));
    retirement.advance(timeline);
}

} // namespace

TEST(ResourceTableTest, ASecondAcquireOfOneKeyBuildsNothing)
{
    ResourceTable<int, Counted> table;

    const auto first  = table.acquire(7, []() -> std::optional<Counted> { return Counted{ 7 }; });
    const auto second = table.acquire(7, []() -> std::optional<Counted> { return Counted{ 70 }; });

    ASSERT_NE(first.value, nullptr);
    EXPECT_TRUE(first.built);
    EXPECT_FALSE(second.built) << "a hit touches neither the builder nor the table";
    EXPECT_EQ(second.value, first.value) << "the SAME value serves both, which is what makes one upload one upload";
    EXPECT_EQ(second.generation, first.generation) << "one build, one generation";
    EXPECT_EQ(first.value->id, 7) << "the second call's builder must not have run";
    EXPECT_EQ(table.builds(), 1U);
    EXPECT_EQ(table.live(), 1U);
}

TEST(ResourceTableTest, ARefusedBuildChangesNothing)
{
    ResourceTable<int, Counted> table;

    const auto refused = table.acquire(1, []() -> std::optional<Counted> { return std::nullopt; });

    EXPECT_EQ(refused.value, nullptr);
    EXPECT_FALSE(refused.built);
    EXPECT_EQ(table.live(), 0U) << "a refusal is not an entry";
    EXPECT_EQ(table.builds(), 0U);
    EXPECT_EQ(table.retired(), 0U) << "and it is not a departure either";
}

TEST(ResourceTableTest, ADepartureIsParkedAndOnlyTheParkDestroysIt)
{
    ResourceTable<int, Counted> table;
    (void)table.acquire(3, []() -> std::optional<Counted> { return Counted{ 3 }; });
    const int alive_before = Counted::alive;

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);

    EXPECT_EQ(table.retireUnpinned([](int, const Counted&) { return false; }, timeline, retirement), 1U);
    EXPECT_EQ(table.live(), 0U) << "the lookup stops answering now";
    EXPECT_EQ(table.retired(), 1U);
    EXPECT_EQ(Counted::alive, alive_before) << "the value is parked, NOT destroyed: a submitted frame may still bind it";

    letParksComeDue(timeline, retirement);
    EXPECT_EQ(Counted::alive, alive_before - 1) << "the park came due and the value died then";
}

TEST(ResourceTableTest, AnEntryThatCannotBeParkedIsRetainedAndRetried)
{
    ResourceTable<int, Counted> table;
    (void)table.acquire(4, []() -> std::optional<Counted> { return Counted{ 4 }; });

    FrameTimeline   timeline;
    RetirementQueue retirement(0U); // the session has not learned its in-flight count: parking is OFF

    EXPECT_EQ(table.retireUnpinned([](int, const Counted&) { return false; }, timeline, retirement), 0U);
    EXPECT_EQ(table.live(), 1U) << "no window means keep, never free early";
    EXPECT_EQ(table.retained(), 1U);

    retirement.setSlots(1U); // the session learned it: the retry parks like every later one
    EXPECT_EQ(table.retireUnpinned([](int, const Counted&) { return false; }, timeline, retirement), 1U);
    EXPECT_EQ(table.live(), 0U);
    EXPECT_EQ(table.retired(), 1U);
}

TEST(ResourceTableTest, AKeyBuiltAgainWhileItsOldValueIsParkedGetsANewGeneration)
{
    ResourceTable<int, Counted> table;

    const auto first = table.acquire(9, []() -> std::optional<Counted> { return Counted{ 9 }; });
    ASSERT_TRUE(first.built);
    const int alive_before = Counted::alive;

    FrameTimeline   timeline;
    RetirementQueue retirement(1U);
    EXPECT_EQ(table.retireUnpinned([](int, const Counted&) { return false; }, timeline, retirement), 1U);
    EXPECT_EQ(table.live(), 0U);

    // The same key is wanted again while the old value is still inside the parking window.
    const auto second = table.acquire(9, []() -> std::optional<Counted> { return Counted{ 9 }; });
    ASSERT_TRUE(second.built) << "the parked value is not resurrected as a lookup: a new one is built";
    EXPECT_GT(second.generation, first.generation) << "a new build is a new generation";
    EXPECT_EQ(Counted::alive, alive_before + 1) << "the two generations COEXIST: one in the table, one in the queue";

    letParksComeDue(timeline, retirement);
    EXPECT_EQ(Counted::alive, alive_before) << "only the older generation left";
    EXPECT_EQ(table.live(), 1U) << "the current one is untouched";
}

TEST(ResourceTableTest, TheVisitSeesEveryResidentValueAndCanChangeItsBody)
{
    ResourceTable<int, Counted> table;
    (void)table.acquire(1, []() -> std::optional<Counted> { return Counted{ 1 }; });
    (void)table.acquire(2, []() -> std::optional<Counted> { return Counted{ 2 }; });

    int seen = 0;
    int sum  = 0;
    table.visitValues([&](Counted& value) {
        ++seen;
        value.id += 10; // changing the BODY is what a repoint does; an entry's identity is the table's
        sum += value.id;
    });

    EXPECT_EQ(seen, 2) << "one visit per resident value";
    EXPECT_EQ(sum, 11 + 12) << "the visitor saw the values the table held, and its change stuck";

    const ResourceTable<int, Counted>& const_table = table;
    int                               const_sum   = 0;
    const_table.visitValues([&](const Counted& value) { const_sum += value.id; });
    EXPECT_EQ(const_sum, sum) << "the const door sees the same values";
}

TEST(ResourceTableTest, ARefusedKeyIsForgottenByTheSamePinRuleAsAResidentValue)
{
    // The refused half of a residency rule (see forgetUnpinned): a remembered key has nothing built behind it, so it
    // leaves by the mirror of the table's rule - and the producers that keep such a set share this one implementation.
    std::unordered_set<int> refused{ 1, 2, 3 };
    EXPECT_EQ(forgetUnpinned(refused, [](int key) { return key < 3; }), 1U) << "only the key whose pin is gone leaves";
    EXPECT_EQ(refused.count(3), 0U);
    EXPECT_EQ(refused.size(), 2U);

    EXPECT_EQ(forgetUnpinned(refused, [](int) { return true; }), 0U) << "everything still pinned forgets nothing";
    EXPECT_EQ(refused.size(), 2U);
}

TEST(ResourceTableTest, ClearDropsWithoutParkingBecauseNothingIsInFlight)
{
    ResourceTable<int, Counted> table;
    (void)table.acquire(5, []() -> std::optional<Counted> { return Counted{ 5 }; });
    const int alive_before = Counted::alive;

    table.clear();

    EXPECT_EQ(table.live(), 0U);
    EXPECT_EQ(Counted::alive, alive_before - 1) << "the session-teardown path destroys in place: no frame is in flight";
}
