/**
 * @brief Pass-scope protocol tests (the beginPass / endPass contract).
 *
 * The engine announces every executed pass with RenderBackend::beginPass(pass) /
 * endPass() and queues the pass' request (target, order, depth policy, viewport,
 * lights) between the two. That state used to live in separate pending_* fields
 * that had to be reset in step and were read from three different entry points,
 * so "what does this call mean" depended on the call order — the shape behind a
 * whole class of bugs (state leaking into the next pass, a pass' stacking order
 * silently dropping to 0).
 *
 * The request is now ONE structure with an explicit scope, and misusing the
 * scope is reported on the diagnostics channel instead of degrading quietly.
 * These tests drive the protocol on a renderer that never initialized a device
 * (constructing VsgRenderer creates no window/viewer, and the scope calls only
 * touch the retained request), so they are deterministic and need no GPU.
 */

#include <gtest/gtest.h>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include "VsgRendererImpl.hpp"

#include <vector>

using namespace vine::graphics;

namespace
{

/// Collects the diagnostics a renderer reports.
struct Captured
{
    std::vector<RenderDiagnostic> items;

    /// Installs this collector as the renderer's sink.
    void installOn(vine::vsg::VsgRenderer& renderer)
    {
        renderer.setDiagnosticSink([this](const RenderDiagnostic& diagnostic) {
            items.push_back(diagnostic);
        });
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
 * @brief endPass() without an open scope is a protocol violation, and reported.
 *
 * It means the announced state had already been dropped, so whatever the caller
 * expected to apply did not: silence here would hide a frame that draws
 * something other than what the host asked for.
 */
TEST(PassProtocolTest, EndPassWithoutBeginPassIsReported)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    renderer.endPass();

    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::PassProtocolViolation);
    EXPECT_FALSE(captured.items[0].message.empty());
    EXPECT_EQ(renderer.diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);

    // A well-formed scope after the violation is not reported again.
    RenderPassPtr pass(new RenderPass());
    renderer.beginPass(pass.get());
    renderer.endPass();
    EXPECT_EQ(captured.items.size(), 1u);
}

/**
 * @brief A nested beginPass() is reported, and the new pass starts clean.
 *
 * The engine runs one pass at a time; a nested begin means the outer scope was
 * never ended, so its request (target / order / depth / viewport / lights) would
 * otherwise apply to the new pass.
 */
TEST(PassProtocolTest, NestedBeginPassIsReportedAndStartsClean)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    RenderPassPtr outer(new RenderPass());
    RenderPassPtr inner(new RenderPass());

    renderer.beginPass(outer.get());
    renderer.setPassOrder(7);
    renderer.beginPass(inner.get());
    renderer.endPass();

    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::PassProtocolViolation);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);

    // The pass that closed is the inner one: closing once more is therefore the
    // unpaired case (the outer scope was dropped when the nested one opened).
    renderer.endPass();
    ASSERT_EQ(captured.items.size(), 2u);
    EXPECT_EQ(renderer.diagnosticCount(DiagnosticCategory::PassProtocolViolation), 2u);
}

/**
 * @brief Legal sequences stay silent — including the direct-driver path.
 *
 * Both supported ways of driving the backend must produce NO diagnostics:
 *   * the engine's pass protocol (matched beginPass / endPass per pass, with the
 *     request queued in between);
 *   * a direct driver that never opens a scope (the device self-test and the
 *     legacy keying path), which keeps its request until it overwrites it.
 */
TEST(PassProtocolTest, LegalSequencesAreSilent)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    RenderPassPtr pass_a(new RenderPass());
    RenderPassPtr pass_b(new RenderPass());

    // Engine-driven: two passes with their own requests, each scope closed.
    EXPECT_FALSE(renderer.isPassScopeOpen());
    renderer.beginPass(pass_a.get());
    EXPECT_TRUE(renderer.isPassScopeOpen());
    renderer.setRenderTarget(nullptr);
    renderer.setPassOrder(0);
    renderer.setDepthMode(DepthMode::TestAndWrite);
    renderer.setViewport(0, 0, 16, 16);
    renderer.setLights({});
    renderer.endPass();
    EXPECT_FALSE(renderer.isPassScopeOpen());

    renderer.beginPass(pass_b.get());
    renderer.setPassOrder(1);
    renderer.setDepthMode(DepthMode::TestOnly);
    renderer.endPass();

    // Direct driver: no scope at all, only queued state.
    renderer.setRenderTarget(nullptr);
    renderer.setPassOrder(2);
    renderer.setDepthMode(DepthMode::Disabled);
    renderer.setLights({});

    EXPECT_TRUE(captured.items.empty());
    EXPECT_EQ(renderer.diagnosticCount(), 0u);
}
