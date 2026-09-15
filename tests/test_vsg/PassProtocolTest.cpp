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
 * The request is now ONE structure with an explicit scope, and the scope is the ONLY way to drive
 * this backend: the state setters are inert on their own (the next scope starts from an empty
 * request), while a DRAWING call with no pass behind it is refused and reported — it would otherwise
 * produce content nobody asked for, with whatever state the last scope left. These tests drive that
 * on a renderer that never initialized a device (constructing VsgRenderer creates no window/viewer,
 * and the protocol calls only touch the retained request), so they are deterministic and need no GPU.
 */

#include <gtest/gtest.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/vsg/VsgRenderer.hpp>
#include <vine/vsg/VsgRendererState.hpp>

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
 * @brief Legal sequences stay silent — including state announced with no scope open.
 *
 * The engine's pass protocol (matched beginPass / endPass per pass, with the request queued in
 * between) is one legal sequence. The other one a host can write is the state setters alone, with no
 * scope at all: they are inert rather than a violation — a request nobody drew with is dropped by
 * the next beginPass() — so nothing may be reported for them. The DRAWING calls are a different
 * matter: with no pass behind them they are refused (see the test below).
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

    // State with no scope open: inert, and therefore silent.
    renderer.setRenderTarget(nullptr);
    renderer.setPassOrder(2);
    renderer.setDepthMode(DepthMode::Disabled);
    renderer.setLights({});
    renderer.setViewport(0, 0, 4, 4);

    EXPECT_TRUE(captured.items.empty());
    EXPECT_EQ(renderer.diagnosticCount(), 0u);
}

/**
 * @brief One viewport / lights announcement serves exactly ONE drawing call.
 *
 * The contract is per drawing call (RenderBackend::setViewport / setLights), and that is what lets one
 * pass scope place several pictures-in-picture: each draw announces its own rectangle. The rule is
 * easy to lose in a refactor — a "sticky" viewport looks harmless and would silently move every
 * second PiP of a pass to the full surface — so the queue's consumption is pinned here. A pass that
 * draws more than once must announce again before each draw; a draw that finds nothing queued keeps
 * the backend default (lights) / the whole surface (viewport).
 */
TEST(PassProtocolTest, OneAnnouncementServesOneDrawingCall)
{
    vine::vsg::VsgPassRequest request;

    const Viewport rect{ 4, 5, 96, 54 };
    request.viewport = rect;

    const auto first = request.takeViewport();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->x, 4);
    EXPECT_EQ(first->y, 5);
    EXPECT_EQ(first->width, 96);
    EXPECT_EQ(first->height, 54);

    // Consumed: the next draw of the same scope finds nothing queued...
    EXPECT_FALSE(request.takeViewport().has_value());
    // ...and the same holds for the lights announced for that draw.
    EXPECT_TRUE(request.takeLights().empty());

    // A fresh announcement is served to the next draw, and only to it.
    request.viewport = Viewport{ 8, 9, 10, 11 };
    ASSERT_TRUE(request.takeViewport().has_value());
    EXPECT_FALSE(request.takeViewport().has_value());
}

/**
 * @brief A drawing call with no pass announced is refused, and says so once per frame.
 *
 * The pass is the identity of everything this backend retains and the scope is what makes "which call
 * means what" independent of the call order, so a drawing call with no pass behind it has nothing to
 * belong to: serving it would put content on screen with whatever state the last scope left (a target,
 * an order, a depth policy — none of them announced for this call). The state setters are NOT in this
 * club: they are inert on their own and silent (see LegalSequencesAreSilent).
 *
 * The report is an EPISODE bounded by the frame: the first refusal says so (naming the call), the rest
 * of the frame is refused silently, and the next beginFrame() re-arms it — a host looping on such a
 * call is told once, not every iteration.
 */
