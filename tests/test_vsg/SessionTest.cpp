/**
 * @brief The session: bring-up, empty frames, and the completion evidence that decides when a parked
 * object may die.
 *
 * The case this file exists for is the last one: a session parks an object during frame 1, and the test
 * proves it is NOT released until the frame whose submission waits the fence of the slot that recorded
 * frame 1 - which is the whole "completion is evidence, not a guess" claim of the design (D5), exercised
 * against a real device and a real swapchain instead of against a mock timeline.
 *
 * It needs a window system and a usable device, so it SKIPS on a machine without either and never fails
 * for an environment it cannot control. Under the repository's own gate (lavapipe + validation) it runs
 * for real.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <vine/graphics/RenderDiagnostic.hpp>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/SessionContent.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/SlotProbe.hpp>

#include "FailingStepNode.hpp"

using vine::graphics::RenderTarget;
using vine::vsg::OffscreenTarget;
using vine::vsg::VsgExecutor;
using vine::vsg::api::Session;
using vine::vsg::api::SessionOptions;
using vine::vsg::api::probePhysicalDevices;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameTimeline;
using vine::vsg::core::FrameToken;
using vine::vsg::core::Observe;
using vine::vsg::core::RetirementQueue;
using vine::vsg::core::Rgba8;
using vine::vsg::core::TargetAction;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::TargetShape;

namespace
{

/// @brief Prints a `vine::String` (UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vine::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// @brief The 8-bit value a clear colour quantises to (UNORM conversion, round to nearest).
std::uint8_t quantise(float value)
{
    return static_cast<std::uint8_t>(std::lround(value * 255.0F));
}

/** @brief Sets or clears the environment variable the profile switch is read from.
 *
 * The switch itself is read with std::getenv() by the code under test, so this has to write the environment
 * the running process sees: setenv()/unsetenv() on POSIX, _putenv_s() on Windows (which updates what
 * getenv() reads in the same CRT).
 *
 * @param name Variable name.
 * @param on   true to set it to "1", false to clear it.
 */
void setEnvironmentFlag(const char* name, bool on)
{
#if defined(_WIN32)
    (void)::_putenv_s(name, on ? "1" : "");
#else
    if (on)
    {
        setenv(name, "1", 1);
    }
    else
    {
        unsetenv(name);
    }
#endif
}

}  // namespace

TEST(SessionTest, EmptyFramesAreCommittedAndAParkedObjectWaitsForTheCompletionEvidence)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    vine::vsg::core::Diagnostics diagnostics;
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[session-test] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    Session session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));
    EXPECT_TRUE(session.initialized());
    EXPECT_EQ(session.slots(), 0U) << "nothing may be assumed before the count is learned";
    EXPECT_FALSE(session.retirement().parkingAvailable())
        << "a session does not park on an assumed window: the count is learned, not guessed";

    const auto frame = [&session] {
        EXPECT_TRUE(session.beginFrame());
        EXPECT_TRUE(session.commitFrame());
    };

    // The count is learned OVER FRAMES, not in one probe: the framework fills one entry of its slot table
    // per frame, so a single answer is 1 and only the answer that stops growing is the real count. Until
    // then the session parks nothing - a 1-frame window would destroy an object a submitted command buffer
    // still names (see SlotProbe).
    std::size_t frames_to_learn = 0;
    while (!session.retirement().parkingAvailable() && frames_to_learn < 16)
    {
        frame();
        ++frames_to_learn;
    }
    ASSERT_TRUE(session.retirement().parkingAvailable()) << "the session never learned its in-flight slot count";
    EXPECT_GE(session.slots(), 1U);
    EXPECT_GE(frames_to_learn, 2U) << "one probe is not enough to learn the count";

    // Now the window is known, so the retirement timing can be pinned for real. Frame S+1 replaces an
    // object: it is parked during the frame and dated against the frames submitted BEFORE it.
    int                 releases  = 0;
    EXPECT_TRUE(session.beginFrame());
    const std::uint64_t retire_at = session.retirement().retirePoint(session.timeline());
    EXPECT_TRUE(session.retirement().retire(session.timeline(), [&releases] { ++releases; }));
    EXPECT_TRUE(session.commitFrame());

    // Completion arrives with the frame whose submission waits the fence of the slot that recorded the
    // frame the object was parked in: submitting frame F proves F - slots finished, so the object may die
    // at F = retire_at + slots.
    const std::uint64_t retiring_frame = retire_at + session.slots();
    while (session.framesPresented() + 1 < retiring_frame)
    {
        frame();
    }
    EXPECT_EQ(releases, 0) << "released before the completion evidence arrived";

    frame();
    EXPECT_EQ(releases, 1) << "the slot that recorded that frame has been recycled: the object is safe to release";

    // Empty frames were submitted and presented the whole time, and none of it cost a device wait.
    EXPECT_EQ(session.framesPresented(), retiring_frame);
    EXPECT_EQ(session.deviceWaits(), 0U);

    // The only thing a healthy session may report is the slot-count note, and only when the learned count
    // differs from the one this code was written against.
    if (session.slots() == vine::vsg::core::kAssumedInFlightSlots)
    {
        EXPECT_TRUE(diagnostics.clean()) << "learning the expected count is not an event";
    }
    else
    {
        EXPECT_EQ(diagnostics.total(), 1U) << "a changed slot count is reported exactly once";
    }

    session.shutdown();
    EXPECT_FALSE(session.initialized());
    session.shutdown();  // idempotent, and safe after a live session
    EXPECT_EQ(session.slots(), 0U);
}

