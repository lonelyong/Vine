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

#include <vsg/app/CommandGraph.h>

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

/// How many CountedTargets have been destroyed (see the release test: "freed" has to be observable
/// without reading the freed object).
int s_counted_target_destructions = 0;

/// @brief A render target whose destruction is countable.
///
/// The release test needs to know whether the object it handed to the release path was freed DURING
/// that call. Reading anything off the freed object to find out would itself be undefined behaviour
/// (and an assertion that passes on freed memory proves nothing), so the object announces its
/// destruction instead.
struct CountedTarget : RenderTarget
{
    ~CountedTarget() override { ++s_counted_target_destructions; }
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
    vine::vsg::ReportOnce reported;

    // A build attempt on an unsized target reports...
    EXPECT_TRUE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_TRUE(reported.reported());
    // ...the rest of the episode does not...
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(640, 0, reported));
    // ...and a usable size ends it.
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(640, 360, reported));
    EXPECT_FALSE(reported.reported());

    // So the next episode is reported again instead of being swallowed.
    EXPECT_TRUE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
    EXPECT_FALSE(vine::vsg::detail::beginTargetSizeMissingEpisode(0, 0, reported));
}

/**
 * @brief The tombstone of a released depth source holds that source, so its address cannot be reused.
 *
 * A borrower whose source is released keeps a tombstone saying "this source is not borrowable any more", so
 * the borrow is not retried (and re-reported) every frame. A tombstone that only REMEMBERED the address
 * would refuse the borrow of a brand-new target allocated at the released one's address — for the rest of
 * the session, with nothing to tell the host why — which is the same address-reuse defect the target table
 * itself fixed by owning the target it is keyed by.
 */
TEST(TargetBookkeepingTest, AReleasedSourcesTombstoneOwnsItSoItsAddressCannotBeReused)
{
    vine::vsg::VsgRendererState state;
    // The release path is the session's, and the graph it re-orders afterwards is the session's command
    // graph: both are needed for the bookkeeping to run without a device (no window, no viewer, no images).
    state.initialized   = true;
    state.command_graph = ::vsg::CommandGraph::create();

    Captured   captured;
    const auto diagnostics = captured.route();

    RenderTargetPtr borrower(new RenderTarget());
    {
        RenderTargetPtr source(new RenderTarget());
        // The borrow as the build records it (the field the release path reads).
        state.entryFor(borrower.get()).depth_source = source.get();

        vine::vsg::detail::releaseRenderTarget(state, diagnostics, source.get());

        // The tombstone is what tells the next build "do not retry this source"...
        const auto& tombstone = state.entryFor(borrower.get()).unusable_depth_source;
        EXPECT_EQ(tombstone.get(), source.get());
        // ...and it HOLDS it: the host's reference going out of scope must not free the address, because a
        // new target landing there would compare equal to the tombstone and stay refused for ever.
        EXPECT_EQ(source->useCount(), 2u); // this scope's reference + the tombstone
    }

    // The host has let go: the only remaining reference is the tombstone's, which is the point of the test.
    EXPECT_EQ(state.entryFor(borrower.get()).unusable_depth_source->useCount(), 1u);
}

/**
 * @brief The released target outlives its own release, even when the table entry was its last owner.
 *
 * `releaseRenderTarget` erases the table entry, and the entry OWNS the target (that ownership is what
 * keeps the address unreusable for a later target) — so on the path that exists exactly for "the host
 * dropped the target itself" (VsgRenderer::releaseAbandonedTargets releases what only the entry still
 * holds) the erase is the target's LAST release. The rest of the function then still reads the
 * released target: its name for the log line, its identity compared against every borrowing and
 * sampling slot, and the tombstone that keeps a borrowing target from retrying the borrow. Without a
 * reference held for the length of the call that is a use-after-free whose write lands on freed
 * memory (the tombstone's addRef), which is why this test counts the destructions instead of
 * believing the call.
 */
TEST(TargetBookkeepingTest, AReleasedTargetOutlivesItsOwnReleaseWhenTheEntryWasItsLastOwner)
{
    vine::vsg::VsgRendererState state;
    // The release path is the session's, and the graph it re-orders afterwards is the session's command
    // graph: both are needed for the bookkeeping to run without a device (no window, no viewer, no images).
    state.initialized   = true;
    state.command_graph = ::vsg::CommandGraph::create();

    Captured   captured;
    const auto diagnostics = captured.route();

    // The released target is BORROWED by another one: that is what makes the release path read it
    // after the entry is erased (the borrower's tombstone and the log line name it).
    RenderTargetPtr               borrower(new RenderTarget());
    vine::graphics::RenderTarget* released = nullptr;
    {
        RenderTargetPtr source(new CountedTarget());
        released = source.get();
        // The table entry owns it (the rule every path that touches the table follows)...
        state.entryFor(released);
        state.entryFor(borrower.get()).depth_source = released;
    }
    // ...and the host's reference is gone: the entry's owner is the last one, which is the state
    // VsgRenderer::releaseAbandonedTargets looks for.
    ASSERT_EQ(released->useCount(), 1u);

    s_counted_target_destructions = 0;
    vine::vsg::detail::releaseRenderTarget(state, diagnostics, released);

    // The release may not free the target it is still describing — not for the tombstone below, and
    // not for the log line above it.
    EXPECT_EQ(s_counted_target_destructions, 0) << "the target was freed while its own release was still using it";
    // And the borrower's tombstone now OWNS it, which is what makes "a later shareDepth() naming a
    // different source clears the condition" true.
    EXPECT_EQ(state.entryFor(borrower.get()).unusable_depth_source.get(), released);
    EXPECT_EQ(released->useCount(), 1u); // the tombstone's reference only
    EXPECT_TRUE(state.targets.find(released) == state.targets.end());
}
