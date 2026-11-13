/**
 * @brief M3's acceptance: MRT clears/writes per attachment, a resize keeps the pipelines, and the pass' depth policy
 *        decides which fragment survives.
 *
 * What each case pins:
 *   - MRT: every colour attachment is its own image, cleared to its OWN colour (`ClearPolicy::extra_colors`) and
 *     written by its own fragment output location - two facts a single-attachment target cannot tell apart;
 *   - RESIZE: an extent change replaces the images and the framebuffer but not the render passes, so the pipeline is
 *     REUSED (`Device::contentPipelines()` does not move); a SHAPE change does rebuild it. That is `planTarget`'s
 *     ResizeInPlace vs Rebuild, observed from outside;
 *   - DEPTH: the default pass policy tests and writes depth (the SDK projection puts the near plane at clip z = -1,
 *     and the backend stores it as the larger device depth), so a farther fragment drawn later must be rejected;
 *     with `DepthMode::Disabled` the later fragment wins.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/backend_vk/internal/Device.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DepthMode;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PassRenderState;
using vn::graphics::Primitive;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Topology;
using vn::intrusive_ptr;

namespace
{

/// @brief The M4 content ABI: the camera pair is the 128-byte push (what the draw block used to carry is derived
///        inside it).
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

/// @brief A fragment stage writing one opaque colour.
constexpr const char8_t* kRedFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

constexpr const char8_t* kGreenFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

/// @brief A fragment stage writing three attachments - the MRT case.
constexpr const char8_t* kMrFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color0;
layout(location = 1) out vec4 out_color1;
layout(location = 2) out vec4 out_color2;
void main()
{
    out_color0 = vec4(1.0, 0.0, 0.0, 1.0);
    out_color1 = vec4(0.0, 1.0, 0.0, 1.0);
    out_color2 = vec4(0.0, 0.0, 1.0, 1.0);
}
)";

/// @brief Builds a two-stage program (fresh object per call, like every SDK factory).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram(const char8_t* fragment_source)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m3_program");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_source), String(u8"main") });
    return program;
}

/// @brief Builds an indexed triangle in clip space at depth @p z (a SMALLER z is nearer: the SDK projection puts the
///        near plane at clip z = -1, which the backend stores as the larger device depth).
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(float z)
{
    std::vector<float> positions{
        -0.75F, -0.75F, z,
        0.75F,  -0.75F, z,
        0.0F,   0.75F,  z,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief Builds a target with @p color_count RGBA8 attachments and D24 depth.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(int color_count, int width = 64, int height = 64)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m3_target"));
    for (int index = 0; index < color_count; ++index) {
        target->attachColor(RenderTarget::ColorFormat::RGBA8);
    }
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(width, height);
    return target;
}

/// @brief A camera with identity matrices, so the triangle's clip-space positions ARE the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
    return camera;
}

/// @brief One command drawing @p geometry with @p program.
[[nodiscard]] RenderCommand commandOf(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program)
{
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    return command;
}

/// @brief A frame with ONE pass that clears @p target (with @p clear) and issues @p commands.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, const ClearPolicy& clear, PassRenderState state, std::vector<RenderCommand> commands)
{
    auto list = std::make_shared<std::vector<RenderCommand>>(std::move(commands));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = list;

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target = target;
    pass.clear  = clear;
    pass.state  = state;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback of @p width pixels per row.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int width, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief The packed byte a linear colour component writes (one UNORM step of rounding is allowed by callers).
[[nodiscard]] int byteOf(float value)
{
    return static_cast<int>(value * 255.0F + 0.5F);
}

/// @brief Asserts one pixel is @p expected within one UNORM step.
void expectPixel(const std::vector<std::uint8_t>& pixels, int width, int x, int y, const std::array<int, 4>& expected, const char* what)
{
    const std::array<int, 4> actual = pixelAt(pixels, width, x, y);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_LE(std::abs(actual[channel] - expected[channel]), 1) << what << ": channel " << channel << " is " << actual[channel] << ", expected " << expected[channel];
    }
}

} // namespace

TEST(VkContentTargetTest, EveryColourAttachmentIsClearedAndWrittenOnItsOwn)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    ASSERT_GE(backend->capabilities().max_color_attachments, 3U) << "M3 serves MRT up to four attachments";

    const intrusive_ptr<RenderTarget>  target   = makeTarget(3);
    const intrusive_ptr<ShaderProgram> program  = makeProgram(kMrFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);

    ClearPolicy clear;
    clear.color        = Colorf(0.25F, 0.5F, 0.75F, 1.0F);
    clear.extra_colors = { Colorf(0.1F, 0.2F, 0.3F, 1.0F), Colorf(0.4F, 0.5F, 0.6F, 1.0F) };

    const vn::graphics::PresentOutcome outcome =
        backend->executeFrame(drawingFrame(target.get(), clear, PassRenderState{}, { commandOf(geometry, program) }));
    EXPECT_EQ(outcome, vn::graphics::PresentOutcome::Skipped);

    // EVERY ATTACHMENT, READ ON ITS OWN. The centre is where the triangle is; the corner is where only the clear is.
    const std::array<std::array<int, 4>, 3> written{ { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } } };
    const std::array<Colorf, 3>             cleared{ clear.color, clear.extra_colors[0], clear.extra_colors[1] };
    for (int attachment = 0; attachment < 3; ++attachment) {
        std::vector<std::uint8_t> pixels;
        ReadbackResult            why = ReadbackResult::Failed;
        ASSERT_TRUE(backend->readColorBuffer(target.get(), attachment, pixels, &why))
            << "attachment " << attachment << " refused: " << static_cast<int>(why);
        ASSERT_EQ(pixels.size(), std::size_t{ 64U * 64U } * 4U);

        expectPixel(pixels, 64, 32, 32, written[attachment], "the attachment's fragment output");
        expectPixel(pixels,
                    64,
                    2,
                    2,
                    { byteOf(cleared[attachment].r), byteOf(cleared[attachment].g), byteOf(cleared[attachment].b), byteOf(cleared[attachment].a) },
                    "the attachment's own clear colour");
    }

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentTargetTest, AResizeKeepsThePipelinesAndAShapeChangeRebuildsThem)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>  target   = makeTarget(1);
    const intrusive_ptr<ShaderProgram> program  = makeProgram(kRedFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle(0.5F);
    const std::vector<RenderCommand>   commands{ commandOf(geometry, program) };

    ClearPolicy clear;
    clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    const auto drawAndProbeCentre = [&](int width, int height) {
        const vn::graphics::PresentOutcome outcome = backend->executeFrame(drawingFrame(target.get(), clear, PassRenderState{}, commands));
        EXPECT_EQ(outcome, vn::graphics::PresentOutcome::Skipped);
        std::vector<std::uint8_t> pixels;
        ReadbackResult            why = ReadbackResult::Failed;
        EXPECT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "readback refused: " << static_cast<int>(why);
        if (pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U) {
            expectPixel(pixels, width, width / 2, height / 2, { 255, 0, 0, 255 }, "the triangle after the frame");
        }
        else {
            EXPECT_EQ(pixels.size(), static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
        }
    };

    drawAndProbeCentre(64, 64);
    const std::uint64_t afterFirstFrame = device->contentPipelines();
    EXPECT_EQ(afterFirstFrame, 1U) << "one program, one layout: the first frame builds one pipeline";

    // RESIZE IN PLACE: new images and a new framebuffer, the SAME render passes - so the pipeline is reused.
    target->setSize(96, 48);
    drawAndProbeCentre(96, 48);
    EXPECT_EQ(device->contentPipelines(), afterFirstFrame) << "an extent change recompiled the content pipeline";

    // SHAPE CHANGE: a second colour attachment is a different render pass, so a different pipeline.
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(64, 64);
    drawAndProbeCentre(64, 64);
    EXPECT_EQ(device->contentPipelines(), afterFirstFrame + 1U) << "a shape change must rebuild the pipeline";

    // And a steady frame after that leaves it alone again.
    drawAndProbeCentre(64, 64);
    EXPECT_EQ(device->contentPipelines(), afterFirstFrame + 1U);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkContentTargetTest, ThePassDepthPolicyDecidesWhichFragmentSurvives)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<ShaderProgram> green_program = makeProgram(kGreenFragment);
    const intrusive_ptr<ShaderProgram> red_program   = makeProgram(kRedFragment);
    const intrusive_ptr<Geometry>      near_geometry = makeTriangle(0.25F); // near: SMALLER clip z
    const intrusive_ptr<Geometry>      far_geometry  = makeTriangle(0.75F);

    // NEAR first, FAR second: the default policy tests and writes depth, so the far fragment is rejected.
    const std::vector<RenderCommand> ordered{ commandOf(near_geometry, green_program), commandOf(far_geometry, red_program) };

    ClearPolicy clear;
    clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);

    {
        const intrusive_ptr<RenderTarget> target = makeTarget(1);
        backend->executeFrame(drawingFrame(target.get(), clear, PassRenderState{}, ordered));

        std::vector<std::uint8_t> pixels;
        ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
        expectPixel(pixels, 64, 32, 32, { 0, 255, 0, 255 }, "the nearer fragment drawn first must keep the pixel");
    }

    // THE SAME ORDER with depth disabled: nothing is tested, so the later fragment wins.
    {
        PassRenderState no_depth;
        no_depth.depth = DepthMode::Disabled;

        const intrusive_ptr<RenderTarget> target = makeTarget(1);
        backend->executeFrame(drawingFrame(target.get(), clear, no_depth, ordered));

        std::vector<std::uint8_t> pixels;
        ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
        expectPixel(pixels, 64, 32, 32, { 255, 0, 0, 255 }, "a pass with depth disabled must let the later draw win");
    }

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}
