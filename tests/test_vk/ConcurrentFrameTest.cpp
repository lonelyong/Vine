/**
 * @brief Two threads driving ONE device: the frame gate serialises them, and both pictures come out right.
 *
 * WHY THIS FILE EXISTS. `graphics-threading.md` C21 says a device drives one frame at a time, and the audit (A9) said
 * that claim had no evidence. `FrameGate` makes it a checked fact; this case is the integration half - two threads,
 * one device, real frames, each clearing its own target to its own colour - so "the second thread waits" is observed
 * as "nothing was refused, nothing was mixed up" rather than as a comment. `HeadlessBackendTest`'s
 * `OneDeviceServesSeveralAreasWithoutCrossingThem` is the SEQUENTIAL form of the same statement.
 */

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderFrameExecutor.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::ColorReadback;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::PresentOutcome;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderFrameExecutor;
using vn::graphics::RenderTarget;

namespace
{

constexpr int kTargetSize = 32;
constexpr int kFrames     = 8;

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const char8_t* name)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(name));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief One frame with one pass clearing @p target to @p colour and drawing nothing.
[[nodiscard]] FrameDescription clearingFrame(RenderTarget* target, const Colorf& colour)
{
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();

    vn::graphics::PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = colour;
    entry.draw        = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

} // namespace

TEST(VkConcurrentFrameTest, TwoThreadsDriveOneDeviceAndBothPicturesAreRight)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target_a = makeTarget(u8"vk_concurrent_a");
    const intrusive_ptr<RenderTarget> target_b = makeTarget(u8"vk_concurrent_b");

    std::atomic<int>  ready{ 0 };
    std::atomic<bool> go{ false };
    std::atomic<int>  refused{ 0 };

    const auto drive = [&](RenderTarget* target, const Colorf colour) {
        const FrameDescription frame = clearingFrame(target, colour);
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }
        for (int index = 0; index < kFrames; ++index) {
            if (backend->executeFrame(frame, nullptr) == PresentOutcome::Refused) {
                refused.fetch_add(1);
            }
        }
    };

    std::thread first(drive, target_a.get(), Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    std::thread second(drive, target_b.get(), Colorf(0.0F, 1.0F, 0.0F, 1.0F));
    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    go.store(true);
    first.join();
    second.join();

    // THE SECOND THREAD WAITED INSTEAD OF BEING TREATED AS A RE-ENTRY: `Refused` is the answer reserved for the
    // thread that already drives a frame, and a mutex that refused instead of waiting would show up here.
    EXPECT_EQ(refused.load(), 0) << "the gate refused a frame from another thread instead of waiting";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::PassProtocolViolation), 0U)
        << "two threads were reported as a re-entry";
    EXPECT_TRUE(backend->initialized()) << "the session did not survive the concurrent frames";

    // AND EACH PICTURE IS THE ONE ITS OWN THREAD DREW: serialising must not mix the frames up (that would be the
    // symptom of a shared command buffer carrying the other thread's pass).
    const ColorReadback red = backend->readColorImage(target_a.get(), 0);
    ASSERT_TRUE(red.ok());
    ASSERT_GE(red.values.size(), 4U);
    EXPECT_EQ(red.values[0], 255) << "the first target is not the red its thread cleared it to";
    EXPECT_EQ(red.values[1], 0);

    const ColorReadback green = backend->readColorImage(target_b.get(), 0);
    ASSERT_TRUE(green.ok());
    ASSERT_GE(green.values.size(), 4U);
    EXPECT_EQ(green.values[0], 0) << "the second target is not the green its thread cleared it to";
    EXPECT_EQ(green.values[1], 255);

    backend->shutdown();
}

