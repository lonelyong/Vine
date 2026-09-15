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
    return state.retireRing.parkedCount();
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
    EXPECT_EQ(state.retireRing.waitCount(), 0u);
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
        EXPECT_EQ(state.retireRing.waitCount(), 0u);
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
        state.retireRing.advance(vine::vsg::FrameCommit::submitted());
    }
    EXPECT_EQ(parkedObjects(state), 0u);
    EXPECT_EQ(state.retireRing.releasedCount(), 1u);
}

/**
 * @brief Dropping a full-screen program slot detaches its view, PARKS its node and erases the slot.
 *
 * A program slot's node owns the pipeline, the descriptor sets and (through them) the image views of
 * the images it sampled, and the frames in flight may still name all of that: the drop therefore goes
 * through ONE home (detail::eraseProgramSlot), which is also what makes it wait-free. Two of the
 * four drop sites used to skip the parking — the slot REBUILD overwrote the slot, which destroyed
 * the node in flight, and a released render target stopped the device once PER DROPPED SLOT to
 * compensate — so the rule is pinned here rather than left to each site to remember.
 */
TEST(PassObjectReleaseTest, DroppingAProgramSlotParksItsNodeAndDetachesItsView)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    window.graph = ::vsg::RenderGraph::create(); // the graph a window slot's view records into

    RenderPassPtr            pass(new RenderPass());
    const vine::vsg::SlotKey key = vine::vsg::SlotKey::ownerPass(pass.get());

    auto  view     = ::vsg::View::create();
    auto  node     = ::vsg::Group::create();
    auto* node_ptr = node.get();
    {
        auto& slot  = window.program_slots[key];
        slot.view   = view;
        slot.node   = node;
        slot.ready  = true;
        slot.order  = 7;
        window.graph->addChild(view);
    }
    // The slot is the node's only holder, which is what "parked, not destroyed" has to keep alive.
    node.reset();
    ASSERT_EQ(node_ptr->referenceCount(), 1u);

    vine::vsg::detail::eraseProgramSlot(state, window, nullptr, key);

    // The slot is gone and its view stopped being recorded...
    EXPECT_TRUE(window.program_slots.empty());
    EXPECT_EQ(window.graph->children.size(), 0u);
    // ...its node is PARKED (the ring holds it: a submitted command buffer may still name it)...
    EXPECT_EQ(node_ptr->referenceCount(), 1u);
    EXPECT_EQ(parkedObjects(state), 1u);
    // ...and nothing stopped the device for the drop.
    EXPECT_EQ(state.retireRing.waitCount(), 0u);

    // Parked is not leaked: the ring hands it back once the frames in flight are done.
    for (std::size_t i = 0; i < vine::vsg::VsgRetireRing::kRetireRingDepth; ++i) {
        state.retireRing.advance(vine::vsg::FrameCommit::submitted());
    }
    EXPECT_EQ(parkedObjects(state), 0u);
    EXPECT_EQ(state.retireRing.releasedCount(), 1u);
}

TEST(PassObjectReleaseTest, DroppingAProgramSlotThatIsNotThereIsANoOp)
{
    vine::vsg::VsgRendererState state;
    auto&                        window = state.entryFor(nullptr);
    RenderPassPtr                pass(new RenderPass());

    vine::vsg::detail::eraseProgramSlot(state, window, nullptr, vine::vsg::SlotKey::ownerPass(pass.get()));

    EXPECT_EQ(parkedObjects(state), 0u);
    EXPECT_EQ(state.retireRing.waitCount(), 0u);
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
