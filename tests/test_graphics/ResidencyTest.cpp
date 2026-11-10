#include <vine/graphics/Residency.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using vn::graphics::ResidencyKind;
using vn::graphics::ResidencyLedger;
using vn::graphics::ResidencyRow;

/**
 * @brief The residency ledger is a VOCABULARY: these cases pin the two rules a reader depends on - a number
 *        nobody keeps stays absent, and a total is only given when every row reported it.
 *
 * The device half (which backend fills which row) is a rendering backend's own evidence and is tested where the
 * device is (see `.ai/decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`, migration step 1).
 */
TEST(ResidencyLedgerTest, AnEmptyLedgerSaysNothingRatherThanZero)
{
    const ResidencyLedger ledger;

    EXPECT_TRUE(ledger.rows().empty());
    const vn::graphics::ResidencyTotals totals = ledger.totals();
    EXPECT_EQ(totals.live, 0U);
    EXPECT_FALSE(totals.bytes.has_value()) << "an empty ledger did not measure a total, it has none";
    EXPECT_EQ(totals.unknown, 0U);
}

TEST(ResidencyLedgerTest, AKindThatDoesNotCountBytesKeepsTheTotalSilent)
{
    ResidencyLedger ledger;
    // The stream store counts entries and churn but not bytes (see VsgBackend::frameReport); the image cache
    // counts bytes. Adding them would produce a number nobody measured.
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Streams, .live = 7U, .built = 7U, .retired = 0U, .refused = 0U });
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Images, .live = 2U, .bytes = 4096U });

    const vn::graphics::ResidencyTotals totals = ledger.totals();
    EXPECT_EQ(totals.live, 9U);
    EXPECT_FALSE(totals.bytes.has_value()) << "one silent row is enough to make the total a guess";
    EXPECT_EQ(totals.unknown, 1U) << "and the answer says how many kinds are missing";
}

TEST(ResidencyLedgerTest, ATotalIsGivenOnlyWhenEveryRowReportedItsBytes)
{
    ResidencyLedger ledger;
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Images, .live = 2U, .bytes = 4096U });
    ledger.note(ResidencyRow{ .kind = ResidencyKind::MaterialBlocks, .live = 3U, .bytes = 1536U });

    const vn::graphics::ResidencyTotals totals = ledger.totals();
    ASSERT_TRUE(totals.bytes.has_value());
    EXPECT_EQ(*totals.bytes, 4096U + 1536U);
    EXPECT_EQ(totals.live, 5U);
    EXPECT_EQ(totals.unknown, 0U);
}

TEST(ResidencyLedgerTest, ABytesReadingThatDropsIsWhatMakesItUsableAsATrimSignal)
{
    ResidencyLedger ledger;
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Images, .live = 3U, .bytes = 9000U });
    ASSERT_EQ(*ledger.totals().bytes, 9000U);

    // The same kind, one entry released: the row is REPLACED, not accumulated (one fact, one row).
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Images, .live = 2U, .bytes = 6000U });
    EXPECT_EQ(ledger.rows().size(), 1U);
    EXPECT_EQ(*ledger.totals().bytes, 6000U) << "the ledger reports what is resident now, not a running total";
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
    ledger.note(ResidencyRow{ .kind = ResidencyKind::Streams, .live = 4U });
    ASSERT_EQ(ledger.rows().size(), 1U);

    ledger.reset();
    EXPECT_TRUE(ledger.rows().empty());
    EXPECT_FALSE(ledger.totals().bytes.has_value());
}
