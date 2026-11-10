/**
 * @brief The upload staging is NOT KEPT: the frames give it back on their own, and no device idle is ever paid.
 *
 * A static upload reaches device-local memory through a HOST_VISIBLE buffer of the transfer's own size, which vsg keeps
 * in the `TransferBlock` it will reuse (`TransferTask::transferData`) and no public call releases. On a 14M-triangle
 * model that was ~1.2 GB of host memory resident for the rest of the session. The backend now frees those buffers itself
 * (see `.ai/design/vsg-backend.md` F16.2), and this file is about WHEN: the first frame boundary whose copy has finished.
 *
 * WHY THE TRIGGER IS A FRAME AND NOT A HOST CALL: a staging buffer is the SOURCE of exactly one `vkCmdCopyBuffer`, so
 * the copy's own fence is the proof it may go - no timeline window, no retirement queue, no device idle, and nothing the
 * host has to know. A copy that has not finished is simply picked up by a later frame, which is why this case drives a
 * few quiet frames and watches the number go to zero.
 *
 * WHY NOT AFTER EVERY UPLOADING FRAME EITHER: freeing a buffer means the next upload sizes a fresh one, so a host that
 * uploads every frame would trade one allocation per frame for nothing. That is not asserted here (it is a cost, not a
 * rule); what is asserted is that the staging of a finished upload does not outlive the frame after it.
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

using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::RenderPass;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::test::setPrimitiveIndices;

namespace
{

/// @brief The content one case draws: a program, one geometry and its single drawing call.
///
/// Every call builds FRESH buffers, which is what makes it a fresh stream - and therefore a fresh upload - in the
/// stream layer (see `StreamUploads`: the identity is the buffer, its revision and the segment).
struct TriangleFixture {
    vn::intrusive_ptr<ShaderProgram> program;
    vn::intrusive_ptr<Geometry>      geometry;
    vn::intrusive_ptr<Material>      material;
    std::vector<RenderCommand>       commands;
};

/// @brief Builds the triangle: a view-block vertex stage and a constant red fragment stage.
TriangleFixture makeTriangle()
{
    TriangleFixture fixture;

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>("layout(location = 0) in vec3 position;\n"
                                                                    "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
                                                                    "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
                                                                    "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>("layout(location = 0) out vec4 outColor;\n"
                                                                      "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    fixture.program = program;

    const vn::intrusive_ptr<Geometry>          geometry(new Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>> positions =
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F }));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(positions);
    setPrimitiveIndices(*geometry, 0U, indices);
    geometry->setRevision(1U);
    fixture.geometry = geometry;

    const vn::intrusive_ptr<Material> material(new Material());
    material->setBaseColor(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    fixture.material = material;

    RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    fixture.commands.push_back(command);
    return fixture;
}

/// @brief A camera looking at (0, 0, 0) from (0, 0, 1.5), through an orthographic window of [-1, 1] squared.
vn::intrusive_ptr<vn::graphics::Camera> makeCamera()
{
    vn::intrusive_ptr<vn::graphics::Camera> camera(new vn::graphics::Camera());
    camera->lookAt(vn::math::Vec3d(0.0, 0.0, 1.5), vn::math::Vec3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setOrthographic(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    return camera;
}

} // namespace

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

    // NOTHING IS KEPT. The scratch this upload sized is already gone: the backend frees it at the first frame boundary
    // where the copy's own fence has signalled, so a host that keeps drawing never asks and never pays. Nothing about
    // that is a guess - `uploadStagingBytes()` is the number the GPU counters show as `SharedUsage`.
    EXPECT_EQ(backend->uploadStagingBytes(), 0U)
        << "an upload's staging is not retained: it is freed once its copy's fence signals";
    EXPECT_EQ(backend->deviceWaits(), settled) << "and freeing it never stops the device";

    for (int frame = 0; frame < 4; ++frame) {
        drive(nullptr);
    }
    drive(&triangle);
    EXPECT_EQ(backend->deviceWaits(), settled) << "a frame never stops the device";
    EXPECT_EQ(backend->uploadStagingBytes(), 0U) << "and a re-draw of the same content stages nothing at all (no upload)";

    // A FRESH STREAM STAGES AGAIN, AND THE FRAMES AFTER IT GIVE IT BACK - nobody asks, because there is nothing to ask.
    const TriangleFixture second = makeTriangle();
    drive(&second);
    EXPECT_GT(backend->uploadStagingBytes(), 0U) << "the upload stages while its copy is in flight";
    EXPECT_EQ(backend->deviceWaits(), settled) << "uploading alone still stops nothing";
    // A COPY'S FENCE IS THE DEVICE'S, NOT THE FRAME'S: the free lands on the first boundary AFTER it signals, which is a
    // handful of frames. What is asserted is that nobody asks - not that it happens on a particular frame.
    int quiet_frames = 0;
    while (backend->uploadStagingBytes() != 0U && quiet_frames < 16) {
        drive(nullptr);
        ++quiet_frames;
    }
    EXPECT_EQ(backend->uploadStagingBytes(), 0U) << "the frames that follow free it, with no call from the host";
    EXPECT_LT(quiet_frames, 16) << "and it takes a handful of frames, not a session";
    EXPECT_EQ(backend->deviceWaits(), settled) << "still without stopping the device";

    backend->shutdown();
}
