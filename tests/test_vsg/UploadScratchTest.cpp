/**
 * @brief One-off upload staging is given back; the reusable block stays (T26).
 *
 * A static upload reaches device-local memory through a HOST_VISIBLE buffer of the transfer's own size, which vsg keeps
 * in the `TransferBlock` it will reuse (`TransferTask::transferData`) and no public call releases. On a 14M-triangle
 * model that was ~1.2 GB of host memory resident for the rest of the session. The backend now frees those buffers itself
 * (see `.ai/design/vsg-backend.md` F16.2), and this file is about WHEN: the first frame boundary whose copy has finished.
 *
 * WHY THE TRIGGER IS A FRAME AND NOT A HOST CALL: a staging buffer is the SOURCE of exactly one `vkCmdCopyBuffer`, so
 * the copy's own fence is the proof it may go - no timeline window, no retirement queue, no device idle, and nothing the
 * host has to know. A copy that has not finished is simply picked up by a later frame, which is why this case drives a
 * few quiet frames and watches the number settle.
 *
 * WHAT STAYS IS THE REUSABLE SIZE (T26): a block at or under `minimumStagingBufferSize` is the buffer a per-frame
 * uploader would rebuild every frame, so keeping it makes that host's steady state zero allocations. Anything LARGER is
 * one-off weight (the model load above) and is still given back - both halves are asserted here.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/internal/VsgBackend.hpp>

#include "TestFrame.hpp"
#include "TestGeometryBuilders.hpp"
#include "TestTriangleContent.hpp"

using vn::test::makeBigTriangle;
using vn::test::makeCamera;
using vn::test::makeTriangle;
using vn::test::TriangleFixture;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;

/**
 * @brief The release is the caller's: no frame stops the device, and one ask gives the staging back.
 *
 * Four assertions, in the order they matter:
 * - a frame never stops the device (there is no idle anywhere in this path);
 * - the staging of a finished upload is already gone after the frames that follow it - nobody asks;
 * - a re-draw of the same content stages nothing at all (no upload happens);
 * - a fresh stream stages again while its copy is in flight, and the frames after that free it too.
 */
TEST(UploadScratchTest, TheUploadScratchIsNotRetainedAndTheFramesGiveItBack)
{
    vn::graphics::DeviceDesc desc;
    desc.presentation = vn::graphics::DevicePresentation::None; // a device that presents nothing needs no display
    const vn::intrusive_ptr<vn::vsg::VsgBackend> backend(new vn::vsg::VsgBackend(desc));
    if (!backend->initialize() || !backend->capabilities().render_targets) {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(32, 32);

    const vn::intrusive_ptr<vn::graphics::Camera> camera = makeCamera();
    const vn::intrusive_ptr<RenderPass>           pass(new RenderPass());
    const vn::graphics::ClearPolicy               clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    // The frame's own shape: one pass, one target, one drawing call naming the content it draws (a frame with no
    // content uploads nothing, which is the quiet frame below).
    const auto drive = [&](const TriangleFixture* content) {
        TestFrame frame;
        frame.pass(pass.get()).target(target.get()).clear(clear).depth(vn::graphics::DepthMode::TestAndWrite);
        if (content != nullptr) {
            frame.content(content->commands, camera.get());
        }
        backend->executeFrame(frame.value(), nullptr);
    };

    // The first frames of a session cost idles of their own (the target is built, the in-flight count is learnt), so
    // the baseline is taken once the content has been drawn for a while and the counter has stopped moving.
    const TriangleFixture triangle = makeTriangle();
    for (int frame = 0; frame < 8; ++frame) {
        drive(&triangle);
    }
    const std::size_t settled = backend->deviceWaits();

    // THE REUSABLE FLOOR IS KEPT (T26); nothing larger is. A block whose staging is at or under vsg's
    // minimumStagingBufferSize is the buffer a per-frame uploader would rebuild every frame, so it stays - and one such
    // block exists per frame slot the uploads rotated through (at most three here). A surviving block is therefore a
    // multiple of that minimum; a ONE-OFF larger block is what the sweep gives back, and its absence is the assertion.
    constexpr std::size_t kMinimumStaging = 16U * 1024U * 1024U; // vsg's TransferTask::minimumStagingBufferSize
    const std::size_t     floor_bytes     = backend->uploadStagingBytes();
    EXPECT_GT(floor_bytes, 0U) << "the reusable minimum-sized block is kept for a host that uploads every frame";
    EXPECT_EQ(floor_bytes % kMinimumStaging, 0U) << "only reusable-size blocks survive";
    EXPECT_LE(floor_bytes, 3U * kMinimumStaging) << "one block per frame slot at most";
    EXPECT_EQ(backend->deviceWaits(), settled) << "and freeing it never stops the device";

    for (int frame = 0; frame < 4; ++frame) {
        drive(nullptr);
    }
    drive(&triangle);
    EXPECT_EQ(backend->deviceWaits(), settled) << "a frame never stops the device";
    EXPECT_EQ(backend->uploadStagingBytes(), floor_bytes) << "a re-draw of the same content stages nothing at all (no upload)";

    // A FRESH STREAM STAGES AGAIN, AND THE FRAMES AFTER IT GIVE IT BACK - nobody asks, because there is nothing to ask.
    const TriangleFixture second = makeTriangle();
    drive(&second);
    EXPECT_GT(backend->uploadStagingBytes(), floor_bytes) << "the upload stages while its copy is in flight";
    EXPECT_EQ(backend->deviceWaits(), settled) << "uploading alone still stops nothing";
    // A COPY'S FENCE IS THE DEVICE'S, NOT THE FRAME'S: the free lands on the first boundary AFTER it signals, which is a
    // handful of frames. What is asserted is that nobody asks - not that it happens on a particular frame.
    int quiet_frames = 0;
    while (backend->uploadStagingBytes() > 3U * kMinimumStaging && quiet_frames < 16) {
        drive(nullptr);
        ++quiet_frames;
    }
    EXPECT_EQ(backend->uploadStagingBytes() % kMinimumStaging, 0U) << "the frames that follow keep only reusable blocks";
    EXPECT_LT(quiet_frames, 16) << "and it takes a handful of frames, not a session";
    EXPECT_EQ(backend->deviceWaits(), settled) << "still without stopping the device";
    const std::size_t reused_floor = backend->uploadStagingBytes();
    EXPECT_GE(reused_floor, floor_bytes) << "a second frame slot keeps its own reusable block";

    // A ONE-OFF UPLOAD LARGER THAN THE FLOOR IS STILL GIVEN BACK: a model load stages its own size, and that weight must
    // not outlive its copy - the 1.2 GB case the sweep exists for. What stays is only reusable-size blocks.
    const TriangleFixture big = makeBigTriangle();
    drive(&big);
    EXPECT_GT(backend->uploadStagingBytes(), reused_floor) << "a transfer above the minimum stages its own size";
    quiet_frames = 0;
    while (backend->uploadStagingBytes() % kMinimumStaging != 0U && quiet_frames < 16) {
        drive(nullptr);
        ++quiet_frames;
    }
    EXPECT_EQ(backend->uploadStagingBytes() % kMinimumStaging, 0U) << "the one-off large block is given back";
    EXPECT_LT(quiet_frames, 16) << "a handful of frames, not a session";
    EXPECT_EQ(backend->deviceWaits(), settled) << "and no device idle on the way";

    backend->shutdown();
}