TEST(SessionTest, AGraphHandedOverWhileASubmissionIsInFlightIsWhatTheNextCommitSubmits)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    Diagnostics diagnostics;
    Session     session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));

    // Two off-screen targets, one colour each - so WHICH graph the session submitted is readable: each frame
    // hands over its own graph (one per frame, see makeFrameGraph) and writes one of them with its own clear.
    const ::vsg::ref_ptr<::vsg::Device> device = vine::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(device, nullptr);
    auto first_target = OffscreenTarget::create(device, OffscreenTarget::Layout{ 8U, 8U, { 0.2F, 0.0F, 0.0F, 1.0F } });
    auto second_target = OffscreenTarget::create(device, OffscreenTarget::Layout{ 8U, 8U, { 0.6F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(first_target, nullptr);
    ASSERT_NE(second_target, nullptr);

    vine::vsg::WindowTarget* window = vine::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window, nullptr);
    VsgExecutor executor(diagnostics);
    executor.setWindow(window);
    executor.addTarget(first_target.get(), first_target.get());
    executor.addTarget(second_target.get(), second_target.get());

    FrameArena    arena{ 64 * 1024 };
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    ClearPolicy window_clear;
    window_clear.color          = true;
    window_clear.color_value[3] = 1.0F;

    // One frame, driven the way the frame drive does it: open the session's frame, record a window pass and
    // (when asked) one off-screen pass, compile, hand the session THIS frame's graph, and commit. Every
    // handover happens while the previous frame's submission is still in flight - nothing here waits the
    // device - which is exactly what the session has to survive.
    const auto drive = [&](OffscreenTarget* into, float value) {
        const FrameToken token = session.beginFrame();
        EXPECT_TRUE(token);

        ClearPolicy clear;
        clear.color          = true;
        clear.color_value[0] = value;
        clear.color_value[3] = 1.0F;

        EXPECT_TRUE(recorder.beginFrame(token));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(nullptr));
        EXPECT_TRUE(recorder.setClearPolicy(window_clear));
        EXPECT_TRUE(recorder.endPass());
        if (into != nullptr)
        {
            EXPECT_TRUE(recorder.beginPass(2U));
            EXPECT_TRUE(recorder.setRenderTarget(into));
            EXPECT_TRUE(recorder.setClearPolicy(clear));
            EXPECT_TRUE(recorder.endPass());
        }
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());

        std::vector<TargetFacts> table(1U);
        table[0] = window->facts();
        if (into != nullptr)
        {
            TargetFacts offscreen;
            offscreen.target        = into;
            offscreen.wanted.width  = 8;
            offscreen.wanted.height = 8;
            offscreen.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
            offscreen.current = into->instance();
            table.push_back(offscreen);
        }
        const CompiledFrame& compiled = compiler.compile(recorder.description(), FrameFacts{ table });

        const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
            vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
        EXPECT_NE(graph, nullptr);
        EXPECT_TRUE(executor.record(compiled, graph));
        EXPECT_EQ(executor.skipped(), 0U);
        EXPECT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ graph }));
        return executor.commit(compiled, session);
    };

    EXPECT_TRUE(drive(first_target.get(), 0.2F));
    EXPECT_TRUE(drive(second_target.get(), 0.6F));
    EXPECT_TRUE(drive(first_target.get(), 0.4F));

    // The last three frames write the window only, and that is not decoration: a frame waits the fence of the
    // slot it enters, so the frames that follow are what PROVE the first three have finished - which is what
    // makes reading their pixels evidence instead of a race.
    EXPECT_TRUE(drive(nullptr, 0.0F));
    EXPECT_TRUE(drive(nullptr, 0.0F));
    EXPECT_TRUE(drive(nullptr, 0.0F));

    EXPECT_EQ(session.framesPresented(), 6U) << "every one of the six submissions reached the queue";
    EXPECT_EQ(session.lostFrames(), 0U);
    EXPECT_EQ(session.deviceWaits(), 0U) << "handing graphs over must not need a device idle";
    EXPECT_TRUE(diagnostics.clean()) << "nothing about the handovers is an event";
    {
        // The graphs that were SUBMITTED are the ones handed over per frame: a session that kept the first
        // frame's graph (or the one before it) would show the earlier clear here.
        const vine::vsg::core::PixelProbe first = first_target->probe();
        ASSERT_TRUE(first.valid());
        EXPECT_NEAR(first.pixel(4, 4).r, quantise(0.4F), 1) << "the third frame's graph is what ran last for it";
        const vine::vsg::core::PixelProbe second = second_target->probe();
        ASSERT_TRUE(second.valid());
        EXPECT_NEAR(second.pixel(4, 4).r, quantise(0.6F), 1);
    }

    session.shutdown();
}

