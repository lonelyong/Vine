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
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>

#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/api/ContentPass.hpp>

#include <vine/vsg/core/AllocationGate.hpp>
#include <vine/vsg/core/ClearPlan.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/PhaseTable.hpp>
#include <vine/vsg/core/PixelProbe.hpp>

using vine::graphics::DiagnosticCategory;
using vine::graphics::DiagnosticSeverity;
using vine::graphics::RenderCommand;
using vine::graphics::RenderDiagnostic;
using vine::graphics::RenderTarget;
using vine::graphics::Viewport;
using vine::vsg::core::AllocationGate;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::Phase;
using vine::vsg::core::PhaseTable;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::ReportOnce;
using vine::vsg::core::Rgba8;
using vine::vsg::core::TargetShape;

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

TEST(CoreAllocationGateTest, TheCountedHalfSeesChurnTheHeapReadingCannot)
{
    // THE POSITIVE CONTROL FOR THE COUNTING HALF, and the defect it exists to close: a window that allocates
    // and frees the SAME block leaves the process' allocated bytes exactly where they were, so the heap
    // reading answers "nothing happened". That is what a per-pass vector or a `std::function` parked and
    // released looks like from the outside - the shape almost every per-frame allocation actually has.
    AllocationGate gate;
    ASSERT_TRUE(AllocationGate::countsAvailable())
        << "this binary must instrument the allocator: without it, \"counted zero\" and \"nothing counted\" "
           "are the same number (see AllocationGate's file note)";

    gate.begin();
    for (int i = 0; i < 64; ++i)
    {
        auto* block = new char[64];  // allocated and released inside the window
        delete[] block;
    }
    const std::ptrdiff_t grew = gate.end();

    EXPECT_EQ(gate.allocations(), 64u) << "every allocation in the window is counted, churn included";
    EXPECT_EQ(gate.bytes(), 64u * 64u) << "and the bytes it asked for are reported beside the count";
    if (AllocationGate::supported())
    {
        EXPECT_EQ(grew, 0) << "while the heap reading alone would have passed this window";
    }
}

