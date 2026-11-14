/**
 * @brief M6's acceptance: residency is an ANSWERED question, and it follows the content that leaves.
 *
 * What this pins, and why each half matters (see `.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §8.3,
 * M6):
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
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
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
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::EngineNotReady;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ResidencyKind;
using vn::graphics::ResidencyRow;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;
using vn::intrusive_ptr;

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
[[nodiscard]] DrawDescription sceneDraw(const Content& content, const intrusive_ptr<vn::graphics::Material>& material)
{
    RenderCommand command(content.geometry, material, vn::math::Mat4d{});
    command.program             = content.program;
    command.program_description = content.program->description();
    if (material != nullptr) {
        command.material             = material;
        command.material_description = material->description();
    }

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;
    return draw;
}

/// @brief A frame with ONE pass that clears @p target and issues @p draw.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawDescription draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.draws.push_back(std::move(draw));
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
    // under one program revision, so the ledger's `halves` are the pipelines).
    const FrameReport first = backend->frameReport();
    ASSERT_TRUE(first.counters.has_value());
    ASSERT_TRUE(first.retention.has_value());
    ASSERT_TRUE(first.uploads.has_value()) << "a frame ran, so this device has numbers for it";
    EXPECT_EQ(first.counters->passes, 1U);
    EXPECT_EQ(first.counters->draws, 1U);
    EXPECT_EQ(first.uploads->stream_uploads, 2U);
    EXPECT_GT(first.uploads->stream_bytes, 0U);
    EXPECT_EQ(first.uploads->texture_uploads, 0U);

    const std::uint64_t pipelines_after_first = device->contentPipelines();
    ASSERT_GT(pipelines_after_first, 0U) << "the first frame had to compile a pipeline";

    const std::optional<ResidencyRow> streams = rowOf(first, ResidencyKind::Streams);
    ASSERT_TRUE(streams.has_value()) << "residency is an answered question once a frame has run";
    EXPECT_EQ(streams->live, 2U) << "the vertex and index streams are resident";
    ASSERT_TRUE(streams->bytes.has_value()) << "a buffer's bytes are known";
    EXPECT_GT(*streams->bytes, 0U);

    const std::optional<ResidencyRow> halves = rowOf(first, ResidencyKind::Halves);
    ASSERT_TRUE(halves.has_value());
    EXPECT_EQ(halves->live, static_cast<std::uint32_t>(pipelines_after_first));

    // THE STEADY STATE: the same value again uploads nothing, copies nothing and compiles nothing.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);

    const FrameReport second = backend->frameReport();
    ASSERT_TRUE(second.uploads.has_value());
    EXPECT_EQ(second.uploads->stream_uploads, 0U) << "a warm cache must not re-upload a stream";
    EXPECT_EQ(second.uploads->stream_bytes, 0U);
    EXPECT_EQ(second.uploads->texture_uploads, 0U);
    EXPECT_EQ(device->contentPipelines(), pipelines_after_first) << "a warm cache must not recompile a pipeline";
    EXPECT_EQ(second.counters->draws, 1U) << "the frame still drew; its cost was already paid";

    ASSERT_TRUE(second.progress.has_value());
    EXPECT_EQ(second.progress->submitted, 2U);
    EXPECT_EQ(second.progress->completed, 2U);
    EXPECT_EQ(second.progress->presented, 0U) << "an off-screen device presents nothing";

    // THE PICTURE IS STILL THE TRIANGLE: "nothing happened" must not mean "nothing was drawn".
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

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
    EXPECT_EQ(prepared.draws, 1U);
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
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

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

    const std::optional<ResidencyRow> before = rowOf(backend->frameReport(), ResidencyKind::Streams);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->live, 2U);

    ASSERT_TRUE(backend->releaseContent(content.geometry.get()));

    // RESIDENCY DROPS IMMEDIATELY: this device's queue is idle between frames, so a release is a destruction.
    const FrameReport released = backend->frameReport();
    const std::optional<ResidencyRow> after = rowOf(released, ResidencyKind::Streams);
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

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "releasing and re-uploading must draw the same picture";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
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

    const std::optional<ResidencyRow> halves_before = rowOf(backend->frameReport(), ResidencyKind::Halves);
    ASSERT_TRUE(halves_before.has_value());
    const std::uint32_t pipelines = halves_before->live;
    ASSERT_GT(pipelines, 0U);
    const std::uint64_t pipelines_created = device->contentPipelines();

    ASSERT_TRUE(backend->releaseContent(content.program.get()));

    const std::optional<ResidencyRow> halves_after = rowOf(backend->frameReport(), ResidencyKind::Halves);
    ASSERT_TRUE(halves_after.has_value());
    EXPECT_EQ(halves_after->live, 0U) << "a released program takes its pipelines with it";
    EXPECT_EQ(halves_after->retired.value_or(0U), pipelines);

    // THE STREAMS STAY: a program release says nothing about geometry.
    const std::optional<ResidencyRow> streams = rowOf(backend->frameReport(), ResidencyKind::Streams);
    ASSERT_TRUE(streams.has_value());
    EXPECT_EQ(streams->live, 2U);

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

    const std::optional<ResidencyRow> images_before = rowOf(first, ResidencyKind::Images);
    ASSERT_TRUE(images_before.has_value());
    ASSERT_EQ(images_before->live, 1U);
    ASSERT_GT(images_before->bytes.value_or(0U), 0U);

    ASSERT_TRUE(backend->releaseContent(texture.get()));

    const std::optional<ResidencyRow> images_after = rowOf(backend->frameReport(), ResidencyKind::Images);
    ASSERT_TRUE(images_after.has_value());
    EXPECT_EQ(images_after->live, 0U) << "a released texture holds no image";
    EXPECT_EQ(images_after->bytes.value_or(1U), 0U);
    EXPECT_EQ(images_after->retired.value_or(0U), 1U);

    // THE NEXT FRAME UPLOADS IT AGAIN, and still samples the right texel.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(sampled, material))), PresentOutcome::Skipped);
    const FrameReport rerun = backend->frameReport();
    EXPECT_EQ(rerun.uploads->texture_uploads, 1U);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
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
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    // THE RELEASE IS THE HOST'S STATEMENT, and the answer is that this device holds no attachments for the target:
    // a readback has nothing to read rather than stale pixels (the SDK's ReadbackResult states the difference).
    device->releaseRenderTarget(target.get());
    std::vector<std::uint8_t> after_release;
    vn::graphics::ReadbackResult why = vn::graphics::ReadbackResult::Ok;
    EXPECT_FALSE(backend->readColorBuffer(target.get(), 0, after_release, &why));
    EXPECT_EQ(why, vn::graphics::ReadbackResult::NotReady);
    EXPECT_TRUE(after_release.empty()) << "a refused read must not hand out the previous pixels";

    // THE OBJECT STAYS A DESCRIPTION: the next frame that names it builds the attachments again, and the picture is
    // the one the content draws - a release changes what is resident, never what the target means.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    std::vector<std::uint8_t> rebuilt;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, rebuilt));
    EXPECT_EQ(pixelAt(rebuilt, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

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

    const std::optional<ResidencyRow> before = rowOf(backend->frameReport(), ResidencyKind::Streams);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->live, 2U);

    // THE VERTEX BUFFER ONLY: the index stream reads a different buffer and must stay.
    ASSERT_TRUE(backend->releaseContent(*content.positions));

    const std::optional<ResidencyRow> after = rowOf(backend->frameReport(), ResidencyKind::Streams);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->live, 1U) << "the index stream was not named by the release";
    EXPECT_LT(after->bytes.value_or(0U), *before->bytes);

    // THE NEXT FRAME RE-UPLOADS EXACTLY ONE STREAM, and draws the same picture.
    ASSERT_EQ(backend->executeFrame(drawingFrame(target.get(), sceneDraw(content, material))), PresentOutcome::Skipped);
    const FrameReport rerun = backend->frameReport();
    ASSERT_TRUE(rerun.uploads.has_value());
    EXPECT_EQ(rerun.uploads->stream_uploads, 1U);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

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
    EXPECT_EQ(fresh.counters->draws, 0U);
    EXPECT_FALSE(fresh.uploads.has_value());
    ASSERT_TRUE(fresh.progress.has_value());
    EXPECT_EQ(fresh.progress->submitted, 0U);
    EXPECT_EQ(fresh.progress->completed, 0U);

    // RESIDENCY IS ANSWERED AND EMPTY: the three kinds this backend retains are reported, at zero.
    const std::optional<ResidencyRow> streams = rowOf(fresh, ResidencyKind::Streams);
    ASSERT_TRUE(streams.has_value());
    EXPECT_EQ(streams->live, 0U);
    EXPECT_EQ(streams->bytes.value_or(1U), 0U);
    const std::optional<ResidencyRow> images = rowOf(fresh, ResidencyKind::Images);
    ASSERT_TRUE(images.has_value());
    EXPECT_EQ(images->live, 0U);
    const std::optional<ResidencyRow> halves = rowOf(fresh, ResidencyKind::Halves);
    ASSERT_TRUE(halves.has_value());
    EXPECT_EQ(halves->live, 0U);

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
    // (PrepareReport::Outcome::NotReady + EngineNotReady::NotInitialized), not a zero it did not earn.
    EXPECT_FALSE(backend->releaseContent(material.get()));
    const FrameDescription frame;
    const PrepareReport    prepared = backend->prepareFrame(frame);
    EXPECT_FALSE(prepared.prepared());
    EXPECT_EQ(prepared.outcome, PrepareReport::Outcome::NotReady);
    EXPECT_EQ(prepared.reason, EngineNotReady::NotInitialized);
    EXPECT_FALSE(backend->frameReport().retention->residency.has_value()) << "a shut-down device accounts for nothing";
}