TEST(SessionTest, ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    ClearPolicy window_clear;
    window_clear.color          = true;
    window_clear.color_value[3] = 1.0F;
    ClearPolicy target_clear;
    target_clear.color          = true;
    target_clear.color_value[0] = 0.25F;
    target_clear.color_value[1] = 0.5F;
    target_clear.color_value[2] = 0.75F;
    target_clear.color_value[3] = 1.0F;

    Diagnostics diagnostics;
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[drive-test] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });

    // Frame 1: the happy path, on a session of its own - committed through the drive, and the pixels are the
    // evidence that the off-screen pass really reached the device through the session's submission.
    {
        Session session;
        ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));

        // The frame drive of the window path: one executor that holds the window and the off-screen target,
        // so every target the plan can resolve to is one it knows.
        const ::vsg::ref_ptr<::vsg::Device> device = vine::vsg::detail::SessionContentAccess::device(session);
        ASSERT_NE(device, nullptr);
        auto target = OffscreenTarget::create(device,
                                              OffscreenTarget::Layout{ 8U, 8U, { 0.25F, 0.5F, 0.75F, 1.0F } });
        ASSERT_NE(target, nullptr);
        vine::vsg::WindowTarget* window = vine::vsg::detail::SessionContentAccess::windowTarget(session);
        ASSERT_NE(window, nullptr);

        VsgExecutor executor(diagnostics);
        executor.setWindow(window);
        executor.addTarget(target.get(), target.get());

        FrameArena    arena{ 64 * 1024 };
        Observe       observe;
        FrameRecorder recorder{ arena, diagnostics, observe };
        FrameCompiler compiler{ arena, diagnostics, observe };

        // One frame as the drive sees it: a pass into the window (the default framebuffer is a target like
        // any other) and a pass into the off-screen target, compiled from the facts both answer for.
        const auto record_frame = [&](FrameToken token) {
            EXPECT_TRUE(recorder.beginFrame(token));
            EXPECT_TRUE(recorder.beginPass(1U));
            EXPECT_TRUE(recorder.setRenderTarget(nullptr));
            EXPECT_TRUE(recorder.setClearPolicy(window_clear));
            EXPECT_TRUE(recorder.endPass());
            EXPECT_TRUE(recorder.beginPass(2U));
            EXPECT_TRUE(recorder.setRenderTarget(target.get()));
            EXPECT_TRUE(recorder.setClearPolicy(target_clear));
            EXPECT_TRUE(recorder.endPass());
            EXPECT_TRUE(recorder.endFrame());
            EXPECT_TRUE(recorder.swapBuffers());
            TargetFacts offscreen;
            offscreen.target        = target.get();
            offscreen.wanted.width  = 8;
            offscreen.wanted.height = 8;
            offscreen.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
            offscreen.current               = target->instance();
            const std::vector<TargetFacts> table{ window->facts(), offscreen };
            return &compiler.compile(recorder.description(), FrameFacts{ table });
        };

        const FrameToken first_token = session.beginFrame();
        ASSERT_TRUE(first_token);
        const CompiledFrame* first = record_frame(first_token);
        ASSERT_EQ(first->passes.size(), 2U);
        const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
            vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
        ASSERT_NE(graph, nullptr);
        ASSERT_TRUE(executor.record(*first, graph));
        EXPECT_EQ(executor.skipped(), 0U);
        ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ graph }));
        EXPECT_TRUE(executor.commit(*first, session)) << "the drive submits and presents through the session";
        EXPECT_EQ(session.framesPresented(), 1U);
        EXPECT_EQ(session.deviceWaits(), 0U) << "a frame that goes through does not stop the device";
        EXPECT_FALSE(target->instance().attachments_invalidated)
            << "a submission that happened leaves nothing to repair";
        {
            const vine::vsg::core::PixelProbe probe = target->probe();
            ASSERT_TRUE(probe.valid());
            const Rgba8 sampled  = probe.pixel(4, 4);
            const Rgba8 expected{ quantise(0.25F), quantise(0.5F), quantise(0.75F), 255U };
            EXPECT_NEAR(sampled.r, expected.r, 1);
            EXPECT_NEAR(sampled.g, expected.g, 1);
            EXPECT_NEAR(sampled.b, expected.b, 1);
            EXPECT_TRUE(probe.wholeImageMatches(sampled));
        }
        session.shutdown();
    }

    // Frames 2 and 3: a session of its own again, because a lost frame leaves the swapchain holding an image
    // it never presented (see the case above) - which is exactly the state a host rebuilds the session from,
    // not one it keeps driving.
    Session session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));
    const ::vsg::ref_ptr<::vsg::Device> device = vine::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(device, nullptr);
    auto target = OffscreenTarget::create(device,
                                          OffscreenTarget::Layout{ 8U, 8U, { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr);
    vine::vsg::WindowTarget* window = vine::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window, nullptr);

    VsgExecutor executor(diagnostics);
    executor.setWindow(window);
    executor.addTarget(target.get(), target.get());

    FrameArena    arena{ 64 * 1024 };
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const auto record_frame = [&](FrameToken token) {
        EXPECT_TRUE(recorder.beginFrame(token));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(nullptr));
        EXPECT_TRUE(recorder.setClearPolicy(window_clear));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.beginPass(2U));
        EXPECT_TRUE(recorder.setRenderTarget(target.get()));
        EXPECT_TRUE(recorder.setClearPolicy(target_clear));
        EXPECT_TRUE(recorder.endPass());
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());
        TargetFacts offscreen;
        offscreen.target        = target.get();
        offscreen.wanted.width  = 8;
        offscreen.wanted.height = 8;
        offscreen.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
        offscreen.current               = target->instance();
        const std::vector<TargetFacts> table{ window->facts(), offscreen };
        return &compiler.compile(recorder.description(), FrameFacts{ table });
    };

    // Frame 2: the submission is made to fail. One call, and the failure is already the target's fact: the
    // session reports (its side, see the case above), the executor marks (its side), and the caller
    // interprets neither. The failing node sits FIRST, so the failure happens where vsg's own allocation
    // failure would - before any of this frame's passes is recorded.
    const FrameToken second_token = session.beginFrame();
    ASSERT_TRUE(second_token);
    const CompiledFrame* second = record_frame(second_token);
    const auto failing_graph = vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(failing_graph, nullptr);
    ::vsg::ref_ptr<FailingStepNode> failing(new FailingStepNode());
    failing_graph->addChild(failing);
    ASSERT_TRUE(executor.record(*second, failing_graph));
    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(
        session, ::vsg::CommandGraphs{ failing_graph }));
    failing->armed = true;

    const std::uint64_t failures_before =
        diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed);
    EXPECT_FALSE(executor.commit(*second, session)) << "the submission did not happen, and the drive says so";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed), failures_before + 1U)
        << "the session is the reporter on this path";
    EXPECT_TRUE(target->instance().attachments_invalidated)
        << "the target this frame wrote holds contents nobody can vouch for";
    EXPECT_EQ(session.framesPresented(), 0U) << "nothing was presented for the lost frame";
    EXPECT_EQ(session.lostFrames(), 1U);

    // Frame 3: the SAME session serves the next frame, because a lost frame is survivable here - the
    // session rebuilt the swapchain for exactly the image this frame acquired and never presented (one
    // counted device idle). The plan reads the mark: the pass that writes the off-screen target bootstraps
    // (it clears what nobody can vouch for) and the window pass does not - the frame's window write belongs
    // to the presentation, not to this target's contents.
    const auto drive = [&](bool with_offscreen) -> const CompiledFrame* {
        const FrameToken token = session.beginFrame();
        EXPECT_TRUE(token);
        EXPECT_TRUE(recorder.beginFrame(token));
        EXPECT_TRUE(recorder.beginPass(1U));
        EXPECT_TRUE(recorder.setRenderTarget(nullptr));
        EXPECT_TRUE(recorder.setClearPolicy(window_clear));
        EXPECT_TRUE(recorder.endPass());
        if (with_offscreen)
        {
            EXPECT_TRUE(recorder.beginPass(2U));
            EXPECT_TRUE(recorder.setRenderTarget(target.get()));
            EXPECT_TRUE(recorder.setClearPolicy(target_clear));
            EXPECT_TRUE(recorder.endPass());
        }
        EXPECT_TRUE(recorder.endFrame());
        EXPECT_TRUE(recorder.swapBuffers());

        std::vector<TargetFacts> table(1U);
        table[0] = window->facts();
        if (with_offscreen)
        {
            TargetFacts offscreen;
            offscreen.target        = target.get();
            offscreen.wanted.width  = 8;
            offscreen.wanted.height = 8;
            offscreen.wanted.shape.color_formats.push_back(RenderTarget::ColorFormat::RGBA8);
            offscreen.current = target->instance();
            table.push_back(offscreen);
        }
        const CompiledFrame& compiled = compiler.compile(recorder.description(), FrameFacts{ table });

        const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
            vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
        EXPECT_NE(graph, nullptr);
        EXPECT_TRUE(executor.record(compiled, graph));
        EXPECT_EQ(executor.skipped(), 0U);
        EXPECT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ graph }));
        EXPECT_TRUE(executor.commit(compiled, session)) << "the next frame goes through on the same session";
        return &compiled;
    };

    const CompiledFrame* third = drive(/*with_offscreen*/ true);
    ASSERT_EQ(third->passes.size(), 2U);
    bool offscreen_bootstraps = false;
    bool window_bootstraps    = false;
    for (const vine::vsg::core::CompiledPass& pass : third->passes)
    {
        if (third->targets[pass.target_index].target == target.get())
        {
            offscreen_bootstraps = pass.bootstrap;
        }
        else
        {
            window_bootstraps = pass.bootstrap;
        }
    }
    EXPECT_TRUE(offscreen_bootstraps) << "the plan answers the mark: the pass that writes it clears";
    EXPECT_FALSE(window_bootstraps) << "the window is not implicated by the off-screen target's mark";
    EXPECT_FALSE(target->instance().attachments_invalidated)
        << "the bootstrapping pass repaired it: contents are known again";

    // Four frames that write the window only (one more than the slots vsg keeps in flight, so the healed
    // frame's slot has been re-entered): a frame waits the fence of the slot it enters, so these are what
    // PROVE the healed frame's copy has run - which is what makes reading its pixels evidence, not a race.
    drive(/*with_offscreen*/ false);
    drive(/*with_offscreen*/ false);
    drive(/*with_offscreen*/ false);
    drive(/*with_offscreen*/ false);

    EXPECT_EQ(session.framesPresented(), 5U) << "the lost frame is not a presented one; the five that followed are";
    EXPECT_EQ(session.lostFrames(), 1U);
    EXPECT_EQ(session.deviceWaits(), 1U) << "the swapchain rebuild a lost frame costs is counted, and it is the only one";
    EXPECT_EQ(diagnostics.total(), failures_before + 1U)
        << "the lost frame was the one event, and it was reported exactly once";
    {
        const vine::vsg::core::PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        const Rgba8 sampled  = probe.pixel(4, 4);
        const Rgba8 expected{ quantise(0.25F), quantise(0.5F), quantise(0.75F), 255U };
        EXPECT_NEAR(sampled.r, expected.r, 1) << "the repaired target renders the frame that repaired it";
        EXPECT_NEAR(sampled.g, expected.g, 1);
        EXPECT_NEAR(sampled.b, expected.b, 1);
        EXPECT_TRUE(probe.wholeImageMatches(sampled));
    }

    session.shutdown();
}

