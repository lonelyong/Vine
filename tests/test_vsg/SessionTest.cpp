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

#include <cstdint>
#include <cstdlib>
#include <string>

#include <vine/graphics/RenderDiagnostic.hpp>

#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/SlotProbe.hpp>

using vine::vsg::api::Session;
using vine::vsg::api::SessionOptions;
using vine::vsg::api::probePhysicalDevices;

namespace
{

/// @brief Prints a `vine::String` (UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vine::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
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
    ::setenv("VINE_VSG_PROFILE", "1", 1);
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
    ::unsetenv("VINE_VSG_PROFILE");
}
