/**
 * @brief Releasing a pass must drop the per-pass objects its target retained for it.
 *
 * RenderBackend's class contract states two rules for retained state: it is released by the
 * matching release* call, and it must not grow with the frame count (a long-running session has to
 * reach a steady state). A pass' MATERIALISED objects (its render pass + one-frame transient
 * variant, its framebuffer and its RenderGraph) were the exception: only a TARGET rebuild ever
 * dropped them, so a host that adds and removes passes — an editor, or RenderEngine::removePass,
 * which does call releasePass() — accumulated one set per ever-seen pass, keyed by a raw
 * RenderPass* the entry did not own (the rule the target / material / geometry / program caches
 * already follow). Worse, an address-reusing pass could then match its dead predecessor's entry.
 *
 * These tests drive the erase path on a state that never initialized a device: the table, the key,
 * the parking and the ring all work without one. The pass' GPU objects themselves are built by a
 * device, so the test parks a device-free RenderGraph (the same park() call the real objects take)
 * and asserts the ring gives it back — while the park-vs-destroy safety of the Vulkan objects
 * themselves stays covered by the device-level gate (RESULT: PASS, 0 VUID).
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <vsg/app/RenderGraph.h>

#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>
#include <vine/vsg/VsgRendererState.hpp>
#include <vine/vsg/VsgTargetBookkeeping.hpp>

using namespace vine::graphics;

namespace
{

/// How many objects the session's retire ring is holding right now.
std::size_t parkedObjects(const vine::vsg::VsgRendererState& state)
{
    std::size_t count = 0;
    for (const auto& bucket : state.retireRing.ring) {
        count += bucket.size();
    }
    return count;
}

/// Registers one materialised pass under @p pass in @p entry (its objects, as far as a
/// device-less table can hold them: the graph is real, the Vulkan objects need a device).
void retainAPass(vine::vsg::VsgRenderTargetEntry& entry, const RenderPass* pass)
{
    vine::vsg::VsgRenderTargetEntry::PassObjects objects;
    objects.graph = ::vsg::RenderGraph::create();
    entry.passes.insert_or_assign(vine::vsg::SlotKey::ownerPass(pass), objects);
}

} // namespace

TEST(PassObjectReleaseTest, ReleasingAPassDropsTheObjectsItsTargetRetainedForIt)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    RenderPassPtr                pass(new RenderPass());
    retainAPass(window, pass.get());

    vine::vsg::detail::erasePassFromTarget(state, nullptr, pass.get());

    // The table no longer holds the pass: it cannot be replaced at the same address and inherit its
    // predecessor's render pass / framebuffer / graph.
    EXPECT_TRUE(window.passes.empty());
    // Nothing is destroyed synchronously and the device is NOT stopped: the objects are parked (an
    // in-flight command buffer may still name them), unlike the destructive slot teardown which
    // keeps the counted device wait.
    EXPECT_EQ(parkedObjects(state), 1u);
    EXPECT_EQ(state.retireRing.waits, 0u);
}

TEST(PassObjectReleaseTest, TheRetainedPassTableDoesNotGrowWithThePassesASessionHasSeen)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);

    for (int i = 0; i < 32; ++i) {
        RenderPassPtr pass(new RenderPass());
        retainAPass(window, pass.get());
        ASSERT_EQ(window.passes.size(), 1u);

        vine::vsg::detail::erasePassFromTarget(state, nullptr, pass.get());

        // Every release leaves the table as empty as it found it: retained state has to reach a
        // steady state ("must not grow with the frame count").
        EXPECT_TRUE(window.passes.empty());
        EXPECT_EQ(state.retireRing.waits, 0u);
    }
}

TEST(PassObjectReleaseTest, MovingAPassToAnotherTargetDropsItFromTheTargetItLeft)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    RenderTargetPtr              other(new RenderTarget());
    auto&                        other_entry = state.entryFor(other.get());
    RenderPassPtr                pass(new RenderPass());
    retainAPass(window, pass.get());
    retainAPass(other_entry, pass.get());

    // The pass renders into `other` now, so the window must stop retaining anything for it...
    vine::vsg::detail::retargetPass(state, pass.get(), other.get());
    EXPECT_TRUE(window.passes.empty());
    // ...while the target it moved to keeps what it holds.
    EXPECT_EQ(other_entry.passes.size(), 1u);
}

TEST(PassObjectReleaseTest, ANullOrUnknownPassIsANoOp)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    RenderPassPtr                pass(new RenderPass());
    RenderPassPtr                unknown(new RenderPass());
    retainAPass(window, pass.get());

    vine::vsg::detail::erasePassFromTarget(state, nullptr, nullptr);
    vine::vsg::detail::erasePassFromTarget(state, nullptr, unknown.get());
    EXPECT_EQ(window.passes.size(), 1u);

    vine::vsg::detail::erasePassFromTarget(state, nullptr, pass.get());
    EXPECT_TRUE(window.passes.empty());
}

TEST(PassObjectReleaseTest, TheRingReleasesWhatTheReleaseParked)
{
    // Parked is not leaked: the ring holds the objects and hands them back once kRetireRingDepth
    // frame advances have passed.
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    RenderPassPtr                pass(new RenderPass());
    retainAPass(window, pass.get());

    vine::vsg::detail::erasePassFromTarget(state, nullptr, pass.get());
    ASSERT_EQ(parkedObjects(state), 1u);

    for (std::size_t i = 0; i < vine::vsg::VsgRetireRing::kRetireRingDepth; ++i) {
        state.retireRing.advance();
    }
    EXPECT_EQ(parkedObjects(state), 0u);
    EXPECT_EQ(state.retireRing.released, 1u);
}

/**
 * @brief Releasing a target drops the announcement that names it.
 *
 * The queued request is the direct driver's to manage and survives frames (RenderBackend::
 * beginPass), so a release that left the announcement in place would keep a pointer whose last
 * owner the caller is about to destroy — exactly what the class contract forbids. The release
 * clears it and marks WHY, which is what lets the next call that needed it refuse the work and
 * say so instead of silently drawing into the window (PassProtocolTest covers that report).
 */
TEST(PassObjectReleaseTest, ReleasingATargetDropsTheQueuedAnnouncementThatNamesIt)
{
    vine::vsg::VsgRendererState state;
    vine::vsg::VsgDiagnostics diagnostics;
    RenderTargetPtr             target(new RenderTarget());
    RenderTargetPtr             other(new RenderTarget());

    state.request.target = target.get();
    vine::vsg::detail::releaseRenderTarget(state, diagnostics, target.get());

    EXPECT_EQ(state.request.target, nullptr);
    EXPECT_TRUE(state.request.target_released);

    // An announcement naming ANOTHER target is none of this release's business, so it neither
    // loses the target nor ends up looking dead.
    state.request = vine::vsg::VsgPassRequest{};
    state.request.target = other.get();
    vine::vsg::detail::releaseRenderTarget(state, diagnostics, target.get());

    EXPECT_EQ(state.request.target, other.get());
    EXPECT_FALSE(state.request.target_released);
}
