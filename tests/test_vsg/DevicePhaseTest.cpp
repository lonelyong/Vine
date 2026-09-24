/**
 * @brief The DEVICE phases as a phase table: the rewrite's real-device capabilities in the `[selftest]`
 *        evidence format the gate reads.
 *
 * WHY A SECOND TABLE. The plan-side phases (`BackendEvidenceTest`) gate the frame path that needs no
 * device; these gate the capabilities that need one. They are the SAME bodies the detailed cases run
 * (`DevicePhases.hpp`): a capability whose case grows an assertion the phase does not run, or a phase
 * row whose capability has no body, is impossible by construction - which is the whole reason the bodies
 * are functions rather than inline test code.
 *
 * ONE DEVICE FOR ALL FOUR. The phases are independent capabilities, run in order on one device: the run
 * costs one device instead of four, and a phase that leaked a device-level object would show up in the
 * next one's results.
 *
 * WHAT EACH ROW GATES ON. The counter, not the name: "the shared-depth phase built exactly two targets",
 * "the resize phase replaced exactly one extent", "the lost-submission phase drove exactly three frames".
 * A row that only said "it ran" would pass while the capability quietly did nothing - the failure mode the
 * counter column exists for.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/core/PhaseTable.hpp>

#include "DevicePhases.hpp"

using vn::vsg::createDevice;
using vn::vsg::DeviceOptions;
using vn::vsg::DeviceResult;
using vn::vsg::core::Phase;
using vn::vsg::core::PhaseTable;

namespace
{

/// @brief Runs @p phase with the table's before/after counter window, and reports whether it passed.
bool runPhase(void (*phase)(const DeviceResult&, DevicePhaseCounters&), const DeviceResult& device,
              DevicePhaseCounters& counters)
{
    phase(device, counters);
    return !::testing::Test::HasFailure();
}

}  // namespace

TEST(DevicePhaseTest, TheRealDeviceCapabilitiesArePhasesWithCounterExpectations)
{
    DeviceOptions options;
    options.validation = true;  // the phases are only evidence with the layers looking
    const auto device  = createDevice(options);
    if (!device.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    DevicePhaseCounters counters;
    PhaseTable          table;
    table.add(Phase{
        "offscreen readback: the clear colour is what comes back",
        [&]() { return runPhase(runOffscreenReadbackPhase, device, counters); },
        [&]() { return counters.targets_built; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 1U; },
    });
    table.add(Phase{
        "shared depth: the borrower draws against the lender's depth and reads it back",
        [&]() { return runPhase(runSharedDepthPhase, device, counters); },
        [&]() { return counters.targets_built; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 2U; },
    });
    table.add(Phase{
        "target resize: the plan replaces the extent, the old set is parked, the new one renders",
        [&]() { return runPhase(runTargetResizePhase, device, counters); },
        [&]() { return counters.resizes_replaced; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 1U; },
    });
    table.add(Phase{
        "target rebuild: the shape changes, the pass and the attachments are rebuilt, the new shape renders",
        [&]() { return runPhase(runTargetRebuildPhase, device, counters); },
        [&]() { return counters.rebuilds_replaced; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 1U; },
    });
    table.add(Phase{
        "plan-driven targets: the executor applies the plan's resize and rebuild answers",
        [&]() { return runPhase(runPlanDrivenTargetPhase, device, counters); },
        [&]() { return counters.plan_applied; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 2U; },
    });
    table.add(Phase{
        "leased targets: the plan's answers are applied lender first, so a pair that grows together moves "
        "together",
        [&]() { return runPhase(runLeasedTargetOrderPhase, device, counters); },
        [&]() { return counters.resizes_replaced; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 2U; },
    });
    table.add(Phase{
        "lost submission: repaired by the next frame, and only once",
        [&]() { return runPhase(runLostSubmissionPhase, device, counters); },
        [&]() { return counters.frames; },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 3U; },
    });

    const PhaseTable::Report report = table.runAll();
    for (const std::string& line : report.lines) {
        std::cout << line << '\n';  // the evidence lines the gate reads
    }

    EXPECT_TRUE(report.ok()) << "every device phase has to pass before this run claims anything";
    EXPECT_EQ(report.passed, 7U);
    EXPECT_EQ(report.failed, 0U);

    const std::vector<std::string> baseline{
        "[selftest] offscreen readback: the clear colour is what comes back",
        "[selftest] shared depth: the borrower draws against the lender's depth and reads it back",
        "[selftest] target resize: the plan replaces the extent, the old set is parked, the new one renders",
        "[selftest] target rebuild: the shape changes, the pass and the attachments are rebuilt, the new shape renders",
        "[selftest] plan-driven targets: the executor applies the plan's resize and rebuild answers",
        "[selftest] leased targets: the plan's answers are applied lender first, so a pair that grows together moves "
        "together",
        "[selftest] lost submission: repaired by the next frame, and only once",
        "[selftest] done",
    };
    EXPECT_EQ(report.lines, baseline) << "the phase list IS the capability list: a diff here is a device "
                                         "capability that appeared, vanished or changed name";

    // The same counters the rows gated on, read once more: a row that passed while its phase did nothing
    // would have to have moved nothing, and these numbers are what the phase drove in total.
    EXPECT_EQ(counters.frames, 14U)
        << "readback 1 + shared depth 1 + resize 2 + rebuild 2 + drive 3 + leased pair 2 + lost submission 3";
    EXPECT_EQ(counters.targets_built, 10U)
        << "readback 1 + shared depth 2 + resize 1 + rebuild 1 + drive 2 + leased pair 2 + lost submission 1";
    EXPECT_EQ(counters.resizes_replaced, 4U) << "the resize phase's one, the drive's one, and the leased pair's two";
    EXPECT_EQ(counters.rebuilds_replaced, 2U);
    EXPECT_EQ(counters.plan_applied, 4U) << "one resize answer, one rebuild answer, and the leased pair's two";
    EXPECT_EQ(counters.parked, 6U);
}