TEST(SessionTest, AWindowPlanThatGotTheFormatWrongIsNotRecorded)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    Diagnostics diagnostics;
    Session     session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));

    vine::vsg::WindowTarget* window = vine::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window, nullptr);

    VsgExecutor executor(diagnostics);
    executor.setWindow(window);

    FrameArena    arena{ 64 * 1024 };
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const FrameToken token = session.beginFrame();
    ASSERT_TRUE(token);

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[3] = 1.0F;

    EXPECT_TRUE(recorder.beginFrame(token));
    EXPECT_TRUE(recorder.beginPass(1U));
    EXPECT_TRUE(recorder.setRenderTarget(nullptr));  // the default framebuffer is the window's
    EXPECT_TRUE(recorder.setClearPolicy(clear));
    EXPECT_TRUE(recorder.endPass());
    EXPECT_TRUE(recorder.endFrame());

    // What the window really is, in its own words (its surface format, its depth, and the DEVICE formats
    // both map from - see WindowTarget::facts).
    const std::vector<TargetFacts> truthful{ window->facts() };
    ASSERT_EQ(truthful[0].wanted.shape.color_formats.size(), 1U);

    // ...and a plan told the same COUNT of colour attachments in a different FORMAT: the count-and-depth
    // half of the record step's check cannot see this, and a pipeline keyed on the plan's account would be
    // compiled against a render pass the window does not have.
    std::vector<TargetFacts> drifted = truthful;
    drifted[0].wanted.shape.color_formats[0] = RenderTarget::ColorFormat::RGBA16F;
    drifted[0].current.desc                  = drifted[0].wanted;  // the table lies consistently

    const std::size_t skipped_before = diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped);

    const CompiledFrame& wrong = compiler.compile(recorder.description(), FrameFacts{ drifted });
    ASSERT_EQ(wrong.passes.size(), 1U);
    EXPECT_EQ(wrong.passes[0].color_attachments, 1U) << "the count agrees: only the format drifted";

    const ::vsg::ref_ptr<::vsg::CommandGraph> refused =
        vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(refused, nullptr);
    EXPECT_FALSE(executor.record(wrong, refused)) << "the same count is not the same render pass";
    EXPECT_EQ(executor.skipped(), 1U);
    EXPECT_TRUE(executor.recorded().empty());
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), skipped_before + 1U);

    // Told the truth, the same pass records - and the frame goes on to be presented, so what was wrong was
    // the plan's account of the window, not the window.
    const CompiledFrame& right = compiler.compile(recorder.description(), FrameFacts{ truthful });
    ASSERT_EQ(right.passes.size(), 1U);

    const ::vsg::ref_ptr<::vsg::CommandGraph> served =
        vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(served, nullptr);
    EXPECT_TRUE(executor.record(right, served));
    EXPECT_EQ(executor.skipped(), 0U);
    ASSERT_EQ(executor.recorded().size(), 1U);
    EXPECT_EQ(executor.recorded()[0], 1U);

    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ served }));
    EXPECT_TRUE(session.commitFrame());
    EXPECT_EQ(session.framesPresented(), 1U);
    EXPECT_EQ(session.deviceWaits(), 0U) << "none of this is a reason to stop the device";

    session.shutdown();
}

