/**
 * @brief The frame protocol: what a beginFrame() ... swapBuffers() pair guarantees.
 *
 * Every deferred release this backend does (replaced renderer objects, per-draw block slots, the
 * retained nodes a content slot drops) is counted in SUBMITTED frames, so its advance is only legal
 * after a frame has been committed — a precondition that used to live as prose ("called once per
 * SUBMITTED frame") in three separate declarations, where any new call site could break it. It is a
 * TOKEN now (@ref FrameCommit): VsgRenderer mints exactly one per frame in beginFrame() and the one
 * submitFrame() consumes it, so neither "advance before the submit" nor "advance twice in one frame"
 * — both of which would release GPU objects a frame too early, while a submitted command buffer may
 * still name them — is something a call site can write by accident.
 *
 * These tests drive that on a renderer that never initialized a device (constructing VsgRenderer
 * creates no window/viewer, and the frame calls only touch the token), so they are deterministic and
 * need no GPU. The other half of the contract — that the rings really do release one ring cycle after
 * a committed frame — is pinned by DrawBlockPoolRetireTest, GeometrySafetyTest and
 * PassObjectReleaseTest, which now mint the same token for the frames they simulate.
 */

#include <gtest/gtest.h>

#include <type_traits>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/vsg/VsgDeferredRelease.hpp>
#include <vine/vsg/VsgRenderer.hpp>

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

} // namespace

/**
 * @brief The token cannot be made up: it has to be spelled, and it stays cheap to pass.
 *
 * The point of the type is that an advance SAYS which commit released it (see FrameCommit): a
 * default-constructible token would be a value any code could produce without saying anything, which
 * is exactly the "precondition in prose" this replaced. Copyable and trivial, because it travels into
 * every content slot's ring on the settle path.
 */
TEST(FrameCommitTest, TheTokenCannotBeDefaultConstructedAndIsCheapToPass)
{
    static_assert(!std::is_default_constructible_v<vine::vsg::FrameCommit>,
                  "a token nobody minted must not be expressible");
    static_assert(std::is_trivially_copyable_v<vine::vsg::FrameCommit>,
                  "the token travels by value into every ring advance");
    SUCCEED();
}

/**
 * @brief swapBuffers() without a beginFrame() is refused, and says so once.
 *
 * A submit that no frame opened cannot be served: serving it would record and present the frame a
 * second time and advance the deferral rings a second time for it, releasing what they parked a frame
 * too early. The refusal is an EPISODE — the rest of it is silent, and a beginFrame() re-arms the
 * report — so a host looping on swapBuffers() is told once instead of every iteration.
 */
TEST(FrameCommitTest, SwapBuffersWithoutBeginFrameIsRefusedAndReportedOnce)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    renderer.swapBuffers();
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_EQ(captured.items[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(captured.items[0].category, DiagnosticCategory::PassProtocolViolation);
    EXPECT_EQ(renderer.diagnosticCount(DiagnosticCategory::PassProtocolViolation), 1u);

    // Same episode: still refused, not reported again.
    renderer.swapBuffers();
    EXPECT_EQ(captured.items.size(), 1u);

    // A frame that was opened is submitted silently, and re-arms the report for the next one.
    renderer.beginFrame();
    renderer.endFrame();
    renderer.swapBuffers();
    EXPECT_EQ(captured.items.size(), 1u);

    renderer.swapBuffers();
    EXPECT_EQ(captured.items.size(), 2u);
    EXPECT_EQ(renderer.diagnosticCount(DiagnosticCategory::PassProtocolViolation), 2u);
}

/**
 * @brief A well-formed frame pair is silent, and a second submit of one frame advances nothing.
 *
 * The rings are what make the refusal worth having: two submits for one opened frame would advance
 * them twice, so the second must not reach them. Nothing was parked over this test, so the observable
 * proof is the counts staying put and no diagnostic — the ring's own release timing is pinned by the
 * tests that park objects.
 */
TEST(FrameCommitTest, ASecondSubmitOfOneFrameIsRefusedAndAdvancesNothing)
{
    vine::vsg::VsgRenderer renderer;
    Captured                  captured;
    captured.installOn(renderer);

    renderer.beginFrame();
    renderer.swapBuffers();
    EXPECT_TRUE(captured.items.empty()) << "one frame, one submit: nothing to report";

    const std::size_t waits_before   = renderer.deviceWaitCount();
    const std::size_t retired_before = renderer.retiredObjectCount();
    renderer.swapBuffers();
    EXPECT_EQ(renderer.deviceWaitCount(), waits_before);
    EXPECT_EQ(renderer.retiredObjectCount(), retired_before)
        << "a submit no frame opened must not advance the rings";
    EXPECT_EQ(captured.items.size(), 1u);
}