TEST(VkConcurrentFrameTest, AHostVerbCalledFromInsideAFrameIsRefusedInsteadOfHanging)
{
    // THE GATE'S OTHER HALF (see graphics::FrameGate and Impl::body_gate): a host verb called from the thread that is
    // inside a frame gets an EMPTY lease, answers its refusal value, and says why ONCE. Before the gate could answer
    // "no", this call was a deadlock - the thread waiting for the frame it was itself inside. A diagnostic sink is the
    // reachable way to make that call: it runs on the frame's own thread, in the middle of the frame body.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    std::vector<vn::graphics::RenderDiagnostic> reported;
    std::optional<std::uint64_t>                pipelines_from_inside;
    backend->setDiagnosticSink([&](const vn::graphics::RenderDiagnostic& diagnostic) {
        reported.push_back(diagnostic);
        if (!pipelines_from_inside.has_value()) {
            // A HOST VERB, from the frame body's thread. It answers its refusal value instead of waiting for itself.
            pipelines_from_inside = device->contentPipelines();
        }
    });

    // A FRAME THAT REPORTS SOMETHING INSIDE ITS BODY: a pass entry with no drawing leaves nothing this backend can
    // run, which it says once (see vk.content.no_runnable_pass) - while the frame body holds the gate.
    FrameDescription       frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    frame.passes.push_back(std::move(entry));

    EXPECT_EQ(backend->executeFrame(frame, nullptr), PresentOutcome::Skipped) << "the frame with nothing to run was not skipped";
    ASSERT_TRUE(pipelines_from_inside.has_value()) << "the sink never ran inside a frame";
    EXPECT_EQ(*pipelines_from_inside, 0U) << "a host verb from inside a frame answered a real number instead of refusing";

    const auto has_code = [&reported](const char8_t* code) {
        for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
            if (diagnostic.code == String(code)) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(has_code(u8"vk.device.reentry")) << "the refusal was silent: a host has to hear it once";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::PassProtocolViolation), 1U)
        << "the re-entry was reported once, not per verb";
    EXPECT_TRUE(backend->initialized()) << "the device did not survive a verb called from inside its own frame";

    backend->shutdown();
}

/*
 * @brief The frame report is the frame that ENDED, whole, while the next one runs.
 *
 * The report used to read the driver's live books - the very value the frame body increments as it goes - so a host
 * asking "how did it go" while a frame ran either raced it (the numbers were not published as thread-safe) or had to
 * know a rule the interface never stated: "read it after the frame is answered". The books are the frame's ANSWER now:
 * `closeFrameBooks` publishes them under a lock, and a reader sees a closed value (the same shape `PresentRecord` and
 * the per-pass costs already had). Two threads make that visible, and the TSan run over this case is the witness that
 * the two are no longer one object.
 */
TEST(VkConcurrentFrameTest, TheFrameReportIsTheFrameThatEndedWhileTheNextOneRuns)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target = makeTarget(u8"vk_report");
    const FrameDescription            frame  = clearingFrame(target.get(), Colorf(0.0F, 0.0F, 1.0F, 1.0F));

    // THE DEVICE'S OWN DRIVER: what a RenderEngine drives frames through, and the object that keeps the books.
    RenderFrameExecutor& executor = backend->executor();

    std::atomic<bool> stop{ false };
    std::atomic<int>  frames_run{ 0 };
    std::thread       driver([&] {
        while (!stop.load()) {
            (void)executor.executeFrame(frame, nullptr);
            frames_run.fetch_add(1);
        }
    });

    // THE READER, WHILE FRAMES ARE BEING RECORDED. The parts of the report that only a CLOSED frame can carry - what it
    // uploaded, what it spent on the content lock - are published together, so a reader either sees none of them (no
    // frame has ended) or both of them (one frame, whole). A value stitched from two frames, or read out of the books
    // while they are being written, is what breaks that; the race itself is what the TSan run over this case checks.
    std::uint64_t observations = 0;
    std::uint64_t whole        = 0;
    for (std::uint64_t attempt = 0; whole < 8U && attempt < 100000U; ++attempt) {
        const FrameReport report = executor.frameReport();
        ++observations;
        const bool answers = report.uploads.has_value();
        EXPECT_EQ(report.locking.has_value(), answers) << "report " << observations << " saw half a frame";
        if (answers) {
            ++whole;
        }
    }
    stop.store(true);
    driver.join();

    EXPECT_GT(frames_run.load(), 0) << "the driver never ran a frame";
    EXPECT_GT(whole, 0U) << "no report was read while frames were running (the case is built on that)";

    const FrameReport after = executor.frameReport();
    ASSERT_TRUE(after.uploads.has_value()) << "the last frame that ended is the report";
    EXPECT_TRUE(after.locking.has_value());

    backend->shutdown();
}
