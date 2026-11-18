#include <vine/graphics/Residency.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using vn::graphics::ResidencyKind;
using vn::graphics::ResidencyLedger;
using vn::graphics::ResidencyRow;

/**
 * @brief The residency ledger is a VOCABULARY: these cases pin the rules a reader depends on - a kind is REPLACED,
 *        not accumulated, a number nobody keeps stays absent, and the only figure the ledger adds is the COUNT.
 *
 * The device half (which backend fills which row) is a rendering backend's own evidence and is tested where the
 * device is (see `.ai/decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`, migration step 1).
 */
TEST(ResidencyLedgerTest, AnEmptyLedgerSaysNothingRatherThanZero)
{
    const ResidencyLedger ledger;

    EXPECT_TRUE(ledger.rows().empty());
    EXPECT_EQ(ledger.live(), 0U) << "nothing is resident, and the count is 0 rather than unknown";
}

TEST(ResidencyLedgerTest, ARowThatDoesNotCountBytesSaysSoRatherThanReportingZero)
{
    ResidencyLedger ledger;
    // No GPU API reports a pipeline's bytes (see `ResidencyKind::Programs`), so this row keeps a count only.
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Programs, .live = 3U });

    ASSERT_EQ(ledger.rows().size(), 1U);
    EXPECT_FALSE(ledger.rows().front().bytes.has_value()) << "absent is not zero: the backend counted entries, not bytes";
    EXPECT_EQ(ledger.live(), 3U) << "the COUNT is always available, which is why it is the only figure added up";
}

TEST(ResidencyLedgerTest, ANoteReplacesItsKindInsteadOfAccumulating)
{
    ResidencyLedger ledger;
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Textures, .live = 3U, .bytes = 9000U });
    ASSERT_EQ(ledger.live(), 3U);

    // The same kind, one entry released: the row is REPLACED, not accumulated (one fact, one row).
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Textures, .live = 2U, .bytes = 6000U });
    ASSERT_EQ(ledger.rows().size(), 1U);
    EXPECT_EQ(*ledger.rows().front().bytes, 6000U)
        << "the ledger reports what is resident now, not a running total - a release has to make it go DOWN";
    EXPECT_EQ(ledger.live(), 2U);
}

TEST(ResidencyLedgerTest, EveryKindHasItsOwnStableName)
{
    std::vector<std::string> names;
    for (std::size_t index = 0; index < vn::graphics::kResidencyKindCount; ++index) {
        const ResidencyKind kind = static_cast<ResidencyKind>(index);
        const char*         name = ResidencyLedger::nameOf(kind);
        ASSERT_NE(name, nullptr);
        ASSERT_FALSE(std::string(name).empty());
        names.emplace_back(name);
    }
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1U; right < names.size(); ++right) {
            EXPECT_NE(names[left], names[right]) << "two kinds sharing a name would make a report unreadable";
        }
    }
}

TEST(ResidencyLedgerTest, ResetForgetsTheRowsAndKeepsTheObject)
{
    ResidencyLedger ledger;
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Geometry, .live = 4U });
    ASSERT_EQ(ledger.rows().size(), 1U);
    ASSERT_EQ(ledger.live(), 4U);

    ledger.reset();
    EXPECT_TRUE(ledger.rows().empty());
    EXPECT_EQ(ledger.live(), 0U);
}
