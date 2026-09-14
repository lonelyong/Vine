/**
 * @brief The plugin's diagnostic route (design §50, device-free).
 *
 * VsgDiagnostics is the one place a failing path reports through: it traces the report on
 * stderr and hands it to the host's channel. The SDK already owns the host-facing sink and
 * the counters, so what these cases pin is the ROUTE — a report reaches the installed
 * downstream unchanged, a module's reports (a bridge's) go through the same route, and a route
 * with no downstream still accepts one (the trace is then the only listener; the harness
 * assertions cover that half).
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRenderer.hpp>
#include <vine/vsg/VsgUtils.hpp>

using vine::graphics::DiagnosticCategory;
using vine::graphics::DiagnosticSeverity;
using vine::graphics::RenderDiagnostic;
using vine::vsg::formatDiagnostic;
using vine::vsg::VsgDiagnostics;

namespace
{
/// Records what a route delivers, so a case can assert on the exact report.
struct Captured
{
    std::vector<RenderDiagnostic> items;
};
} // namespace

TEST(VsgDiagnostics, ReportReachesTheInstalledDownstream)
{
    VsgDiagnostics diagnostics;
    Captured captured;
    diagnostics.setDownstream([&](const RenderDiagnostic& diagnostic) { captured.items.push_back(diagnostic); });

    const vine::String message = formatDiagnostic(u8"content skipped: %d channels", 3);
    diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::ContentSkipped, message);

    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::ContentSkipped);
    EXPECT_EQ(captured.items[0].message.stdstr(), message.stdstr());
}

TEST(VsgDiagnostics, ReportWithoutADownstreamIsAccepted)
{
    VsgDiagnostics diagnostics;

    // The trace is the fallback listener, so a report with nobody subscribed must still be a
    // no-op rather than a crash: a host that installs no sink loses only the programmatic
    // channel (see RenderBackend's contract).
    diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::InitFailed, formatDiagnostic(u8"no sink installed"));
}

TEST(VsgDiagnostics, RouteForwardsAModulesReportsUnchanged)
{
    VsgDiagnostics diagnostics;
    Captured captured;
    diagnostics.setDownstream([&](const RenderDiagnostic& diagnostic) { captured.items.push_back(diagnostic); });

    const vine::graphics::DiagnosticSink module_sink = diagnostics.route();
    ASSERT_TRUE(static_cast<bool>(module_sink));
    module_sink(RenderDiagnostic{ DiagnosticSeverity::Error, DiagnosticCategory::TargetBuildFailed,
                                  formatDiagnostic(u8"a bridge's report: %s", "no depth") });

    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Error);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::TargetBuildFailed);
    EXPECT_EQ(captured.items[0].message.stdstr(), std::string("a bridge's report: no depth"));
}

TEST(VsgDiagnostics, ReplacingTheDownstreamStopsDeliveringToTheOldOne)
{
    VsgDiagnostics diagnostics;
    Captured first;
    Captured second;
    diagnostics.setDownstream([&](const RenderDiagnostic& diagnostic) { first.items.push_back(diagnostic); });

    diagnostics.setDownstream([&](const RenderDiagnostic& diagnostic) { second.items.push_back(diagnostic); });
    diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::ShaderFallback,
                       formatDiagnostic(u8"fallback"));

    EXPECT_TRUE(first.items.empty());
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_EQ(second.items[0].category, DiagnosticCategory::ShaderFallback);
}

/**
 * @brief The retention picture a session can be asked for (VsgRetentionStats).
 *
 * A backend retains things past the frame that built them, and the pieces are only meaningful read
 * together (a pool whose capacity climbs while its reserved count does not is a leak; compile
 * contexts that outnumber the slots that registered them are retention vsg cannot release). This
 * pins the API: a fresh renderer reports zero everywhere rather than garbage, every field is wired
 * to a real counter, and the picture is readable without a device (it is counters, not Vulkan).
 */
TEST(DiagnosticsTest, AFreshSessionReportsAnEmptyRetentionPicture)
{
    vine::vsg::VsgRenderer renderer;

    const vine::vsg::VsgRetentionStats stats = renderer.retentionStats();
    EXPECT_EQ(stats.content_slots, 0u);
    EXPECT_EQ(stats.slots.chunks, 0u) << "no device: the pool allocated nothing";
    EXPECT_EQ(stats.slots.capacity, 0u);
    EXPECT_EQ(stats.slots.reserved, 0u);
    EXPECT_EQ(stats.slots.retired, 0u);
    EXPECT_EQ(stats.parked_nodes, 0u);
    EXPECT_EQ(stats.released_nodes, 0u);
    EXPECT_EQ(stats.device_waits, 0u);
    EXPECT_EQ(stats.compile_contexts, 0u);

    // The single-number accessors read the same counters, so a caller that wants one number does
    // not need the picture and cannot get a different answer.
    EXPECT_EQ(renderer.retiredObjectCount(), stats.released_nodes);
    EXPECT_EQ(renderer.deviceWaitCount(), stats.device_waits);
}
