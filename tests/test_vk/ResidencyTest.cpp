/**
 * @brief M6's acceptance: residency is an ANSWERED question, and it follows the content that leaves.
 *
 * What this pins, and why each half matters (`.ai/design/vk-backend.md` 2.17 for the three residency states, and
 * `.ai/decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` for why a pin, not a time window, ends one):
 *   - the STEADY STATE IS FREE: a second frame of the same declarations uploads no stream, copies no texture and
 *     compiles no pipeline - which is what makes a long-running session's cost bounded;
 *   - PREPARE BUILDS THE FRAME WITHOUT RUNNING IT: `prepareFrame` answers the same passes/draws the frame would, and
 *     its deltas are what the frame then no longer pays (the SDK's `R23`, answered by the build counts);
 *   - RELEASING CONTENT LOWERS RESIDENCY, and the next frame that draws it uploads or compiles it again - the cost an
 *     explicit release is supposed to have;
 *   - ABSENT IS NOT ZERO: `FrameReport::uploads` is disengaged until a frame has run, and every other part says what
 *     it measured.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::EngineNotReadyReason;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::ReadbackStatus;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderTarget;
using vn::graphics::ResidencyKind;
using vn::graphics::ResidencyRow;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The M4 content ABI's vertex stage: the camera push plus the draw block.
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;

layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;

void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
}
)";

/// @brief Writes the material's base colour: set 0 binding 0 is the material block.
constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 params;
    uvec4 flags;
} vn_material;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(vn_material.base_color.rgb, 1.0);
}
)";

/// @brief Samples the first texture slot: binding 1 is the base-colour map.
constexpr const char8_t* kTextureFragment = u8R"(
#version 450
layout(set = 0, binding = 1) uniform sampler2D vn_tex;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(vn_tex, vec2(0.5, 0.5));
}
)";

/// @brief Everything one drawn frame needs, held so a test can release any piece of it by name.
struct Content {
    intrusive_ptr<ShaderProgram>                  program;
    intrusive_ptr<Geometry>                       geometry;
    intrusive_ptr<const vn::Buffer<float>>        positions; // Kept to release ONE stream's buffer.
    intrusive_ptr<const vn::Buffer<std::uint32_t>> indices;  // The primitive's index stream.
};

/// @brief Builds the two-stage program every content case shades with.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgramWith(const char8_t* fragment_source)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m6_program");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_source), String(u8"main") });
    return program;
}

/// @brief Builds the material-base-colour program.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    return makeProgramWith(kFragmentStage);
}

/// @brief An opaque material of one colour.
[[nodiscard]] intrusive_ptr<vn::graphics::Material> makeMaterial(const Colorf& color)
{
    auto material = intrusive_ptr<vn::graphics::Material>(new vn::graphics::Material());
    material->setBaseColor(color);
    return material;
}

/// @brief A triangle in clip space, indexed, as the one command of its geometry (z = 0.5: reverse-Z's mid band).
[[nodiscard]] Content makeContent()
{
    Content content;
    content.program   = makeProgram();
    content.positions = intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.75F, -0.75F, 0.5F, 0.75F, -0.75F, 0.5F, 0.0F, 0.75F, 0.5F }));
    content.indices = intrusive_ptr<const vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));

    content.geometry = intrusive_ptr<Geometry>(new Geometry());
    content.geometry->setPositions(content.positions);
    Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(content.indices);
    content.geometry->clearPrimitives();
    content.geometry->addPrimitive(primitive);
    content.geometry->bumpRevision();
    return content;
}

/// @brief A 64x64 RGBA8 + D24 target.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m6_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A camera whose matrices are identities, so the triangle's clip-space positions ARE the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
    return camera;
}

/// @brief A scene draw of @p content under @p material (or the default material when null).
[[nodiscard]] DrawPacket sceneDraw(const Content& content, const intrusive_ptr<vn::graphics::Material>& material)
{
    DrawItem command(content.geometry, material, vn::math::Mat4d{});
    command.program             = content.program;
    command.program_description = content.program->description();
    if (material != nullptr) {
        command.material             = material;
        command.material_description = material->description();
    }

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief A frame with ONE pass that clears @p target and issues @p draw.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawPacket draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The row @p kind of a report's ledger, or nothing when the report keeps no such row.
[[nodiscard]] std::optional<ResidencyRow> rowOf(const FrameReport& report, ResidencyKind kind)
{
    if (!report.retention.has_value() || !report.retention->residency.has_value()) {
        return std::nullopt;
    }
    for (const ResidencyRow& row : report.retention->residency->rows()) {
        if (row.kind == kind) {
            return row;
        }
    }
    return std::nullopt;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

} // namespace

TEST(VkResidencyTest, TheSecondFrameOfTheSameContentBuildsNothing)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();

    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    // THE FIRST FRAME PAYS FOR EVERYTHING: one vertex stream + one index stream, one pipeline (both modules arrive
    // under one program revision, so the ledger's `programs` row is the pipelines).
    const FrameReport first = backend->frameReport();
    ASSERT_TRUE(first.counters.has_value());
    ASSERT_TRUE(first.retention.has_value());
    ASSERT_TRUE(first.uploads.has_value()) << "a frame ran, so this device has numbers for it";
    EXPECT_EQ(first.counters->passes, 1U);
    EXPECT_EQ(first.counters->draw_packets, 1U);
    EXPECT_EQ(first.uploads->stream_uploads, 2U);
    EXPECT_GT(first.uploads->stream_bytes, 0U);
    EXPECT_EQ(first.uploads->texture_uploads, 0U);

    const std::uint64_t pipelines_after_first = device->contentPipelines();
    ASSERT_GT(pipelines_after_first, 0U) << "the first frame had to compile a pipeline";

    const std::optional<ResidencyRow> geometry = rowOf(first, ResidencyKind::Geometry);
    ASSERT_TRUE(geometry.has_value()) << "residency is an answered question once a frame has run";
    EXPECT_EQ(geometry->live, 2U) << "the vertex and index streams are resident";
    ASSERT_TRUE(geometry->bytes.has_value()) << "a buffer's bytes are known";
    EXPECT_GT(*geometry->bytes, 0U);

    const std::optional<ResidencyRow> programs = rowOf(first, ResidencyKind::Programs);
    ASSERT_TRUE(programs.has_value());
    EXPECT_EQ(programs->live, static_cast<std::uint32_t>(pipelines_after_first));

    // THE STEADY STATE: the same value again uploads nothing, copies nothing and compiles nothing.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const FrameReport second = backend->frameReport();
    ASSERT_TRUE(second.uploads.has_value());
    EXPECT_EQ(second.uploads->stream_uploads, 0U) << "a warm cache must not re-upload a stream";
    EXPECT_EQ(second.uploads->stream_bytes, 0U);
    EXPECT_EQ(second.uploads->texture_uploads, 0U);
    EXPECT_EQ(device->contentPipelines(), pipelines_after_first) << "a warm cache must not recompile a pipeline";
    EXPECT_EQ(second.counters->draw_packets, 1U) << "the frame still drew; its cost was already paid";

    ASSERT_TRUE(second.progress.has_value());
    EXPECT_EQ(second.progress->submitted, 2U);
    EXPECT_EQ(second.progress->completed, 2U);
    EXPECT_EQ(second.progress->presented, 0U) << "an off-screen device presents nothing";

    // THE PICTURE IS STILL THE TRIANGLE: "nothing happened" must not mean "nothing was drawn".
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, PrepareBuildsWhatTheFrameNeedsAndSubmitsNothing)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();
    const FrameDescription              frame    = drawingFrame(target.get(), sceneDraw(content, material));

    const PrepareReport prepared = backend->prepareFrame(frame);
    ASSERT_TRUE(prepared.prepared()) << "outcome " << static_cast<int>(prepared.outcome);
    EXPECT_EQ(prepared.passes, 1U) << "the shape of the frame asked for, not what could be built";
    EXPECT_EQ(prepared.draw_packets, 1U);
    EXPECT_EQ(prepared.uploads, 2U) << "both streams are built by the warm-up";
    EXPECT_EQ(prepared.halves, 1U) << "one program revision";
    EXPECT_GT(prepared.pipelines, 0U);
    EXPECT_EQ(prepared.sets, 0U) << "no descriptor set outlives a frame here";

    // PREPARE IS NOT A FRAME: nothing was submitted, and the last frame's report is untouched.
    const FrameReport after_prepare = backend->frameReport();
    ASSERT_TRUE(after_prepare.progress.has_value());
    EXPECT_EQ(after_prepare.progress->submitted, 0U);
    EXPECT_FALSE(after_prepare.uploads.has_value()) << "no frame has run, so no frame's numbers exist";

    // A SECOND PREPARE OF THE SAME FRAME BUILDS NOTHING - the property the call exists for.
    const PrepareReport again = backend->prepareFrame(frame);
    ASSERT_TRUE(again.prepared());
    EXPECT_EQ(again.uploads, 0U);
    EXPECT_EQ(again.halves, 0U);
    EXPECT_EQ(again.pipelines, 0U);
    EXPECT_EQ(again.passes, 1U);

    // AND THE FRAME ITSELF PAYS NOTHING FOR WHAT WAS PREPARED.
    const std::uint64_t pipelines_before = device->contentPipelines();
    ASSERT_EQ(backend->executeFrame(frame), PresentOutcome::Skipped);
    EXPECT_EQ(device->contentPipelines(), pipelines_before) << "the prepared pipeline was reused";
    const FrameReport ran = backend->frameReport();
    ASSERT_TRUE(ran.uploads.has_value());
    EXPECT_EQ(ran.uploads->stream_uploads, 0U) << "the prepared streams were reused";
    EXPECT_EQ(ran.progress->submitted, 1U);

    // THE PICTURE FOLLOWS THE WARM-UP: preparation must not be a different frame.
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, ReleasingAGeometryLowersResidencyAndTheNextFrameUploadsItAgain)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const std::optional<ResidencyRow> before = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->live, 2U);

    ASSERT_TRUE(backend->releaseContent(content.geometry.get()));

    // RESIDENCY DROPS IMMEDIATELY: this device's queue is idle between frames, so a release is a destruction.
    const FrameReport released = backend->frameReport();
    const std::optional<ResidencyRow> after = rowOf(released, ResidencyKind::Geometry);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->live, 0U) << "a released geometry holds no streams";
    EXPECT_EQ(after->bytes.value_or(1U), 0U) << "its bytes left with it";
    EXPECT_EQ(after->retired.value_or(0U), 2U) << "both streams were let go";
    ASSERT_TRUE(released.retention.has_value());
    EXPECT_EQ(released.retention->released_nodes, 1U);
    EXPECT_EQ(released.retention->device_waits, 0U) << "the frame's own fence wait is not a destructive idle";

    // AND THE NEXT FRAME UPLOADS IT AGAIN - what the release costs.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const FrameReport rerun = backend->frameReport();
    ASSERT_TRUE(rerun.uploads.has_value());
    EXPECT_EQ(rerun.uploads->stream_uploads, 2U);

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "releasing and re-uploading must draw the same picture";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, AReleaseIsHandedOverParkedAndDestroyedAfterTheFramesThatCouldNameIt)
{
    // A RELEASE IS TWO FACTS (see Device::retirementNumbers): the cache forgets the entry AT ONCE - residency drops and
    // the next frame uploads it again - while the Vulkan objects behind it live until the device's completion
    // watermark has passed every frame that could still read them. Without the second half the call would be a
    // between-frames rule the host has to keep; with it, a host may release content while another session frames
    // (ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md, step 2).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const vn::backend_vk::Device::RetirementNumbers before = device->retirementNumbers();
    ASSERT_EQ(before.pending, 0U) << "nothing was released yet, so nothing is parked";

    ASSERT_TRUE(backend->releaseContent(content.geometry.get()));

    // THE CACHE FORGOT IT, AND THE OBJECTS ARE PARKED RATHER THAN DESTROYED.
    const std::optional<ResidencyRow> after = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->live, 0U) << "a released geometry holds no streams";
    const vn::backend_vk::Device::RetirementNumbers parked = device->retirementNumbers();
    EXPECT_EQ(parked.pending, before.pending + 1U) << "the release was destroyed instead of handed to the queue";
    EXPECT_EQ(parked.released, before.released) << "it was released before its frame was proven complete";
    EXPECT_EQ(parked.device_waits, 0U) << "parking a release must not idle the device";

    // TWO FRAMES LATER the watermark has passed the frame that could still name the buffers (slots + 1), so the queue
    // lets go - and it does so on the frame's own last step, not on a device idle.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const vn::backend_vk::Device::RetirementNumbers drained = device->retirementNumbers();
    EXPECT_EQ(drained.pending, 0U) << "the parked release never became due";
    EXPECT_EQ(drained.released, before.released + 1U) << "the parked release never ran";
    EXPECT_EQ(drained.device_waits, 0U) << "the drain idled the device";

    // AND A TEARDOWN FLUSHES WHAT IS STILL PARKED: a release followed by shutdown must leave nothing behind, or every
    // parked object would leak with the session that outlived the queue's turn.
    ASSERT_TRUE(backend->releaseContent(content.geometry.get()));
    EXPECT_EQ(device->retirementNumbers().pending, 1U);
    backend->shutdown();
    EXPECT_EQ(device->retirementNumbers().pending, 0U) << "shutdown left releases parked (a leak)";
    EXPECT_EQ(device->memoryStats().blocks_live, 0U) << "shutdown left blocks allocated";
}

TEST(VkResidencyTest, ReleasingAProgramLowersTheCompiledHalvesAndKeepsTheStreams)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const std::optional<ResidencyRow> programs_before = rowOf(backend->frameReport(), ResidencyKind::Programs);
    ASSERT_TRUE(programs_before.has_value());
    const std::uint32_t pipelines = programs_before->live;
    ASSERT_GT(pipelines, 0U);
    const std::uint64_t pipelines_created = device->contentPipelines();

    ASSERT_TRUE(backend->releaseContent(content.program.get()));

    const std::optional<ResidencyRow> programs_after = rowOf(backend->frameReport(), ResidencyKind::Programs);
    ASSERT_TRUE(programs_after.has_value());
    EXPECT_EQ(programs_after->live, 0U) << "a released program takes its pipelines with it";
    EXPECT_EQ(programs_after->retired.value_or(0U), pipelines);

    // THE STREAMS STAY: a program release says nothing about geometry.
    const std::optional<ResidencyRow> geometry = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(geometry.has_value());
    EXPECT_EQ(geometry->live, 2U);

    // THE NEXT FRAME COMPILES AGAIN - and uploads nothing, because the streams never left.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    EXPECT_GT(device->contentPipelines(), pipelines_created) << "the released pipeline was compiled again";
    const FrameReport rerun = backend->frameReport();
    ASSERT_TRUE(rerun.uploads.has_value());
    EXPECT_EQ(rerun.uploads->stream_uploads, 0U);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, ReleasingATextureLowersTheImageResidency)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // THE PROGRAM SAMPLES THE TEXTURE (binding 1), so the pixel below proves the re-upload, not only the count.
    Content sampled = makeContent();
    sampled.program = makeProgramWith(kTextureFragment);
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0] = std::byte{ 0U };
    texel[1] = std::byte{ 0U };
    texel[2] = std::byte{ 255U };
    texel[3] = std::byte{ 255U };
    const auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    material->setTexture(TextureSlot::BaseColor, texture);

    const intrusive_ptr<RenderTarget> target = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(sampled, material))), PresentOutcome::Skipped);

    const FrameReport first = backend->frameReport();
    ASSERT_TRUE(first.uploads.has_value());
    EXPECT_EQ(first.uploads->texture_uploads, 1U);
    EXPECT_GT(first.uploads->texture_bytes, 0U);

    const std::optional<ResidencyRow> textures_before = rowOf(first, ResidencyKind::Textures);
    ASSERT_TRUE(textures_before.has_value());
    ASSERT_EQ(textures_before->live, 1U);
    ASSERT_GT(textures_before->bytes.value_or(0U), 0U);

    ASSERT_TRUE(backend->releaseContent(texture.get()));

    const std::optional<ResidencyRow> textures_after = rowOf(backend->frameReport(), ResidencyKind::Textures);
    ASSERT_TRUE(textures_after.has_value());
    EXPECT_EQ(textures_after->live, 0U) << "a released texture holds no image";
    EXPECT_EQ(textures_after->bytes.value_or(1U), 0U);
    EXPECT_EQ(textures_after->retired.value_or(0U), 1U);

    // THE NEXT FRAME UPLOADS IT AGAIN, and still samples the right texel.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(sampled, material))), PresentOutcome::Skipped);
    const FrameReport rerun = backend->frameReport();
    EXPECT_EQ(rerun.uploads->texture_uploads, 1U);

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, ARefusedTextureUploadRecordsNothingSoTheFrameNamesNoDestroyedImage)
{
    // PREPARED, THEN RECORDED (`.ai/design/vk-backend.md` 2.23): a texture's bytes are built BEFORE the first command is
    // recorded, so a refused allocation releases what it built and leaves the caller's command buffer exactly as it
    // found it. The defect this case kills (review 19 `Z15`): the staging was allocated INSIDE the recording loop, so a
    // refusal came AFTER the barrier into TRANSFER_DST had been recorded - and the failure path then destroyed the image
    // that barrier named, while the frame went on to SUBMIT that command buffer (validation:
    // `VUID-vkDestroyImage-image-01000`; on a driver without the layer, undefined behaviour).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    // THE SIZE IS THE LEVER (see MemoryBlockSizes): the pool's first host-visible block is 16 MiB and its first
    // device-local one 64 MiB, so a 21 MiB image still comes out of the open device-local block while its staging buffer
    // has no block to come from - which is what makes the refusal land on the STAGING, the allocation that used to
    // happen after the recording had begun. A device that caps those fills smaller refuses the IMAGE instead, one
    // allocation earlier, and the assertions below hold for the same reason.
    constexpr int kTexels = 2304; // 2304 x 2304 x RGBA8 = 21 MiB

    Content content = makeContent();
    content.program = makeProgramWith(kTextureFragment);
    const intrusive_ptr<vn::graphics::Material> textured = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(kTexels, kTexels, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texels = image->mipData(0);
    for (std::size_t texel = 0; texel < texels.size(); texel += 4U) {
        texels[texel + 0U] = std::byte{ 0U };
        texels[texel + 1U] = std::byte{ 0U };
        texels[texel + 2U] = std::byte{ 255U };
        texels[texel + 3U] = std::byte{ 255U };
    }
    const auto texture = intrusive_ptr<Texture2D>(new Texture2D(kTexels, kTexels, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    textured->setTexture(TextureSlot::BaseColor, texture);

    const intrusive_ptr<RenderTarget> target = makeTarget();

    // THE TWO NUMBERS THIS CASE READS, as the frame path has spent them so far (see `Device::RecordingNumbers`).
    const auto recorded = [device]() {
        const vn::backend_vk::Device::RecordingNumbers numbers = device->recordingNumbers();
        return std::pair<std::uint64_t, std::uint64_t>{ numbers.barrier_dependencies, numbers.barriers };
    };

    // THE WARM-UP FRAME OWNS EVERYTHING THIS CASE IS NOT ABOUT: the target's attachments, the program's pipeline, the
    // geometry's streams and the 1x1 stand-in all exist before the frames below, so their barriers do not move the
    // numbers this case reads.
    const intrusive_ptr<vn::graphics::Material> plain = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, plain))), PresentOutcome::Skipped);

    // THE BASELINE: the same pass and the same program, but a material that names no texture, so this frame uploads
    // nothing - its barriers ARE the pass' own, and the two frames below are read against it.
    const auto baseline_before = recorded();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, plain))), PresentOutcome::Skipped);
    const auto baseline = recorded();
    const std::uint64_t baseline_dependencies = baseline.first - baseline_before.first;
    const std::uint64_t baseline_barriers     = baseline.second - baseline_before.second;
    ASSERT_GT(baseline_barriers, 0U) << "the pass itself records barriers, so a zero below would prove nothing";

    // THE REFUSAL: every NEW pool block is refused from here on, and only the texture's staging buffer needs one.
    int refusals = 0;
    device->setMemoryBlockCreator([&refusals](VkDevice, const VkMemoryAllocateInfo&, VkDeviceMemory*) {
        ++refusals;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    });
    const auto refused_before = recorded();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, textured))), PresentOutcome::Skipped);
    EXPECT_GT(refusals, 0) << "the injection was never reached: the case proved nothing";
    const auto refused = recorded();

    // THE FRAME STILL RAN, AND IT BOUND THE STAND-IN: a texture that could not be uploaded is ABSENT, not black.
    const FrameReport rejected = backend->frameReport();
    ASSERT_TRUE(rejected.uploads.has_value());
    EXPECT_EQ(rejected.uploads->texture_uploads, 0U) << "a refused upload published an entry";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 1U) << "the refused upload was not reported";
    const ColorReadback skipped = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(skipped.ok());
    EXPECT_EQ(pixelAt(skipped.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 255, 255, 255 }))
        << "the slot a refused upload could not fill must sample the stand-in, not a picture nobody wrote";

    // A REFUSED UPLOAD RECORDED NOTHING AT ALL - the frame spent exactly what the baseline spent, because the failure
    // came from the PREPARE phase and no command had named the image yet. This is the half that used to be false: the
    // old shape recorded the transition into TRANSFER_DST before the allocation it failed on, so this frame spent one
    // barrier more than the baseline - one command naming an image the failure path then destroyed.
    EXPECT_EQ(refused.first - refused_before.first, baseline_dependencies) << "the refused upload left a command naming the image it let go";
    EXPECT_EQ(refused.second - refused_before.second, baseline_barriers) << "the refused upload left a barrier naming the image it let go";

    // AND THE SAME FRAME WITH THE POOL WORKING AGAIN spends exactly the upload's own two barriers on top (UNDEFINED ->
    // TRANSFER_DST, then TRANSFER_DST -> SHADER_READ_ONLY) - which is what the numbers above are read against.
    device->setMemoryBlockCreator({});
    const auto filled_before = recorded();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, textured))), PresentOutcome::Skipped);
    const auto filled = recorded();
    EXPECT_EQ(filled.first - filled_before.first, baseline_dependencies + 2U) << "an accepted upload still records its two barriers";
    EXPECT_EQ(filled.second - filled_before.second, baseline_barriers + 2U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the driver reported something (a destroyed image in a submitted command buffer is the Z15 shape)";

    // AND THE TEXTURE IS UPLOADED ONCE THE POOL CAN SERVE IT - nothing about the refusal was remembered as done.
    const FrameReport rerun = backend->frameReport();
    ASSERT_TRUE(rerun.uploads.has_value());
    EXPECT_EQ(rerun.uploads->texture_uploads, 1U) << "the refused upload was remembered as done";
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the frame after the refusal did not upload and sample the texture";

    backend->shutdown();
}

TEST(VkResidencyTest, ReleasingATargetFreesItsAttachmentsAndTheNextFrameBuildsThemAgain)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();

    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    // THE RELEASE IS THE HOST'S STATEMENT, and the answer is that this device holds no attachments for the target:
    // a readback has nothing to read rather than stale pixels (the SDK's ReadbackResult states the difference).
    device->releaseRenderTarget(target.get());
    const ColorReadback after_release = backend->readColorImage(target.get(), 0);
    EXPECT_EQ(after_release.status, ReadbackStatus::NotReady);
    EXPECT_TRUE(after_release.values.empty()) << "a refused read must not hand out the previous pixels";

    // THE OBJECT STAYS A DESCRIPTION: the next frame that names it builds the attachments again, and the picture is
    // the one the content draws - a release changes what is resident, never what the target means.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const ColorReadback rebuilt = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(rebuilt.ok());
    EXPECT_EQ(pixelAt(rebuilt.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    // AND RELEASING WHAT WAS NEVER BUILT (OR NOTHING) IS NOT AN ERROR: the statement is already true.
    const intrusive_ptr<RenderTarget> never_drawn = makeTarget();
    device->releaseRenderTarget(never_drawn.get());
    device->releaseRenderTarget(nullptr);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, ReleasingOneBufferLowersOnlyTheStreamsThatReadIt)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const Content                       content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>   target   = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const std::optional<ResidencyRow> before = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->live, 2U);

    // THE VERTEX BUFFER ONLY: the index stream reads a different buffer and must stay.
    ASSERT_TRUE(backend->releaseContent(*content.positions));

    const std::optional<ResidencyRow> after = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->live, 1U) << "the index stream was not named by the release";
    EXPECT_LT(after->bytes.value_or(0U), *before->bytes);

    // THE NEXT FRAME RE-UPLOADS EXACTLY ONE STREAM, and draws the same picture.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const FrameReport rerun = backend->frameReport();
    ASSERT_TRUE(rerun.uploads.has_value());
    EXPECT_EQ(rerun.uploads->stream_uploads, 1U);

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, WhatThisDeviceDidNotMeasureItDoesNotClaim)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // NO FRAME HAS RUN: the counters exist (this device keeps them) and say zero, the upload numbers are DISENGAGED -
    // an unmeasured frame must not read like an empty one (see FrameReport).
    const FrameReport fresh = backend->frameReport();
    ASSERT_TRUE(fresh.counters.has_value());
    EXPECT_EQ(fresh.counters->passes, 0U);
    EXPECT_EQ(fresh.counters->draw_packets, 0U);
    EXPECT_FALSE(fresh.uploads.has_value());
    ASSERT_TRUE(fresh.progress.has_value());
    EXPECT_EQ(fresh.progress->submitted, 0U);
    EXPECT_EQ(fresh.progress->completed, 0U);

    // THE MEMORY FIGURE IS MEASURED FROM THE FIRST MOMENT (see DeviceMemory): `initialize()` itself allocates (the
    // uniform ring and the default program's pipeline), so the account is ENGAGED before any frame - unlike the
    // uploads, which no frame has measured yet. That it is disengaged after `shutdown()` is asserted below.
    ASSERT_TRUE(fresh.memory.has_value()) << "the feature says this device reports its pool";
    EXPECT_GE(fresh.memory->reserved_bytes, fresh.memory->used_bytes);
    EXPECT_GE(fresh.memory->peak_used_bytes, fresh.memory->used_bytes);

    // RESIDENCY IS ANSWERED AND EMPTY: EVERY kind the vocabulary names is reported, at zero - `Materials` is the
    // one this backend accounts for while retaining nothing across frames (see Device::noteResidency), which is a
    // measurement rather than an omission.
    ASSERT_TRUE(fresh.retention.has_value());
    ASSERT_TRUE(fresh.retention->residency.has_value());
    EXPECT_EQ(fresh.retention->residency->rows().size(), vn::graphics::kResidencyKindCount)
        << "the row's existence is the promise; a host writes one loop over the rows";
    const std::optional<ResidencyRow> geometry = rowOf(fresh, ResidencyKind::Geometry);
    ASSERT_TRUE(geometry.has_value());
    EXPECT_EQ(geometry->live, 0U);
    EXPECT_EQ(geometry->bytes.value_or(1U), 0U);
    const std::optional<ResidencyRow> textures = rowOf(fresh, ResidencyKind::Textures);
    ASSERT_TRUE(textures.has_value());
    EXPECT_EQ(textures->live, 0U);
    const std::optional<ResidencyRow> programs = rowOf(fresh, ResidencyKind::Programs);
    ASSERT_TRUE(programs.has_value());
    EXPECT_EQ(programs->live, 0U);
    const std::optional<ResidencyRow> materials = rowOf(fresh, ResidencyKind::Materials);
    ASSERT_TRUE(materials.has_value()) << "this device writes material values into each frame's own scratch";
    EXPECT_EQ(materials->live, 0U);
    ASSERT_TRUE(materials->bytes.has_value()) << "nothing survives a frame, so zero is measured rather than unknown";
    EXPECT_EQ(*materials->bytes, 0U);

    // A MATERIAL IS CONTENT WITH NOTHING RESIDENT: accepted, and nothing is claimed as released.
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    EXPECT_TRUE(backend->releaseContent(material.get()));
    EXPECT_EQ(backend->frameReport().retention->released_nodes, 0U);

    // NOT CONTENT IS REFUSED, not silently ignored.
    const intrusive_ptr<RenderTarget> target = makeTarget();
    EXPECT_FALSE(backend->releaseContent(target.get()));
    EXPECT_FALSE(backend->releaseContent(nullptr));

    backend->shutdown();

    // A DEVICE THAT IS NOT UP HAS NOTHING TO RELEASE AND NOTHING TO PREPARE - and says which
    // (PrepareReport::Outcome::NotReady + EngineNotReadyReason::NotInitialized), not a zero it did not earn.
    EXPECT_FALSE(backend->releaseContent(material.get()));
    const FrameDescription frame;
    const PrepareReport    prepared = backend->prepareFrame(frame);
    EXPECT_FALSE(prepared.prepared());
    EXPECT_EQ(prepared.outcome, PrepareReport::Outcome::NotReady);
    EXPECT_EQ(prepared.reason, EngineNotReadyReason::NotInitialized);
    EXPECT_FALSE(backend->frameReport().retention->residency.has_value()) << "a shut-down device accounts for nothing";
    EXPECT_FALSE(backend->frameReport().memory.has_value()) << "a shut-down device holds no memory to account for";
}

TEST(VkResidencyTest, ASupersededShapeLeavesTheResidencyItReplaced)
{
    // A HOST THAT REGENERATES ITS DATA IS THE ORDINARY DYNAMIC SCENE, and the shape announcement is how it says so
    // (FpsOverlay::writePattern is the in-tree one: a fresh positions buffer plus `bumpRevision()`). A shape revision is a
    // NEW identity, so the entry built for the previous one is nobody's once the geometry has moved on - if the cache
    // keeps it, every regeneration leaks one entry's streams, the description and the host's own bytes, for the
    // session's life.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    Content                                     content  = makeContent();
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<RenderTarget>           target   = makeTarget();

    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const std::optional<ResidencyRow> before = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->live, 2U) << "the vertex and index streams are resident";

    // A SECOND HOLDER OF THE OLD SHAPE: what a cached publication or a frame that is still recording has, and the reason
    // the sweep may not simply drop everything the geometry moved on from.
    DrawPacket held = sceneDraw(content, material);

    // THE SHAPE MOVES: a NEW buffer (a regenerated mesh may not keep its vertex count, so the bytes cannot be edited in
    // place) and the announcement that goes with it.
    content.geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(
        new vn::Buffer<float>(std::vector<float>{ -0.5F, -0.5F, 0.5F, 0.5F, -0.5F, 0.5F, 0.0F, 0.5F, 0.5F })));
    content.geometry->bumpRevision();

    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const FrameReport replaced = backend->frameReport();
    ASSERT_TRUE(replaced.uploads.has_value());
    EXPECT_EQ(replaced.uploads->stream_uploads, 2U) << "the new revision's entry uploads its own streams";
    const std::optional<ResidencyRow> both = rowOf(replaced, ResidencyKind::Geometry);
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(both->live, 4U) << "the old shape is still named, so it is still resident (and parked behind that hold)";

    // NOTHING NAMES THE PREVIOUS SHAPE ANY MORE: its streams must leave at the frame end (the residency this backend
    // promises for content that left - see vk-backend.md N11).
    held = DrawPacket{};
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const std::optional<ResidencyRow> after = rowOf(backend->frameReport(), ResidencyKind::Geometry);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->live, 2U) << "one shape is resident, not one per regeneration";
    EXPECT_LT(after->bytes.value_or(0U), both->bytes.value_or(0U)) << "the superseded streams' bytes went with them";

    // AND THE PICTURE IS STILL THE TRIANGLE THE CURRENT SHAPE STATES.
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the frame after the sweep draws the geometry that is still there";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkResidencyTest, ContentTheHostNoLongerHoldsLeavesResidencyBehind)
{
    // THE OTHER HALF OF THE SAME RULE (`graphics-content-lifecycle.md` 2.9's second switch): the host dropping the last
    // reference is the host saying it does not want the object any more, and the residency that object owns leaves with
    // it. Opening one document and then another is the shape: a cache that keeps the first one's uploads grows with the
    // number of documents the session has ever opened.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // The first document: its own program, geometry and a sampled texture.
    Content first            = makeContent();
    first.program            = makeProgramWith(kTextureFragment);
    intrusive_ptr<vn::graphics::Material> first_material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    auto                                  image          = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte>            texel          = image->mipData(0);
    texel[0] = std::byte{ 200U };
    texel[1] = std::byte{ 0U };
    texel[2] = std::byte{ 0U };
    texel[3] = std::byte{ 255U };
    auto first_texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    first_texture->setImage(image);
    first_material->setTexture(TextureSlot::BaseColor, first_texture);

    const intrusive_ptr<RenderTarget> target = makeTarget();
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(first, first_material))), PresentOutcome::Skipped);
    const FrameReport opened = backend->frameReport();
    ASSERT_TRUE(opened.uploads.has_value());
    EXPECT_EQ(opened.uploads->stream_uploads, 2U);
    EXPECT_EQ(opened.uploads->texture_uploads, 1U);
    ASSERT_EQ(rowOf(opened, ResidencyKind::Geometry)->live, 2U);
    ASSERT_EQ(rowOf(opened, ResidencyKind::Textures)->live, 1U);
    const std::uint32_t programs_open = rowOf(opened, ResidencyKind::Programs)->live;
    ASSERT_GT(programs_open, 0U);

    // THE SECOND DOCUMENT, and then the first document is destroyed - which is the only announcement such a host makes
    // (there is no object left to hand `releaseContent`).
    Content                                     second = makeContent();
    const intrusive_ptr<vn::graphics::Material> plain  = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    first_texture  = nullptr;
    first_material = nullptr;
    first.program  = nullptr;
    first.geometry = nullptr;
    first.positions = nullptr;
    first.indices   = nullptr;

    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(second, plain))), PresentOutcome::Skipped);
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(second, plain))), PresentOutcome::Skipped);

    const FrameReport closed = backend->frameReport();
    const std::optional<ResidencyRow> geometry = rowOf(closed, ResidencyKind::Geometry);
    const std::optional<ResidencyRow> textures = rowOf(closed, ResidencyKind::Textures);
    const std::optional<ResidencyRow> programs = rowOf(closed, ResidencyKind::Programs);
    ASSERT_TRUE(geometry.has_value());
    ASSERT_TRUE(textures.has_value());
    ASSERT_TRUE(programs.has_value());
    EXPECT_EQ(geometry->live, 2U) << "the closed document's streams went with it; the open one's stayed";
    EXPECT_EQ(textures->live, 0U) << "no material names that texture any more";
    // THE SECOND DOCUMENT'S PROGRAM IS ONE PIPELINE OF ITS OWN, so the count stays 1 while the ledger says WHICH one: the
    // closed document's pipeline was built and then retired.
    EXPECT_EQ(programs->live, programs_open) << "one pipeline is resident - the open document's";
    EXPECT_EQ(programs->built.value_or(0U), 2U) << "each document compiled its own program revision";
    EXPECT_EQ(programs->retired.value_or(0U), 1U) << "the closed document's pipeline left with its program";

    // THE SECOND DOCUMENT STILL DRAWS: a sweep that empties the cache would pass the counts above and lose the picture.
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 255, 0, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}