TEST(CoreAllocationGateTest, AZeroCountIsOnlyReadWhereSomethingCounts)
{
    // The two halves are independent, and the gate says which one it is answering: a platform whose C library
    // reports no heap usage (Windows) still gets the counted verdict - that is the whole point of the second
    // half - and a phase must never treat "read nothing" as "measured nothing happened".
    AllocationGate gate;
    gate.begin();
    const std::ptrdiff_t grew = gate.end();

    EXPECT_EQ(gate.allocations(), 0u);
    EXPECT_EQ(gate.bytes(), 0u);
    (void)grew;
    EXPECT_TRUE(AllocationGate::countsAvailable()) << "this binary's allocator is instrumented";
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

TEST(CoreAllocationGateTest, TheRecordPathsBookkeepingCostsAFewSmallVectorsPerPass)
{
    // THE SECOND QUESTION A READER OF THE STEADY-STATE RULE ASKS. The plan path is at ZERO allocations and is
    // gated on it (see CorePhaseTableTest). The record path cannot be: it builds new command NODES for the
    // frame by design (`vsg::RenderGraph`, one bind and one draw per call), so "counted zero" there would be
    // a claim about vsg, not about this backend. What this backend OWNS in that path is its BOOKKEEPING, and
    // that is what the ceilings below are.
    //
    // Measured on 2026-09-24 (MSVC Debug, one pass, one drawing call, steady content): a frame's
    // `VsgExecutor::record` asks for 12 allocations, and they split like this -
    //
    //   * 7 - one `ContentPass` construction: two `std::vector<ReportOnce>` sized per entry (four counted
    //         allocations for two one-element vectors in this build) plus the recorder's own members;
    //   * 2 - `planClearValues`'s one-per-attachment vector (`PassClearPlan::colors`);
    //   * 3 - the pass' `vsg::RenderGraph` and the clear values it carries - NODES, i.e. the deliberate half
    //         (the previous frame's graph may still be in flight, see the executor's note).
    //
    // None of the first two grow with the frame's CONTENT (they are per pass, not per drawing call), and a
    // frame's per-call cost is dominated by the nodes: `test_vsg`'s own fixture measures ~33 allocations for
    // ONE recorded triangle, and a readback/multi-pass frame is bigger still. So the ceilings stay ceilings
    // instead of zeros - the fix (caller-owned scratch for the plan, and episode rows owned by whoever decides
    // the episode) buys a handful of bytes per pass, and it is written up with its trigger in
    // `.ai/design/vsg-reimplementation.md` rather than paid for with an abstraction.
    //
    // This test is the TRIPWIRE: if either number grows, something started allocating per pass and §0.3 of
    // `docs/backend.md` has to be re-argued against the new number.
    ASSERT_TRUE(AllocationGate::countsAvailable())
        << "this binary must instrument the allocator (see AllocationGate's file note)";

    constexpr int kCalls = 64;

    const auto measure = [](int calls, auto&& body) -> std::uint64_t {
        // The body's answer is CONSUMED rather than discarded: what it returns is the check that the measured
        // code really ran (a body that returns a count), and consuming it keeps the bodies free to call
        // `[[nodiscard]]` producers without a cast that this compiler still warns about.
        std::uint64_t sample = 0U;
        for (int index = 0; index < 8; ++index)
        {
            sample += static_cast<std::uint64_t>(body());  // warm-up: the measured code reuses, the first caller may not
        }
        AllocationGate gate;
        gate.begin();
        for (int index = 0; index < calls; ++index)
        {
            sample += static_cast<std::uint64_t>(body());
        }
        (void)gate.end();  // the byte reading is the second opinion here (see the class it comes from)
        EXPECT_GT(sample, 0U) << "the measured code has to have run";
        return gate.allocations();
    };

    TargetShape shape;
    shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    ClearPolicy  policy;
    policy.color         = true;
    const std::uint64_t plan = measure(kCalls, [&]() -> std::uint64_t {
        const vine::vsg::core::PassClearPlan built =
            vine::vsg::core::planClearValues(shape, policy, true, false);
        return built.colors.size();
    });
    EXPECT_LE(plan, 2u * static_cast<std::uint64_t>(kCalls))
        << "one plan is one small vector (measured: 2 counted allocations per call in this build, which is "
           "this build's cost for a one-element vector)";

    Diagnostics                   diagnostics;
    vine::vsg::ContentPass::Scope scope;
    const vine::vsg::ContentPass::Scope::Entry halves[4]{};
    const auto record_once = [&scope, &halves, &diagnostics]() -> std::uint64_t {
        const vine::vsg::ContentPass recorder(scope, diagnostics);
        return scope.entries.size();
    };
    scope.entries = std::span<const vine::vsg::ContentPass::Scope::Entry>(halves, 1U);
    const std::uint64_t one_entry = measure(kCalls, record_once);
    scope.entries = std::span<const vine::vsg::ContentPass::Scope::Entry>(halves, 4U);
    const std::uint64_t four_entries = measure(kCalls, record_once);
    EXPECT_LE(four_entries, 8u * static_cast<std::uint64_t>(kCalls))
        << "one recorder is a fixed handful of small vectors (measured: 7 counted allocations per "
           "construction in this build)";
    EXPECT_EQ(one_entry, four_entries)
        << "the episode memory is one row per entry in a vector that is sized ONCE: its cost must not grow "
           "with the number of entries a pass draws through (a vector per ENTRY would, and that is the shape "
           "this assertion refuses)";
}

TEST(CorePhaseTableTest, AFramesPhaseGatesOnTheCountersAndOnTheHeap)
{
    // The rewrite's own frame path as PHASES: the table is how a run says "this capability held, and the
    // counters say so" - a phase that rendered correctly while rebuilding everything would otherwise pass
    // every assertion in its own body. These rows are the plan half (recorder + compiler), which is where the
    // per-frame allocations and the counters live; the device half is a phase of its own kind (pixels).
    FrameArena    arena(128 * 1024);
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder(arena, diagnostics, observe);
    FrameCompiler compiler(arena, diagnostics, observe);

    vine::intrusive_ptr<RenderTarget> first(new RenderTarget());
    vine::intrusive_ptr<RenderTarget> second(new RenderTarget());

    vine::vsg::core::TargetFacts first_facts;
    first_facts.target        = first.get();
    first_facts.wanted.width  = 64;
    first_facts.wanted.height = 64;
    first_facts.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
    first_facts.current.desc  = first_facts.wanted;
    first_facts.current.built = true;
    vine::vsg::core::TargetFacts second_facts = first_facts;
    second_facts.target                       = second.get();

    const std::vector<RenderCommand> one_draw{ RenderCommand{} };
    const std::vector<vine::vsg::core::TargetFacts> one_target{ first_facts };

    // One frame, recorded the way the API layer records it: one pass, two content draws into the same target.
    // `swapBuffers()` is the contract's last call of a frame - it closes the books the next `beginFrame()`
    // would otherwise find open (a phase that forgot it would stop at the first ASSERT below).
    const auto record_frame = [&](std::uint64_t token, const void* target, int draws) {
        EXPECT_TRUE(recorder.beginFrame(FrameToken{ token }));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(target));
        for (int index = 0; index < draws; ++index)
        {
            EXPECT_TRUE(recorder.render(one_draw, nullptr));
        }
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());
    };
    const auto compile_frame = [&](const std::vector<vine::vsg::core::TargetFacts>& facts) {
        return &compiler.compile(recorder.description(), vine::vsg::core::FrameFacts{ facts });
    };

    // Warm-up: the plan path grows its own storage over the first frames (measured: the second compile
    // allocates 32 bytes, every frame after it nothing), and "steady" is precisely the claim - a frame whose
    // content did not change against an earlier one. Warming up three frames and gating the ones after that
    // is what makes the two windows below a statement about the steady frame and not about glibc.
    for (std::uint64_t warm_up = 1U; warm_up <= 3U; ++warm_up)
    {
        record_frame(warm_up, first.get(), 2);
        ASSERT_NE(compile_frame(one_target), nullptr);
    }

    // TWO consecutive steady frames, each in its own window: one free frame after a warm-up is a fact about
    // that frame; two in a row is the rule. Both halves of the gate are read: the heap reading (where the
    // platform has one) and the COUNT, which needs no C library and sees the churn the byte reading cannot.
    ASSERT_TRUE(AllocationGate::countsAvailable())
        << "the steady-frame rule must be measured by a count: a phase that reads bytes alone asserts nothing "
           "on a platform whose C library does not report heap usage (see AllocationGate's file note)";
    const bool          heap_gated = AllocationGate::supported();
    std::ptrdiff_t      grew_first = 0;
    std::ptrdiff_t      grew_second = 0;
    std::uint64_t       counted_first = 0;
    std::uint64_t       counted_second = 0;
    std::size_t         arena_first = 0;
    std::size_t         arena_second = 0;
    for (std::uint64_t token = 100U; token <= 101U; ++token)
    {
        AllocationGate gate;
        gate.begin();
        record_frame(token, first.get(), 2);
        compile_frame(one_target);
        const std::ptrdiff_t grew = gate.end();
        const std::uint64_t  counted = gate.allocations();
        const std::size_t    allocated = arena.allocations();
        if (token == 100U)
        {
            grew_first    = grew;
            counted_first = counted;
            arena_first   = allocated;
        }
        else
        {
            grew_second    = grew;
            counted_second = counted;
            arena_second   = allocated;
        }
    }

    // A frame whose two passes READ each other's targets: a cycle. The compiler must skip the whole component
    // and COUNT it - "validation clean, one pass missing" is the failure the rule replaces. Both passes draw,
    // because a pass with neither a draw nor a clear is not pass at all and would be dropped for that reason
    // (which is how this row once passed while counting nothing).
    const auto cycles = [&] {
        EXPECT_TRUE(recorder.beginFrame(FrameToken{ 200U }));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(first.get()));
        EXPECT_TRUE(recorder.setPassInputs(std::vector<RenderTarget*>{ second.get() }));
        EXPECT_TRUE(recorder.render(one_draw, nullptr));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.beginPass(2U));
        EXPECT_TRUE(recorder.setRenderTarget(second.get()));
        EXPECT_TRUE(recorder.setPassInputs(std::vector<RenderTarget*>{ first.get() }));
        EXPECT_TRUE(recorder.render(one_draw, nullptr));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());
        const auto* cycle_frame = compile_frame({ first_facts, second_facts });
        return cycle_frame != nullptr && cycle_frame->passes.empty();
    };

    PhaseTable table;
    table.add(Phase{ "two steady frames allocate nothing",
                     [&]() {
                         // The COUNT is the verdict (it is the half that always exists); the byte reading is
                         // the second opinion where the platform has one.
                         const bool counted_nothing = counted_first == 0U && counted_second == 0U;
                         const bool heap_flat = !heap_gated || (grew_first == 0 && grew_second == 0);
                         return counted_nothing && heap_flat && arena_first == 0U && arena_second == 0U;
                     } });
    table.add(Phase{
        "counters move by the frame's own shape",
        [&]() {
            record_frame(102U, first.get(), 2);
            const auto* steady = compile_frame(one_target);
            return steady != nullptr && steady->passes.size() == 1U;
        },
        [&]() { return static_cast<std::uint64_t>(observe.counters().draws); },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 2U; },
    });
    table.add(Phase{
        "a cycle is skipped and counted",
        [&]() { return cycles(); },
        [&]() { return static_cast<std::uint64_t>(observe.counters().invalid_schedules); },
        [](std::uint64_t before, std::uint64_t after) { return after == before + 1U; },
    });

    const vine::vsg::core::PhaseTable::Report report = table.runAll();
    for (const std::string& line : report.lines)
    {
        std::cout << line << '\n';  // the evidence line format, printed so a script can freeze it
    }

    EXPECT_TRUE(report.ok()) << "every phase has to pass before this run claims anything";
    EXPECT_EQ(report.passed, 3U);
    EXPECT_EQ(report.failed, 0U);

    // The BASELINE: these lines are the rewrite's own claim, in the format the legacy self-test uses, so a
    // phase that disappears or gets renamed is a diff here and not a silent gap in the list.
    const std::vector<std::string> baseline{
        "[selftest] two steady frames allocate nothing",
        "[selftest] counters move by the frame's own shape",
        "[selftest] a cycle is skipped and counted",
        "[selftest] done",
    };
    EXPECT_EQ(report.lines, baseline) << "the phase list IS the capability list: a diff here is a phase that "
                                         "appeared, vanished or changed name";
}