TEST(SessionTest, AWindowRebuildIsAnsweredByAskingThePlatformNotByBelievingThePlan)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    Diagnostics diagnostics;
    Session     session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));

    vine::vsg::WindowTarget* window = vine::vsg::detail::SessionContentAccess::windowTarget(session);
    ASSERT_NE(window, nullptr);
    const auto device = vine::vsg::detail::SessionContentAccess::device(session);
    ASSERT_NE(device, nullptr);

    VsgExecutor executor(diagnostics);
    executor.setWindow(window);

    FrameArena    arena{ 64 * 1024 };
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    FrameTimeline   timeline;
    RetirementQueue queue(3U);

    const FrameToken token = session.beginFrame();
    ASSERT_TRUE(token);

    ClearPolicy clear;
    clear.color          = true;
    clear.color_value[3] = 1.0F;

    EXPECT_TRUE(recorder.beginFrame(token));
    EXPECT_TRUE(recorder.beginPass(1U));
    EXPECT_TRUE(recorder.setRenderTarget(nullptr));  // the default framebuffer is the window's
    EXPECT_TRUE(recorder.setClearPolicy(clear));
    EXPECT_TRUE(recorder.endPass());
    EXPECT_TRUE(recorder.endFrame());

    // What the swapchain serves now, and a second DEVICE format a plan could claim it serves instead - taken
    // from a real target, because the layer compares format codes and never invents them.
    const TargetShape live = window->facts().wanted.shape;
    ASSERT_EQ(live.device_color_formats.size(), 1U);
    auto other_target = OffscreenTarget::create(device, OffscreenTarget::Layout{ 8U, 8U, { 0.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(other_target, nullptr);
    const std::uint32_t other_format = other_target->shape().device_color_formats.front();
    ASSERT_NE(other_format, live.device_color_formats.front())
        << "the case needs two device formats to tell apart (an off-screen RGBA8 against the surface's own)";
    EXPECT_FALSE(window->refresh()) << "no host path changes the swapchain's shape under a live session";

    // (1) A truthful account: the plan has nothing to answer for the window, nothing is applied, and the
    // frame records.
    const std::vector<TargetFacts> truthful{ window->facts() };
    const CompiledFrame& steady = compiler.compile(recorder.description(), FrameFacts{ truthful });
    ASSERT_EQ(steady.passes.size(), 1U);
    ASSERT_EQ(steady.targets.size(), 1U);
    EXPECT_EQ(static_cast<int>(steady.targets[0].decision.action), static_cast<int>(TargetAction::None));
    {
        const VsgExecutor::TargetApplications applied =
            executor.applyTargetPlans(steady, truthful, timeline, queue);
        EXPECT_EQ(applied.resized + applied.rebuilt + applied.refused + applied.failed, 0U)
            << "a steady window has no answer to apply";
    }
    {
        const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
            vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
        ASSERT_NE(graph, nullptr);
        EXPECT_TRUE(executor.record(steady, graph));
        EXPECT_EQ(executor.skipped(), 0U);
    }

    // (2) A plan TOLD the swapchain serves another format: the plan answers Rebuild, and the executor asks the
    // platform - which says it has not changed. Nothing is applied, the claim is not adopted into the target,
    // and the pass that relied on it is refused: an unapplied rebuild is never recorded.
    std::vector<TargetFacts> claimed = truthful;
    claimed[0].wanted.shape.device_color_formats = { other_format };

    const std::size_t skipped_before = diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped);

    const CompiledFrame& rebuild = compiler.compile(recorder.description(), FrameFacts{ claimed });
    ASSERT_EQ(rebuild.passes.size(), 1U);
    EXPECT_EQ(static_cast<int>(rebuild.targets[0].decision.action), static_cast<int>(TargetAction::Rebuild))
        << "the plan was told a shape the window does not have";
    {
        const VsgExecutor::TargetApplications applied =
            executor.applyTargetPlans(rebuild, claimed, timeline, queue);
        EXPECT_EQ(applied.rebuilt, 0U) << "the platform did not change the swapchain";
        EXPECT_EQ(applied.failed, 1U) << "the claim was not applied: the platform says otherwise";
        EXPECT_EQ(applied.resized + applied.refused, 0U);
    }
    EXPECT_EQ(window->shape().device_color_formats.front(), live.device_color_formats.front())
        << "the target still reports what the swapchain serves: the plan's claim was not adopted";
    EXPECT_FALSE(window->refresh());
    {
        const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
            vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
        ASSERT_NE(graph, nullptr);
        EXPECT_FALSE(executor.record(rebuild, graph)) << "an unapplied rebuild is not recorded";
        EXPECT_EQ(executor.skipped(), 1U);
        EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), skipped_before + 1U);
    }

    // (3) The window is still usable: told the truth, the same pass records - and the frame is presented.
    const CompiledFrame& again = compiler.compile(recorder.description(), FrameFacts{ truthful });
    ASSERT_EQ(again.passes.size(), 1U);
    const ::vsg::ref_ptr<::vsg::CommandGraph> served =
        vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(served, nullptr);
    EXPECT_TRUE(executor.record(again, served));
    EXPECT_EQ(executor.skipped(), 0U);
    ASSERT_EQ(executor.recorded().size(), 1U);
    EXPECT_EQ(executor.recorded()[0], 1U);

    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ served }));
    EXPECT_TRUE(session.commitFrame());
    EXPECT_EQ(session.framesPresented(), 1U);
    EXPECT_EQ(session.deviceWaits(), 0U) << "none of this is a reason to stop the device";

    session.shutdown();
}

