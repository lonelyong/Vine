/**
 * @brief Target bookkeeping rules that decide whether a build CAN happen at all.
 *
 * An off-screen target whose size is zero cannot be built: no attachments, no graph, and every
 * pass drawing into it draws nothing. The build attempt happens once per frame (the entry stays
 * unbuilt), so a host that forgot to size its target would otherwise see a pass that never
 * appears with no reason for it — the shape D60 fixed on the readback side ("a backend reports
 * what it could not do instead of degrading silently").
 *
 * These tests need no device: the size check runs before the build touches the window's device,
 * and the episode rule is pure.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>

using namespace vine::graphics;

namespace
{

/// Collects what the backend reports, without a renderer.
struct Captured
{
    std::vector<RenderDiagnostic> items;

    /// Route the reports of a device-free bookkeeping call into this collector.
    vine::vsg::VsgDiagnostics route()
    {
        vine::vsg::VsgDiagnostics diagnostics;
        diagnostics.setDownstream([this](const RenderDiagnostic& diagnostic) {
            items.push_back(diagnostic);
        });
        return diagnostics;
    }

    /// Number of captured diagnostics in @p category.
    std::size_t count(DiagnosticCategory category) const
    {
        std::size_t n = 0;
        for (const auto& item : items) {
            if (item.category == category) {
                ++n;
            }
        }
        return n;
    }
};

}  // namespace

/**
 * @brief Building an unsized target says why nothing was built — once.
 */
TEST(TargetBookkeepingTest, BuildingATargetWithoutASizeIsReportedOnce)
{
    vine::vsg::VsgRendererState state;
    Captured                  captured;
    const auto                diagnostics = captured.route();

    RenderTargetPtr target(new RenderTarget());
    target->setName(u8"unsized");
    // A fresh target is 1x1 (RenderTarget's default), so "no size" is the explicit case a
    // host hits when it sizes a target from a window that does not exist yet.
    target->setSize(0, 0);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);

    // Two build attempts, as two frames would: the entry stays unbuilt, so the second attempt is
    // the same problem and must not repeat the report.
    vine::vsg::detail::buildOffscreenTarget(state, diagnostics, target.get());
    vine::vsg::detail::buildOffscreenTarget(state, diagnostics, target.get());

    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::TargetBuildFailed);
    // The message carries the fix (the size) and the consequence (nothing is drawn).
    EXPECT_NE(captured.items[0].message.stdstr().find("no size"), std::string::npos);
    EXPECT_NE(captured.items[0].message.stdstr().find("unsized"), std::string::npos);

    // Nothing was built, so the entry stays without attachments: the passes drawing into this
    // target draw nothing, which is what the report explains.
    const auto entry = state.targets.find(target.get());
    ASSERT_NE(entry, state.targets.end());
    EXPECT_FALSE(entry->second.attachments_built);
}

/**
 * @brief The episode is "this target has no size": a usable size re-arms the report.
 *
 * Without the re-arm, a target that is sized, then loses its size (a host that clears it, a
 * resize path that passes 0), would never be reported again — the silent case would come back
 * the second time it happens.
 */
TEST(TargetBookkeepingTest, AUsableSizeReArmsTheReportOfAMissingOne)
{
    bool reported = false;

    // A build attempt on an unsized target reports...
    EXPECT_TRUE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_TRUE(reported);
    // ...the rest of the episode does not...
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(640, 0, reported));
    // ...and a usable size ends it.
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(640, 360, reported));
    EXPECT_FALSE(reported);

    // So the next episode is reported again instead of being swallowed.
    EXPECT_TRUE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
}
