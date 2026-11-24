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
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderTarget.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::FrameDescription;
using vn::graphics::PresentOutcome;
using vn::graphics::RenderBackendRegistry;
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
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target_a.get(), 0, pixels));
    ASSERT_GE(pixels.size(), 4U);
    EXPECT_EQ(pixels[0], 255) << "the first target is not the red its thread cleared it to";
    EXPECT_EQ(pixels[1], 0);

    pixels.clear();
    ASSERT_TRUE(backend->readColorBuffer(target_b.get(), 0, pixels));
    ASSERT_GE(pixels.size(), 4U);
    EXPECT_EQ(pixels[0], 0) << "the second target is not the green its thread cleared it to";
    EXPECT_EQ(pixels[1], 255);

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