TEST(SessionTest, ACommitWhoseSubmissionFailsSaysSoAndTheFrameIsStillOver)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    vine::vsg::core::Diagnostics diagnostics;
    Session                        session;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));

    // The session's own frame graph, with a node that fails the submission's record step on command: a
    // queue submit the driver refuses cannot be summoned on lavapipe, and vsg's own failure vocabulary for
    // "this step cannot be made" is an exception (see FailingStepNode).
    const ::vsg::ref_ptr<::vsg::CommandGraph> graph =
        vine::vsg::detail::SessionContentAccess::makeFrameGraph(session);
    ASSERT_NE(graph, nullptr);
    ::vsg::ref_ptr<FailingStepNode> failing(new FailingStepNode());
    graph->addChild(failing);
    ASSERT_TRUE(vine::vsg::detail::SessionContentAccess::assignFrameGraphs(session, ::vsg::CommandGraphs{ graph }));

    ASSERT_TRUE(session.beginFrame());
    failing->armed = true;

    const std::uint64_t failures_before =
        diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed);
    EXPECT_FALSE(session.commitFrame()) << "the step did not happen, and the commit says so";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::SubmissionFailed), failures_before + 1U)
        << "reported through the one route, with the category a host can switch on";
    EXPECT_EQ(session.lostFrames(), 1U);
    EXPECT_EQ(session.framesPresented(), 0U) << "nothing was presented, so nothing may be counted as presented";
    EXPECT_EQ(session.timeline().submittedFrame(), 0U)
        << "the submitted watermark counts SUBMISSIONS: a frame that never reached the queue is not one";
    EXPECT_FALSE(session.timeline().hasOpenFrame()) << "the frame is over either way: it cannot be retried";

    // The frame numbers are the SUBMISSION clock, so the frame that never reached the queue is not a frame
    // for the timeline - which is the whole reason the watermark is separate from "frames opened".
    EXPECT_EQ(session.timeline().current().frame, 0U) << "and no frame is open after the commit";

    // What a host does next is what a host does after any frame it cannot submit: the swapchain image the
    // frame acquired was never presented, so this session is not driven again as it is - it is built again
    // (the same path a moved surface takes), and the new session's frames present normally.
    failing->armed = false;
    ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));
    ASSERT_TRUE(session.beginFrame());
    EXPECT_TRUE(session.commitFrame()) << "a session that came up again submits and presents";
    EXPECT_EQ(session.framesPresented(), 1U);
    EXPECT_EQ(session.timeline().submittedFrame(), 1U);

    session.shutdown();
}

