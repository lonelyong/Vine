/**
 * @brief M11's acceptance: multi-sampled off-screen targets.
 *
 * WHAT THE THREE CASES PIN, and why each is a different question:
 *   - THE LIMIT IS THE DEVICE'S ANSWER: `Capabilities::max_samples` is a sample COUNT (not a set of bits) and it is
 *     above 1 on a device that can multisample at all - the number a host reads before it chooses `setSamples`.
 *   - THE PICTURE SURVIVES THE RESOLVE: a 64x64 RGBA8 + D24 target drawn with `samples = 4` (or the largest count
 *     the device offers) hands back the fragment stage's colour at the centre AND at a corner through
 *     `readColorBuffer`, which reads the RESOLVE image - so a target whose resolve never ran, or whose framebuffer
 *     names its attachments in the wrong order, answers the clear instead.
 *   - THE SAME FRAME, SINGLE-SAMPLED, IS THE SAME PICTURE: the two targets are drawn with one program and one
 *     camera, and the centre pixels must be equal - multisampling changes edges, never the colour of a covered pixel.
 *   - AN OVER-LARGE REQUEST IS SERVED AND REPORTED ONCE: asking above the device's limit draws the picture at the
 *     largest count the device offers and reports it (`UnsupportedRequest`); running the same frame again must not
 *     report it a second time.
 *
 * THE SCENE IS STATED IN CLIP SPACE on purpose (identity camera, identity model): every number below is then the
 * value the hardware sees, with no camera derivation in the way. The triangle sits at clip z = 0.5 because the pass
 * policy TESTS depth and this backend is reverse-Z (the far plane is 0, which is what depth is cleared to).
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/Capabilities.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DiagnosticCategory;
using vn::graphics::DiagnosticSeverity;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderDiagnostic;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The M4 content ABI's camera push: identities make the clip-space triangle BE the picture.
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

/// @brief A fragment stage writing ONE constant colour: no material, texture or light block is involved.
constexpr const char8_t* kConstantFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(0.2, 0.4, 0.6, 1.0);
}
)";

/// @brief The fragment stage's colour in packed RGBA8 (0.2 / 0.4 / 0.6 are exact in UNORM8).
constexpr std::array<int, 4> kFragmentColor{ 51, 102, 153, 255 };

/// @brief The sample count the picture cases ask for: four, or the largest the device offers when that is lower.
[[nodiscard]] std::uint32_t pictureSamples(const std::uint32_t max_samples) noexcept
{
    return std::min<std::uint32_t>(4U, max_samples);
}

/// @brief Builds the two-stage program above (fresh object per call, like every SDK factory).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m11_program");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kConstantFragment), String(u8"main") });
    return program;
}

/// @brief A triangle in clip space that covers the whole viewport, so centre AND corner are inside it.
[[nodiscard]] intrusive_ptr<Geometry> makeFullScreenTriangle()
{
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -1.0F, -1.0F, 0.5F, // bottom left
        3.0F,  -1.0F, 0.5F, // bottom right, beyond the right edge
        -1.0F, 3.0F,  0.5F, // top, beyond the top edge
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 64x64 RGBA8 + D24 target drawn with @p samples samples per pixel.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget(const std::uint32_t samples)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m11_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    target->setSamples(samples);
    return target;
}

/// @brief A camera whose matrices are identities, so the triangle's clip-space positions ARE the picture.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    camera.eye        = vn::math::Vec3d(0.0, 0.0, 0.0);
    return camera;
}

/// @brief One scene draw of the full-screen triangle through @p program.
[[nodiscard]] DrawDescription constantDraw(const intrusive_ptr<ShaderProgram>& program, const intrusive_ptr<Geometry>& geometry)
{
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = std::move(commands);
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
    pass.target        = target;
    pass.clear         = ClearPolicy{};
    pass.clear->color  = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.clear->depth  = true;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief Prints a pixel under the test's own prefix.
void printPixel(const char* label, const std::array<int, 4>& pixel)
{
    std::printf("[vk-msaa] %s: r=%d g=%d b=%d a=%d\n", label, pixel[0], pixel[1], pixel[2], pixel[3]);
}

/// @brief Asserts that @p pixel is the fragment stage's colour (the UNORM conversion may round by one).
void expectFragmentColor(const std::array<int, 4>& pixel, const char* label)
{
    for (std::size_t channel = 0; channel < kFragmentColor.size(); ++channel) {
        EXPECT_LE(std::abs(pixel[channel] - kFragmentColor[channel]), 1)
            << label << " is not the fragment stage's colour in channel " << channel;
    }
}

} // namespace

TEST(VkMsaaTest, TheDeviceReportsTheSampleCountItCanBuild)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const vn::graphics::Capabilities capabilities = backend->capabilities();
    std::printf("[vk-msaa] capabilities().max_samples = %u\n", capabilities.max_samples);

    if (capabilities.max_samples <= 1U) {
        GTEST_SKIP() << "this device genuinely reports no multisampling (Capabilities::max_samples is 1)";
    }

    // A COUNT, not a set of bits: Capabilities::max_samples is the largest count a target may ask for, so the value a
    // host hands to RenderTarget::setSamples has to be one of the counts Vulkan offers (a power of two).
    EXPECT_GT(capabilities.max_samples, 1U) << "this device reports no multisampling";
    EXPECT_EQ(capabilities.max_samples & (capabilities.max_samples - 1U), 0U)
        << "Capabilities::max_samples is documented as a sample COUNT, and " << capabilities.max_samples << " is not one";

    backend->shutdown();
}

TEST(VkMsaaTest, AMultisampledTargetResolvesTheSamePicture)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const vn::graphics::Capabilities capabilities = backend->capabilities();
    const std::uint32_t              samples      = pictureSamples(capabilities.max_samples);
    if (samples <= 1U) {
        GTEST_SKIP() << "this device genuinely reports no multisampling (Capabilities::max_samples is 1)";
    }
    std::printf("[vk-msaa] drawing the picture with %u samples (device limit %u)\n", samples, capabilities.max_samples);

    // WHAT THE DRIVER SAYS WHILE THE FRAME RUNS: a resolve, a multisampled framebuffer and a pipeline whose
    // rasterisation sample count must match the render pass are legal-or-not facts, and this is the only instrument
    // for them.
    std::vector<RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const RenderDiagnostic& diagnostic) { driver_messages.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget>  multisampled = makeTarget(samples);
    const intrusive_ptr<RenderTarget>  single       = makeTarget(1U);
    const intrusive_ptr<ShaderProgram> program      = makeProgram();
    const intrusive_ptr<Geometry>      geometry     = makeFullScreenTriangle();

    // THE SAME FRAME VALUE, DRAWN INTO BOTH TARGETS: the only difference between the two pictures is the sample
    // count, so a colour difference is the sample count's doing.
    const PresentOutcome multisampled_outcome =
        backend->executeFrame(drawingFrame(multisampled.get(), constantDraw(program, geometry)), nullptr);
    EXPECT_EQ(multisampled_outcome, PresentOutcome::Skipped)
        << "a device with no window presents nothing; a target is not a surface";

    const PresentOutcome single_outcome = backend->executeFrame(drawingFrame(single.get(), constantDraw(program, geometry)), nullptr);
    EXPECT_EQ(single_outcome, PresentOutcome::Skipped);

    std::vector<std::uint8_t> multisampled_pixels;
    ReadbackResult            why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(multisampled.get(), 0, multisampled_pixels, &why))
        << "the multisampled target's readback was refused: " << static_cast<int>(why)
        << " (readColorBuffer reads the RESOLVE image)";
    ASSERT_EQ(multisampled_pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    std::vector<std::uint8_t> single_pixels;
    why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(single.get(), 0, single_pixels, &why))
        << "the single-sampled target's readback was refused: " << static_cast<int>(why);
    ASSERT_EQ(single_pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    const std::array<int, 4> multisampled_centre = pixelAt(multisampled_pixels, kTargetSize / 2, kTargetSize / 2);
    const std::array<int, 4> multisampled_corner = pixelAt(multisampled_pixels, 0, 0);
    const std::array<int, 4> single_centre      = pixelAt(single_pixels, kTargetSize / 2, kTargetSize / 2);
    printPixel("multisampled centre", multisampled_centre);
    printPixel("multisampled corner", multisampled_corner);
    printPixel("single-sampled centre", single_centre);

    // THE RESOLVE ANSWERED THE FRAGMENT STAGE'S COLOUR, at the centre and at the corner (the triangle covers both).
    expectFragmentColor(multisampled_centre, "the multisampled centre");
    expectFragmentColor(multisampled_corner, "the multisampled corner");

    // A FULLY COVERED PIXEL IS THE SAME COLOUR WITH AND WITHOUT MULTISAMPLING: the resolve averages samples, and every
    // sample of this pixel carries the same value.
    EXPECT_EQ(multisampled_centre, single_centre)
        << "multisampling changed the colour of a fully covered pixel: the resolve, the framebuffer order or the "
           "pipeline's rasterisation sample count is wrong";

    EXPECT_EQ(backend->diagnosticCount(DiagnosticCategory::CompileFailed), 0U)
        << "the constant-colour program has to compile on this backend";
    EXPECT_EQ(backend->diagnosticCount(DiagnosticCategory::ContentSkipped), 0U)
        << "a drawing call into the multisampled target was skipped";
    for (const RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, DiagnosticSeverity::Error)
            << "the driver rejected an object of the multisampled frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}

TEST(VkMsaaTest, AnOverLimitRequestIsServedAndReportedOnce)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const vn::graphics::Capabilities capabilities = backend->capabilities();
    if (capabilities.max_samples <= 1U) {
        GTEST_SKIP() << "this device genuinely reports no multisampling (Capabilities::max_samples is 1)";
    }

    // ABOVE THE LIMIT ON PURPOSE: twice the largest count the device offers (the clamp serves the largest one).
    const std::uint32_t requested = capabilities.max_samples * 2U;
    std::printf("[vk-msaa] asking for %u samples where the device's limit is %u\n", requested, capabilities.max_samples);

    std::vector<RenderDiagnostic> driver_messages;
    backend->setDiagnosticSink([&driver_messages](const RenderDiagnostic& diagnostic) { driver_messages.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget>  target   = makeTarget(requested);
    const intrusive_ptr<ShaderProgram> program  = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeFullScreenTriangle();

    // THE FIRST FRAME: the target is built at the largest count the device offers and the picture is drawn into it.
    (void)backend->executeFrame(drawingFrame(target.get(), constantDraw(program, geometry)), nullptr);

    const std::size_t reported_once = backend->diagnosticCount(DiagnosticCategory::UnsupportedRequest);
    EXPECT_GT(reported_once, 0U) << "a request above the device's limit was not reported";

    std::vector<std::uint8_t> pixels;
    ReadbackResult            why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why))
        << "an over-limit request must still be SERVED (clamped), but its readback was refused: " << static_cast<int>(why);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    const std::array<int, 4> centre = pixelAt(pixels, kTargetSize / 2, kTargetSize / 2);
    printPixel("clamped target centre", centre);
    expectFragmentColor(centre, "the clamped target's centre");

    // THE SECOND FRAME, the same target and the same request: the diagnostic must not be repeated.
    (void)backend->executeFrame(drawingFrame(target.get(), constantDraw(program, geometry)), nullptr);
    EXPECT_EQ(backend->diagnosticCount(DiagnosticCategory::UnsupportedRequest), reported_once)
        << "the same over-limit request was reported again: the report is supposed to happen ONCE per device";

    EXPECT_EQ(backend->diagnosticCount(DiagnosticCategory::CompileFailed), 0U);
    for (const RenderDiagnostic& message : driver_messages) {
        EXPECT_NE(message.severity, DiagnosticSeverity::Error)
            << "the driver rejected an object of the clamped frame: "
            << std::string(reinterpret_cast<const char*>(message.message.data()), message.message.size());
    }

    backend->shutdown();
}