TEST(PassProtocolTest, ADrawingCallOutsideAPassScopeIsRefusedAndReportedOnce)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    RenderTargetPtr source(new RenderTarget());
    const auto      copy_program = vine::graphics::screenCopyProgram();

    // Nothing announced: every drawing entry point has nothing to belong to.
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    renderer.drawScreenProgram(source.get(), copy_program.get(), nullptr);

    ASSERT_EQ(captured.items.size(), 1u) << "one report per frame, whatever the call";
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::PassProtocolViolation);
    // One format string per entry point: the host learns WHICH call was refused.
    EXPECT_NE(captured.items[0].message.stdstr().find("render()"), std::string::npos);

    // A new frame is a new episode.
    renderer.beginFrame();
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    EXPECT_EQ(captured.items.size(), 2u);

    // And a pass scope makes the very same calls legal.
    RenderPassPtr pass(new RenderPass());
    renderer.beginPass(pass.get());
    renderer.setRenderTarget(nullptr);
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    renderer.endPass();
    EXPECT_EQ(captured.items.size(), 2u);
}

/**
 * @brief A call whose announced target was released is refused, and says so once.
 *
 * The pass announced by beginPass() is borrowed for its SCOPE, and releaseRenderTarget() announces
 * that the caller may destroy the target now — so a scope that announced it can no longer use it,
 * and the class contract forbids keeping such a pointer. The call cannot be honoured, and drawing
 * into the window instead would put the content somewhere the host never asked for, so it is skipped
 * and reported with the fix. The release is one EPISODE, and the episode is the rest of the scope:
 * the first refusal says so, the rest are refused silently, and the next setRenderTarget() (or the
 * next scope, which starts from an empty request) re-arms it.
 */
TEST(PassProtocolTest, DrawingOnAReleasedTargetIsRefusedAndReportedOnce)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    RenderTargetPtr target(new RenderTarget());
    RenderPassPtr   pass(new RenderPass());

    renderer.beginPass(pass.get());
    renderer.setRenderTarget(target.get());
    renderer.releaseRenderTarget(target.get());

    renderer.render(std::vector<RenderCommand>{}, nullptr);
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::PassProtocolViolation);
    // One format string per entry point: the host learns WHICH call was skipped.
    EXPECT_NE(captured.items[0].message.stdstr().find("render()"), std::string::npos);

    // The rest of the episode is the same problem: refused, but not a second report...
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    EXPECT_EQ(captured.items.size(), 1u);
    // ...and the other entry points that draw into the announced target refuse it too.
    RenderTargetPtr source(new RenderTarget());
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    const auto copy_program = vine::graphics::screenCopyProgram();
    renderer.drawScreenProgram(source.get(), copy_program.get(), nullptr);
    EXPECT_EQ(captured.items.size(), 1u);

    // Announcing a target again ends the episode: calls are served from here on.
    renderer.setRenderTarget(nullptr);
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    renderer.clear(vine::Color(0, 0, 0, 255), true);
    EXPECT_EQ(captured.items.size(), 1u);

    // A new release is a new episode, so it is reported again.
    renderer.setRenderTarget(target.get());
    renderer.releaseRenderTarget(target.get());
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    ASSERT_EQ(captured.items.size(), 2u);
    EXPECT_EQ(renderer.diagnosticCount(DiagnosticCategory::PassProtocolViolation), 2u);

    renderer.endPass();
}

/**
 * @brief A pass scope starts clean, so a dead announcement cannot leak into it.
 *
 * beginPass() drops the whole request, so state announced before a scope (including a target that
 * was released while announced) is gone when the scope opens — a pass that announced its own (live)
 * target therefore draws, and is not refused for somebody else's dead pointer.
 */
TEST(PassProtocolTest, APassScopeClearsADeadAnnouncement)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    RenderTargetPtr target(new RenderTarget());
    renderer.setRenderTarget(target.get());
    renderer.releaseRenderTarget(target.get());

    RenderPassPtr pass(new RenderPass());
    renderer.beginPass(pass.get());
    renderer.render(std::vector<RenderCommand>{}, nullptr);
    renderer.endPass();

    EXPECT_TRUE(captured.items.empty());
}