TEST(SessionTest, TheProfileIsTheEnvironmentsSwitchAndReadingItNeverStopsTheDevice)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system: a session that owns its window cannot come up";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    vine::vsg::core::Diagnostics diagnostics;
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic& diagnostic) {
        std::printf("[session-test] diagnostic: severity=%d category=%d message=%s\n",
                    static_cast<int>(diagnostic.severity), static_cast<int>(diagnostic.category),
                    as_bytes(diagnostic.message).c_str());
    });
    const auto frame = [](Session& session) {
        EXPECT_TRUE(session.beginFrame());
        EXPECT_TRUE(session.commitFrame());
    };

    // OFF (the default): nothing is measured, and a read says so instead of reporting zeros that would look
    // like a frame that cost nothing.
    {
        Session session;
        ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));
        EXPECT_FALSE(session.profiling());
        for (int index = 0; index < 4; ++index)
        {
            frame(session);
        }
        const Session::GpuProfile profile = session.gpuProfile();
        EXPECT_FALSE(profile.enabled);
        EXPECT_FALSE(profile.readable);
        EXPECT_TRUE(profile.passes.empty());
        EXPECT_EQ(session.deviceWaits(), 0U) << "a session that measures nothing must not idle the device";
    }

    // ON: the switch is the environment's, read once when the session comes up (and restored here, so the
    // rest of the process - and the next session - sees what it saw before).
    setEnvironmentFlag("VINE_VSG_PROFILE", true);
    {
        Session session;
        ASSERT_TRUE(session.initialize(SessionOptions{}, diagnostics));
        EXPECT_TRUE(session.profiling());

        // Frames first, then the read: the profiler's log only holds frames whose timestamps are back.
        for (int index = 0; index < 8; ++index)
        {
            frame(session);
        }

        const std::size_t      waits_before = session.deviceWaits();
        const Session::GpuProfile profile   = session.gpuProfile();
        EXPECT_EQ(session.deviceWaits(), waits_before)
            << "reading the profile must not stop the device: zero frames of lag would mean a blocking read";

        EXPECT_TRUE(profile.enabled);
        if (!profile.timestamps_available)
        {
            GTEST_SKIP() << "this device cannot write timestamps";
        }
        ASSERT_TRUE(profile.readable)
            << "eight committed frames with timestamps: the log must hold results, or the read is "
               "looking in the wrong place";
        EXPECT_LT(profile.age_frames, 8U) << "a result that old cannot be the newest one";
        {
            // The numbers are a measurement: non-negative and attributable is all a test may claim - and
            // nothing here is wrapped (the executor wraps passes, and this session has none), so the frame
            // interval is what a lone session can report.
            EXPECT_GE(profile.frame_gpu_ms, 0.0);
            for (const Session::GpuSample& sample : profile.passes)
            {
                EXPECT_NE(sample.key, nullptr) << "a sample without a key cannot be attributed at all";
                EXPECT_GE(sample.gpu_ms, 0.0);
            }
        }
    }
    setEnvironmentFlag("VINE_VSG_PROFILE", false);
}
