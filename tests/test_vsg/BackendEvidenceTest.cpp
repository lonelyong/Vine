/**
 * @brief The evidence half of the rewritten backend's core: the pixel fixture, the diagnostic route and
 * the steady-state allocation gate - all device-free, all gate-ready.
 *
 * These three exist so that later phases (M1 onwards) add ASSERTIONS instead of inventing them:
 *
 *   * `PixelProbe` is how a phase asks about a picture: "is anything on screen", "is the copy inside its
 *     rectangle while the centre kept the clear colour", "is this region still untouched";
 *   * `Diagnostics` is the one route every report goes through, with "report once per episode" as a type
 *     and per-category counts a phase can gate on (`clean()`);
 *   * `AllocationGate` is the steady-state rule - a frame that changes nothing must not allocate -
 *     measured in-process (glibc's heap reading) instead of by interposing the allocator.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>

#include <vine/vsg/core/AllocationGate.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/PhaseTable.hpp>
#include <vine/vsg/core/PixelProbe.hpp>

using vine::graphics::DiagnosticCategory;
using vine::graphics::DiagnosticSeverity;
using vine::graphics::RenderDiagnostic;
using vine::graphics::Viewport;
using vine::vsg::core::AllocationGate;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::Phase;
using vine::vsg::core::PhaseTable;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::ReportOnce;
using vine::vsg::core::Rgba8;

namespace
{

constexpr Rgba8 kBlack{0, 0, 0, 255};
constexpr Rgba8 kClear{51, 51, 51, 255};
constexpr Rgba8 kRed{255, 0, 0, 255};
constexpr Rgba8 kGreen{0, 255, 0, 255};
constexpr Rgba8 kBlue{0, 0, 255, 255};

/// @brief Builds a 4x4 black image with three known pixels, laid out ROW-major.
///
/// The pattern is chosen so a wrong stride cannot pass: the third pixel is at (0, 1), i.e. the first
/// pixel of the second row, which a column-major or padded reader would read from somewhere else.
PixelProbe makePatternImage()
{
    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    for (std::size_t i = 0; i < 16u; ++i)
    {
        pixels[i * 4u + 3u] = 255;  // opaque
    }
    const auto put = [&pixels](int x, int y, const Rgba8& color) {
        const std::size_t offset = (static_cast<std::size_t>(y) * 4u + static_cast<std::size_t>(x)) * 4u;
        pixels[offset]           = color.r;
        pixels[offset + 1u]      = color.g;
        pixels[offset + 2u]      = color.b;
        pixels[offset + 3u]      = color.a;
    };
    put(0, 0, kRed);
    put(3, 0, kGreen);
    put(0, 1, kBlue);
    return PixelProbe(4, 4, std::move(pixels));
}

/// @brief Builds an 8x8 image filled with the clear colour except a 3x2 "copy" rectangle at (2, 1).
PixelProbe makeCopyIntoRectangleImage()
{
    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    for (std::size_t i = 0; i < 64u; ++i)
    {
        pixels[i * 4u + 0u] = kClear.r;
        pixels[i * 4u + 1u] = kClear.g;
        pixels[i * 4u + 2u] = kClear.b;
        pixels[i * 4u + 3u] = kClear.a;
    }
    for (int y = 1; y < 3; ++y)
    {
        for (int x = 2; x < 5; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x)) * 4u;
            pixels[offset]           = kRed.r;
            pixels[offset + 1u]      = kRed.g;
            pixels[offset + 2u]      = kRed.b;
            pixels[offset + 3u]      = kRed.a;
        }
    }
    return PixelProbe(8, 8, std::move(pixels));
}

}  // namespace

TEST(CorePixelProbeTest, TheBufferMustMatchTheStatedSize)
{
    // A buffer that is not width * height * 4 is a different bug from a wrong picture, and the probe
    // says so instead of reading whatever happens to be there.
    const PixelProbe truncated(4, 4, std::vector<std::uint8_t>(10, 0));
    EXPECT_FALSE(truncated.valid());
    EXPECT_EQ(truncated.pixelCount(), 0u);
    EXPECT_EQ(truncated.nonBlackPixels(), 0u);
    EXPECT_DOUBLE_EQ(truncated.nonBlackFraction(), 0.0);
    EXPECT_EQ(truncated.pixel(0, 0), kBlack);
    EXPECT_EQ(truncated.countMatching(kBlack, Viewport{0, 0, 4, 4}), 0u);
}

TEST(CorePixelProbeTest, PixelsAreReadRowMajorFromTheTopLeft)
{
    const PixelProbe image = makePatternImage();
    ASSERT_TRUE(image.valid());
    EXPECT_EQ(image.pixelCount(), 16u);

    EXPECT_EQ(image.pixel(0, 0), kRed);
    EXPECT_EQ(image.pixel(3, 0), kGreen);
    EXPECT_EQ(image.pixel(0, 1), kBlue);  // first pixel of the second row: stride is width * 4
    EXPECT_EQ(image.pixel(1, 1), kBlack);

    // Out of range reads opaque black rather than crashing.
    EXPECT_EQ(image.pixel(-1, 0), kBlack);
    EXPECT_EQ(image.pixel(4, 0), kBlack);
}

TEST(CorePixelProbeTest, TheNonBlackFractionIsHowNothingDrawnFails)
{
    const PixelProbe pattern = makePatternImage();
    EXPECT_EQ(pattern.nonBlackPixels(), 3u);

    const PixelProbe empty(4, 4, std::vector<std::uint8_t>(64, 0));
    EXPECT_EQ(empty.nonBlackPixels(), 0u);
    EXPECT_DOUBLE_EQ(empty.nonBlackFraction(), 0.0);  // "validation clean and nothing drawn"
}

TEST(CorePixelProbeTest, ACopyInsideItsRectangleLeavesTheRestAlone)
{
    // The picture-in-picture assertion, in the two halves a phase actually needs: the rectangle shows
    // the copy, and everything outside it still shows the clear colour.
    const PixelProbe image = makeCopyIntoRectangleImage();

    const Viewport copy_rect{2, 1, 3, 2};
    EXPECT_EQ(image.countMatching(kRed, copy_rect), 6u);
    EXPECT_EQ(image.countDifferingFrom(kRed, copy_rect), 0u);

    const Viewport outside{0, 0, 8, 8};
    EXPECT_EQ(image.countMatching(kClear, outside), 58u);   // 64 - 6
    EXPECT_FALSE(image.wholeImageMatches(kClear));
}

TEST(CorePixelProbeTest, ARectangleLargerThanTheImageCountsOnlyTheOverlap)
{
    // A phase that asks about a rectangle past the surface must get the overlap counted, not an
    // out-of-range read: the clamping is the probe's job, once, for every phase.
    const PixelProbe image = makePatternImage();
    EXPECT_EQ(image.countMatching(kBlack, Viewport{2, 2, 10, 10}), 4u);  // the 2x2 bottom-right corner
    EXPECT_EQ(image.countDifferingFrom(kBlack, Viewport{-4, -4, 10, 10}), 3u);
    EXPECT_EQ(image.countMatching(kBlack, Viewport{9, 9, 2, 2}), 0u);  // entirely outside
}

TEST(CorePixelProbeTest, APhaseCanGateOnAPicture)
{
    const PixelProbe drawn = makeCopyIntoRectangleImage();
    const PixelProbe empty(4, 4, std::vector<std::uint8_t>(64, 0));

    PhaseTable table;
    table.add(Phase{"something was drawn", [&drawn] { return drawn.nonBlackFraction() >= 0.3; }});
    table.add(Phase{"nothing was drawn", [&empty] { return empty.nonBlackFraction() >= 0.3; }});

    const auto report = table.runAll();
    ASSERT_EQ(report.lines.size(), 2u);
    EXPECT_EQ(report.lines[0], "[selftest] something was drawn");
    EXPECT_EQ(report.lines[1], "[selftest] nothing was drawn FAILED: assertion failed");
    EXPECT_FALSE(report.ok());
}

TEST(CoreDiagnosticsTest, ReportsAreCountedPerCategoryAndForwardedToTheSink)
{
    Diagnostics                    diagnostics;
    std::vector<RenderDiagnostic> forwarded;
    diagnostics.setSink([&forwarded](const RenderDiagnostic& diagnostic) { forwarded.push_back(diagnostic); });

    diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::ContentSkipped,
                       vine::String(u8"a pass had nothing to draw"));
    diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::GeometryRejected,
                       vine::String(u8"no positions"));
    diagnostics.report(DiagnosticSeverity::Error, DiagnosticCategory::GeometryRejected,
                       vine::String(u8"indices out of range"));

    EXPECT_EQ(diagnostics.total(), 3u);
    EXPECT_EQ(diagnostics.count(DiagnosticCategory::GeometryRejected), 2u);
    EXPECT_EQ(diagnostics.count(DiagnosticCategory::ContentSkipped), 1u);
    EXPECT_EQ(diagnostics.count(DiagnosticCategory::ChannelIgnored), 0u);
    EXPECT_FALSE(diagnostics.clean());

    ASSERT_EQ(forwarded.size(), 3u);
    EXPECT_EQ(static_cast<int>(forwarded[0].severity), static_cast<int>(DiagnosticSeverity::Warning));
    EXPECT_EQ(static_cast<int>(forwarded[1].category), static_cast<int>(DiagnosticCategory::GeometryRejected));
    EXPECT_FALSE(forwarded[0].message.empty());
}

TEST(CoreDiagnosticsTest, AReportIsCountedEvenWithNoSinkInstalled)
{
    // The host may install a sink late (or never): the count is what a phase gates on, so it must not
    // depend on anyone listening.
    Diagnostics diagnostics;
    EXPECT_FALSE(diagnostics.sink());

    diagnostics.report(DiagnosticSeverity::Warning, DiagnosticCategory::UnsupportedRequest,
                       vine::String(u8"nothing is listening"));

    EXPECT_EQ(diagnostics.total(), 1u);
    EXPECT_EQ(diagnostics.count(DiagnosticCategory::UnsupportedRequest), 1u);
    EXPECT_FALSE(diagnostics.clean());
}

TEST(CoreDiagnosticsTest, AnEpisodeReportsItselfOnceAndReArms)
{
    Diagnostics  diagnostics;
    ReportOnce   episode;
    const auto   report = [&diagnostics, &episode] {
        return diagnostics.reportOnce(episode, DiagnosticSeverity::Warning,
                                      DiagnosticCategory::PassProtocolViolation,
                                      vine::String(u8"a drawing call with no scope"));
    };

    EXPECT_TRUE(report());
    EXPECT_FALSE(report());  // same episode: silent, and not counted twice
    EXPECT_TRUE(episode.reported());
    EXPECT_EQ(diagnostics.total(), 1u);

    episode.rearm();
    EXPECT_TRUE(report());  // the condition ended and came back
    EXPECT_EQ(diagnostics.total(), 2u);
}

TEST(CoreAllocationGateTest, ASteadyWindowGrowsTheHeapByNothing)
{
    if (!AllocationGate::supported())
    {
        GTEST_SKIP() << "this platform cannot report heap usage";
    }

    // A frame's worth of plan building, with the arena already warm: the second run must reuse the
    // chunks the first one made. Nothing in this lambda touches the heap.
    FrameArena   arena(4096);
    const auto   build_frame = [&arena] {
        arena.reset();
        std::vector<int> borrowed{1, 2, 3, 4, 5, 6, 7, 8};
        const auto       copy = arena.copy<int>(std::span<const int>(borrowed));
        (void)copy;
    };
    build_frame();  // warm up: this one is allowed to start chunks

    AllocationGate gate;
    gate.begin();
    build_frame();
    const std::ptrdiff_t grew = gate.end();

    EXPECT_EQ(arena.allocations(), 0u);  // the arena did not grow either
    EXPECT_EQ(grew, 0);                  // and neither did the process
    EXPECT_EQ(gate.windows(), 1u);
    EXPECT_EQ(gate.lastGrowth(), 0);
}

TEST(CoreAllocationGateTest, ADeliberateAllocationIsCaught)
{
    if (!AllocationGate::supported())
    {
        GTEST_SKIP() << "this platform cannot report heap usage";
    }

    AllocationGate gate;
    gate.begin();
    std::vector<int> grows;
    for (int i = 0; i < 4096; ++i)
    {
        grows.push_back(i);  // reallocation after reallocation: this must show up
    }
    const std::ptrdiff_t grew = gate.end();

    EXPECT_GT(grew, 0) << "the gate has to be able to fail, or it is not a gate";
    EXPECT_GT(grows.size(), 0u);
}
