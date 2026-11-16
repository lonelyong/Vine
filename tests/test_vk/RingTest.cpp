/**
 * @brief The vk backend's COST MODEL: one uniform ring bound through dynamic offsets, and a descriptor-set cache.
 *
 * WHY THIS FILE EXISTS. The review of 2026-11-14 (.ai/research/graphics-2026-11-14-vk-industry-review.md F1/F2) found
 * the backend creating and destroying a buffer AND its memory per draw, and allocating a descriptor set per draw per
 * frame. The fix is an architectural one - a draw's constants are a SLICE of one persistent buffer (bound as dynamic
 * offsets), and a set is a BINDING OF RESOURCES (cached by the images it names) - and a change of that shape is only
 * worth anything if it is pinned: a later edit that goes back to "one buffer per draw" must fail here, not merely
 * look different in a profile.
 *
 * THE WITNESSES ARE THE BACKEND'S OWN (`Device::contentCacheCounters`), which is the same arrangement
 * `contentPipelines()` uses for "a resize must not recompile": the test asks the recorder what it did instead of
 * counting driver calls from outside.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Colorf.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The content ABI's vertex stage (the camera pair is the push, the draw block is set 1 binding 0).
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} vn_pc;
layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;
void main()
{
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(in_position, 1.0);
    gl_PointSize = vn_draw.params.y;
}
)";

/// @brief The content ABI's fragment stage: set 0 binding 0 is the material block.
constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 specular;
    vec4 params;
    vec4 extra;
} vn_material;
layout(location = 0) out vec4 out_color;
void main()
{
    out_color = vn_material.base_color;
}
)";

[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_ring_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d(0.0, 0.0, 0.0);
    return camera;
}

[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F,
        0.75F,  -0.75F, 0.5F,
        0.0F,   0.75F,  0.5F,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));
    Primitive primitive;
    primitive.topology = vn::graphics::Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_ring_program");
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(vn::graphics::ShaderStage{ vn::graphics::ShaderStageType::Fragment, String(kFragmentStage), String(u8"main") });
    return program;
}

/// @brief One frame with one pass clearing @p target and drawing @p commands.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, std::shared_ptr<const std::vector<RenderCommand>> commands)
{
    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = std::move(commands);

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

} // namespace

TEST(VkUniformRingTest, ASteadyFrameCreatesNoRingAndBuildsNoSet)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<Material>      material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    auto commands = std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ std::move(command) });

    const FrameDescription frame = drawingFrame(target.get(), commands);

    // FRAME ONE: the ring and the sets this drawable needs come into existence.
    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::Device::ContentCacheCounters first = device->contentCacheCounters();
    EXPECT_EQ(first.ring_builds, 1U) << "a frame must reuse the ring it came up with";
    EXPECT_GE(first.set_builds, 2U) << "a content draw binds a content set and the per-draw set";
    EXPECT_GE(first.sets_live, 2U);

    std::vector<std::uint8_t> before;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, before));
    EXPECT_EQ(pixelAt(before, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    // FRAMES TWO AND THREE: the same resources - so the ring is not rebuilt and no set is built again. THIS is the
    // regression: "one buffer per draw" and "one set per draw, per frame" both fail these two assertions.
    for (int frame_index = 0; frame_index < 2; ++frame_index) {
        (void)backend->executeFrame(frame, nullptr);
    }
    const vn::backend_vk::Device::ContentCacheCounters steady = device->contentCacheCounters();
    EXPECT_EQ(steady.ring_builds, first.ring_builds) << "a steady frame replaced the uniform ring";
    EXPECT_EQ(steady.set_builds, first.set_builds) << "a steady frame built a descriptor set for a draw it had already served";
    EXPECT_EQ(steady.sets_live, first.sets_live) << "the cache grew across steady frames";

    // AND THE PICTURE IS STILL THE ONE THE FIRST FRAME DREW: the slices are rewritten every frame, so "no churn" must
    // not have been bought by drawing something else.
    std::vector<std::uint8_t> after;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, after));
    EXPECT_EQ(after, before) << "the steady frame drew a different picture";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkUniformRingTest, TwoDrawablesWithDifferentMaterialsShareTheRing)
{
    // THE OTHER HALF OF THE CACHE RULE: a second material names DIFFERENT resources, so it gets its own content set -
    // but the ring and the per-draw set are shared, which is what keeps a scene's cost from growing per draw.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    const auto make_command = [&](const Colorf& colour) {
        auto material = intrusive_ptr<Material>(new Material());
        material->setBaseColor(colour);
        RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
        command.program              = program;
        command.program_description  = program->description();
        command.material             = material;
        command.material_description = material->description();
        return command;
    };

    const auto commands = std::make_shared<const std::vector<RenderCommand>>(
        std::vector<RenderCommand>{ make_command(Colorf(1.0F, 0.0F, 0.0F, 1.0F)), make_command(Colorf(0.0F, 1.0F, 0.0F, 1.0F)) });
    const FrameDescription frame = drawingFrame(target.get(), commands);

    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::Device::ContentCacheCounters first = device->contentCacheCounters();
    // Two materials with no texture bind the SAME image views (the white stand-ins), so the cache is allowed to answer
    // one set for both - what must hold is that a second frame builds nothing new.
    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::Device::ContentCacheCounters steady = device->contentCacheCounters();

    EXPECT_EQ(steady.ring_builds, 1U);
    EXPECT_EQ(steady.set_builds, first.set_builds) << "the second frame built a set it had already built";
    EXPECT_LE(steady.sets_live, 4U) << "the cache grows with resources, not with draws or frames";

    backend->shutdown();
}

TEST(VkDeviceClockTest, AFrameReportsItsDeviceTime)
{
    // THE FRAMEWORK'S `FrameReport::timing` (see FrameTiming): what a frame cost ON THE DEVICE, not on the CPU. This
    // backend writes one timestamp pair around the frame's command buffer and reads it once the fence signals, so the
    // answer describes the frame that just ended - and a device without the clock leaves the part disengaged rather
    // than answering a zero nobody measured.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<Material>      material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    auto       commands          = std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ std::move(command) });
    const FrameDescription frame = drawingFrame(target.get(), commands);

    // THE CLOCK IS SAMPLED (see kTimestampSampleFrames in the backend): the first frame is measured, the frames in
    // between answer DISENGAGED - never the previous frame's value - and the thirty-first is measured again. A report
    // describes the frame that just ended, so "not sampled" must not read as "took 0 ms" or as a stale number.
    (void)backend->executeFrame(frame, nullptr);
    const vn::graphics::FrameReport first = backend->frameReport();
    if (!first.timing.has_value()) {
        GTEST_SKIP() << "this device reports no timestamp clock (timestampComputeAndGraphics is false)";
    }
    EXPECT_GT(first.timing->gpu_micros, 0U) << "a frame that ran cannot have cost no device time";

    (void)backend->executeFrame(frame, nullptr);
    EXPECT_FALSE(backend->frameReport().timing.has_value()) << "an unsampled frame repeated a value it did not measure";

    // Frames 3..31 (two have run): the thirty-first is the next sample (see kTimestampSampleFrames).
    for (int frame_index = 0; frame_index < 29; ++frame_index) {
        (void)backend->executeFrame(frame, nullptr);
    }
    const vn::graphics::FrameReport sampled_again = backend->frameReport();
    ASSERT_TRUE(sampled_again.timing.has_value()) << "the sampling interval never came round";
    EXPECT_GT(sampled_again.timing->gpu_micros, 0U);
    EXPECT_LT(sampled_again.timing->gpu_micros, 10'000'000U) << "the value looks cumulative, not per frame";

    backend->shutdown();
    EXPECT_FALSE(backend->frameReport().timing.has_value()) << "a device that is down keeps no clock";
}

TEST(VkFrameHeadTest, AFramesCacheWorkDoesNotGrowWithItsDrawingCalls)
{
    // THE FRAME HEAD IS THE ONLY PLACE A FRAME CONSULTS THE DEVICE'S CACHES (S6c-1): a steady frame resolves every
    // program, stream, pipeline and texture slot under ONE hold, and the recording path then works from values (see
    // ContentCaches::CommandPlan). The witness is the cache's own acquisition counter: if a later edit goes back to
    // resolving per drawing call, the delta below grows with the number of commands and this case fails.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<Material>      material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    const auto make_commands = [&](std::size_t count) {
        std::vector<RenderCommand> commands;
        commands.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
            command.program              = program;
            command.program_description  = program->description();
            command.material             = material;
            command.material_description = material->description();
            commands.push_back(std::move(command));
        }
        return std::make_shared<const std::vector<RenderCommand>>(std::move(commands));
    };

    const FrameDescription one_command = drawingFrame(target.get(), make_commands(1));
    const FrameDescription six_commands = drawingFrame(target.get(), make_commands(6));

    // FRAME ONE: every identity is a MISS (compile, upload, pipeline). The misses are built inside the head's one
    // hold, so they cost the same acquisitions a steady frame does.
    const std::uint64_t before_first = device->contentCacheCounters().cache_lock_acquisitions;
    (void)backend->executeFrame(one_command, nullptr);
    const std::uint64_t after_first = device->contentCacheCounters().cache_lock_acquisitions;
    const std::uint64_t first_delta = after_first - before_first;

    // FRAME TWO: one drawing call, STEADY (nothing to build).
    const std::uint64_t before_steady = device->contentCacheCounters().cache_lock_acquisitions;
    (void)backend->executeFrame(one_command, nullptr);
    const std::uint64_t steady_delta = device->contentCacheCounters().cache_lock_acquisitions - before_steady;

    // FRAME THREE: the same pass, SIX times the drawing commands. The recording path takes no lock, so the frame head's
    // cost must be identical - this is the assertion "one hold per frame" is shortened to.
    const std::uint64_t before_six = device->contentCacheCounters().cache_lock_acquisitions;
    (void)backend->executeFrame(six_commands, nullptr);
    const std::uint64_t six_delta = device->contentCacheCounters().cache_lock_acquisitions - before_six;

    EXPECT_EQ(six_delta, steady_delta) << "the cache work grew with the number of drawing commands";
    EXPECT_EQ(first_delta, steady_delta) << "a frame with misses paid the cache more than a steady one";
    EXPECT_LE(six_delta, 8U) << "the per-frame cache work is the head plus the two book reads and the frame end";

    // AND THE SIX-COMMAND FRAME REALLY DREW: a cheaper head must not have been bought by skipping the picture.
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }));

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkFrameHeadTest, AFrameReportsWhatItSpentOnTheCacheLock)
{
    // THE THREE-PHASE RULE HAS A NUMBER (see FrameLocking): the frame says how many times its own phases took the
    // cache lock, and whether any of those had to wait. Two frames drawing different numbers of calls must report the
    // SAME acquisitions (the head's one hold, not one per call), and a serial loop must report NO waits - the number
    // that would move first if the recording path went back to taking the lock.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    EXPECT_FALSE(backend->frameReport().locking.has_value()) << "no frame has run yet";

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<Material>      material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    const auto make_commands = [&](std::size_t count) {
        std::vector<RenderCommand> commands;
        commands.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
            command.program              = program;
            command.program_description  = program->description();
            command.material             = material;
            command.material_description = material->description();
            commands.push_back(std::move(command));
        }
        return std::make_shared<const std::vector<RenderCommand>>(std::move(commands));
    };

    (void)backend->executeFrame(drawingFrame(target.get(), make_commands(1)), nullptr);
    const vn::graphics::FrameReport first = backend->frameReport();
    ASSERT_TRUE(first.locking.has_value()) << "a frame that ran measured its locks";
    EXPECT_GT(first.locking->acquisitions, 0U) << "the head takes the cache lock at least once";
    EXPECT_EQ(first.locking->waits, 0U) << "a serial frame never waits for another";

    (void)backend->executeFrame(drawingFrame(target.get(), make_commands(1)), nullptr);
    const vn::graphics::FrameReport steady = backend->frameReport();
    ASSERT_TRUE(steady.locking.has_value());
    EXPECT_EQ(steady.locking->acquisitions, first.locking->acquisitions) << "a steady frame costs the same as the first";

    (void)backend->executeFrame(drawingFrame(target.get(), make_commands(6)), nullptr);
    const vn::graphics::FrameReport six = backend->frameReport();
    ASSERT_TRUE(six.locking.has_value());
    EXPECT_EQ(six.locking->acquisitions, steady.locking->acquisitions)
        << "the cache-lock work grew with the number of drawing calls";
    EXPECT_EQ(six.locking->waits, 0U) << "a serial loop waited for a lock nobody else held";

    backend->shutdown();
}
