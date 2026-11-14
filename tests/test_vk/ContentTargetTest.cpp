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

#include <algorithm>
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

/// @brief Builds a 64x64-style target with one RGBA8 attachment and a depth attachment of @p format.
///
/// D32F is the format a depth READBACK can decode: D24 is Vulkan's COMBINED depth/stencil image, which has no plain
/// depth copy (see the refusal case at the end of this file, and the SDK's own `core::depthReadbackOf`).
[[nodiscard]] intrusive_ptr<RenderTarget> makeDepthTarget(int width, int height, RenderTarget::DepthFormat format, std::uint32_t samples = 1U)
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m3_depth_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(format);
    target->setSize(width, height);
    target->setSamples(samples);
    return target;
}

/// @brief Builds an indexed quad in clip space at depth @p z, covering x in [-1, 0] and y in [0, 1].
///
/// The SDK's clip space is y-UP and the backend folds y down, so this quad is the image's TOP-LEFT quarter. One
/// readback can then pin two facts about the packed copy at once: row-major (the depth sits in the first half of the
/// rows) and row length (the first half of the columns, not wrapped).
[[nodiscard]] intrusive_ptr<Geometry> makeCornerQuad(float z)
{
    std::vector<float> positions{
        -1.0F, 0.0F, z,
        0.0F,  0.0F, z,
        0.0F,  1.0F, z,
        -1.0F, 1.0F, z,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U, 0U, 2U, 3U };

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

/// @brief Builds an indexed quad in clip space at depth @p z, covering x in [@p x0, @p x1] and y in [@p y0, @p y1].
///
/// The general form of makeCornerQuad: a case that needs "this pass covered THIS region and no other" states the
/// region rather than reusing a quarter and hunting for pixels near its edge.
[[nodiscard]] intrusive_ptr<Geometry> makeRectQuad(float x0, float y0, float x1, float y1, float z)
{
    std::vector<float> positions{
        x0, y0, z,
        x1, y0, z,
        x1, y1, z,
        x0, y1, z,
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U, 0U, 2U, 3U };

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

/**
 * @brief The depth attachment reads back as normalised, row-major values - one per texel, the picture the frame left.
 *
 * WHY A CORNER QUAD. The geometry covers the image's top-left quarter only, so one readback says both things a packed
 * copy can get wrong: the ROW ORDER (a flipped read would report the depth at the bottom) and the ROW LENGTH (a short
 * stride would wrap the columns). The depth value is exact: clip z = 0.5 folds to `0.5 - 0.5 z` = 0.25, and D32F
 * stores the float the fragment wrote.
 */
TEST(VkContentTargetTest, TheDepthAttachmentReadsBackAsNormalisedRowMajorValues)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeDepthTarget(64, 64, RenderTarget::DepthFormat::D32F);
    const intrusive_ptr<ShaderProgram> program  = makeProgram(kGreenFragment);
    const intrusive_ptr<Geometry>      geometry = makeCornerQuad(0.5F);

    ClearPolicy clear;
    clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    clear.depth = true;

    EXPECT_EQ(backend->executeFrame(drawingFrame(target.get(), clear, PassRenderState{}, { commandOf(geometry, program) })),
              vn::graphics::PresentOutcome::Skipped);

    std::vector<float> depths;
    ReadbackResult     why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readDepthBuffer(target.get(), depths, &why)) << "the depth readback was refused: " << static_cast<int>(why);
    EXPECT_EQ(why, ReadbackResult::Ok);
    ASSERT_EQ(depths.size(), std::size_t{ 64U * 64U }) << "one value per texel";

    const auto depthAt = [&depths](int x, int y) { return depths[static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)]; };
    EXPECT_NEAR(depthAt(16, 16), 0.25F, 0.002F) << "the covered quarter carries the fragment's depth (clip z = 0.5 folds to 0.25)";
    EXPECT_FLOAT_EQ(depthAt(48, 16), 0.0F) << "outside the quad is the depth the pass cleared: the reverse-Z far plane";
    EXPECT_FLOAT_EQ(depthAt(16, 48), 0.0F) << "a vertically flipped read would see the quad's depth here";
    EXPECT_FLOAT_EQ(depthAt(48, 48), 0.0F);
    for (const float value : depths) {
        EXPECT_GE(value, 0.0F) << "a depth is a normalised value";
        EXPECT_LE(value, 1.0F);
    }

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::TargetBuildFailed), 0U)
        << "a D32F depth attachment is a shape this device builds";
    backend->shutdown();
}

/**
 * @brief A multisampled target's depth reads back as its RESOLVED picture, not the sample buffer.
 *
 * The render pass resolves the depth into a single-sample image (`TargetImages::depthPicture`, `sample0`, backlog
 * T40), and that image is what a reader sees - so the depths are the single-sampled ones, one per pixel. Reading the
 * sample buffer instead would be a multi-texels-per-pixel copy, which the packed size below (width * height) rules out.
 */
TEST(VkContentTargetTest, AMultisampledTargetsDepthReadsItsResolvePicture)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const std::uint32_t samples = std::min<std::uint32_t>(4U, backend->capabilities().max_samples);
    if (samples <= 1U) {
        GTEST_SKIP() << "this device offers no multisampling (Capabilities::max_samples is 1)";
    }

    const intrusive_ptr<RenderTarget>  target   = makeDepthTarget(64, 64, RenderTarget::DepthFormat::D32F, samples);
    const intrusive_ptr<ShaderProgram> program  = makeProgram(kGreenFragment);
    const intrusive_ptr<Geometry>      geometry = makeCornerQuad(0.5F);

    ClearPolicy clear;
    clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    clear.depth = true;

    EXPECT_EQ(backend->executeFrame(drawingFrame(target.get(), clear, PassRenderState{}, { commandOf(geometry, program) })),
              vn::graphics::PresentOutcome::Skipped);

    std::vector<float> depths;
    ReadbackResult     why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readDepthBuffer(target.get(), depths, &why))
        << "a multisampled target's resolved depth is served now (see TargetImages::depthPicture), refused: " << static_cast<int>(why);
    EXPECT_EQ(why, ReadbackResult::Ok);
    ASSERT_EQ(depths.size(), std::size_t{ 64U * 64U }) << "one value per PIXEL, not per sample";

    const auto depthAt = [&depths](int x, int y) { return depths[static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)]; };
    // sample0 is the resolve mode, and the quad's depth is the same in every sample, so the resolved picture IS the
    // single-sampled one - at the covered quarter and at the clear alike.
    EXPECT_NEAR(depthAt(16, 16), 0.25F, 0.002F) << "the resolved picture carries the fragment's depth";
    EXPECT_FLOAT_EQ(depthAt(48, 16), 0.0F) << "and the clear outside the quad";
    EXPECT_FLOAT_EQ(depthAt(48, 48), 0.0F);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

/**
 * @brief Every depth-readback refusal is classified, costs no submission, and leaves the host's vector alone.
 *
 * The reasons are the SDK's, not this backend's: a target it never built is NotReady (a later frame can make it
 * readable), a request with no target is Invalid, a COMBINED depth/stencil format and a BORROWED depth are
 * Unsupported (the SDK's readback table has no plain copy for the first, and the second belongs to the lender) - and
 * a destination that already held values still holds them after a refusal.
 */
TEST(VkContentTargetTest, DepthReadbackRefusalsKeepTheirReasonsAndTouchNothing)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    ClearPolicy clear;
    clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    clear.depth = true;

    // A BUILT D24 target: the SDK's table says a combined depth/stencil image has no plain depth copy, so this
    // backend refuses the read instead of decoding bytes that look like depths and are not.
    const intrusive_ptr<RenderTarget> combined = makeDepthTarget(32, 32, RenderTarget::DepthFormat::D24);
    EXPECT_EQ(backend->executeFrame(drawingFrame(combined.get(), clear, PassRenderState{}, {})), vn::graphics::PresentOutcome::Skipped);

    const std::vector<float> seeded{ 0.5F };
    std::vector<float>       depths = seeded;
    ReadbackResult           why    = ReadbackResult::Ok;
    const std::size_t        waits  = 0U;
    EXPECT_FALSE(backend->readDepthBuffer(combined.get(), depths, &why));
    EXPECT_EQ(why, ReadbackResult::Unsupported) << "D24 is combined depth/stencil: no plain depth copy exists";
    EXPECT_EQ(depths, seeded) << "a refused read leaves the destination untouched";

    // A BUILT target WITHOUT a depth attachment: the request names nothing (Invalid, not Unsupported - no format could
    // ever make it servable). Built by hand: `makeTarget` always attaches a depth.
    const intrusive_ptr<RenderTarget> colour_only(new RenderTarget());
    colour_only->attachColor(RenderTarget::ColorFormat::RGBA8);
    colour_only->setSize(32, 32);
    EXPECT_EQ(backend->executeFrame(drawingFrame(colour_only.get(), clear, PassRenderState{}, {})), vn::graphics::PresentOutcome::Skipped);
    EXPECT_FALSE(backend->readDepthBuffer(colour_only.get(), depths, &why));
    EXPECT_EQ(why, ReadbackResult::Invalid);
    EXPECT_EQ(depths, seeded);

    // A target this device never built: nothing to read YET.
    const intrusive_ptr<RenderTarget> unknown = makeDepthTarget(8, 8, RenderTarget::DepthFormat::D32F);
    EXPECT_FALSE(backend->readDepthBuffer(unknown.get(), depths, &why));
    EXPECT_EQ(why, ReadbackResult::NotReady);

    // No target at all: the request itself is wrong.
    EXPECT_FALSE(backend->readDepthBuffer(nullptr, depths, &why));
    EXPECT_EQ(why, ReadbackResult::Invalid);

    // A BORROWED depth is the lender's to read (the SDK's rule): the borrower says Unsupported, and the LENDER answers
    // its own depths. The borrower is drawn into first so it is BUILT - the refusal is then the borrow itself, not a
    // missing attachment set. (What a borrower ATTACHES is the lender's picture - see
    // ABorrowedDepthRejectsTheFragmentTheLenderAlreadyCovered; what is pinned here is the READBACK rule, which is the
    // SDK's and the same for every backend.)
    const intrusive_ptr<RenderTarget> lender = makeDepthTarget(16, 16, RenderTarget::DepthFormat::D32F);
    EXPECT_EQ(backend->executeFrame(drawingFrame(lender.get(), clear, PassRenderState{}, {})), vn::graphics::PresentOutcome::Skipped);
    const intrusive_ptr<RenderTarget> borrower = makeTarget(1, 16, 16);
    borrower->shareDepth(depthViewOf(lender));
    EXPECT_EQ(backend->executeFrame(drawingFrame(borrower.get(), clear, PassRenderState{}, {})), vn::graphics::PresentOutcome::Skipped);
    EXPECT_FALSE(backend->readDepthBuffer(borrower.get(), depths, &why));
    EXPECT_EQ(why, ReadbackResult::Unsupported) << "read it through the source target";
    EXPECT_EQ(depths, seeded);
    std::vector<float> lender_depths;
    ASSERT_TRUE(backend->readDepthBuffer(lender.get(), lender_depths, &why)) << "the lender's own depth is served, refused: " << static_cast<int>(why);
    ASSERT_EQ(lender_depths.size(), std::size_t{ 16U * 16U });
    for (const float value : lender_depths) {
        EXPECT_FLOAT_EQ(value, 0.0F) << "the lender's pass cleared its depth and drew nothing";
    }

    // A RELEASED target is NotReady again (a later frame announcing it can make it readable - this is residency, not
    // a capability).
    backend->releaseRenderTarget(combined.get());
    EXPECT_FALSE(backend->readDepthBuffer(combined.get(), depths, &why));
    EXPECT_EQ(why, ReadbackResult::NotReady);
    EXPECT_EQ(depths, seeded);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(waits, 0U);
    backend->shutdown();
}

/**
 * @brief A borrowed depth is the LENDER's image: the fragment the lender already covered is rejected.
 *
 * @details
 * - THE PIXELS ARE THE ONLY PROOF. "This target draws against that target's depth" is a claim about objects, and only
 *   a fragment whose fate depends on it can settle the claim. The lender covers the LEFT half at clip z = 0 (device
 *   depth 0.5 under the SDK's reverse-Z fold) and writes its depth; the borrower covers the WHOLE target at clip
 *   z = 0.25 (device depth 0.375, i.e. farther) against the borrowed image. With the GREATER comparison the borrower's
 *   fragments are rejected where the lender wrote and accepted where it did not - so a borrower that had built its own
 *   CLEARED depth would paint both halves, and one that drew nothing at all would leave the right half at its clear.
 * - THE LENDER'S DEPTH, READ BACK AFTERWARDS, tells the two halves apart by VALUE: 0.5 where the lender drew, 0.375
 *   where the borrower's fragment won. A borrower with a private depth image would leave the lender's right half at
 *   the clear value (0.0), so this is the same fact seen from the other side.
 */
TEST(VkContentTargetTest, ABorrowedDepthRejectsTheFragmentTheLenderAlreadyCovered)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> lender = makeDepthTarget(32, 32, RenderTarget::DepthFormat::D32F);

    // THE BORROWER OWNS NO DEPTH: one colour attachment and an edge to the lender's image (RenderTarget::shareDepth).
    const intrusive_ptr<RenderTarget> borrower(new RenderTarget());
    borrower->setName(String(u8"vk_shared_depth_borrower"));
    borrower->attachColor(RenderTarget::ColorFormat::RGBA8);
    borrower->setSize(32, 32);
    borrower->shareDepth(vn::graphics::depthViewOf(lender));

    const intrusive_ptr<ShaderProgram> red         = makeProgram(kRedFragment);
    const intrusive_ptr<ShaderProgram> green       = makeProgram(kGreenFragment);
    const intrusive_ptr<Geometry>      left_half   = makeRectQuad(-1.0F, -1.0F, 0.0F, 1.0F, 0.0F);
    const intrusive_ptr<Geometry>      everywhere  = makeRectQuad(-1.0F, -1.0F, 1.0F, 1.0F, 0.25F);

    ClearPolicy lender_clear;
    lender_clear.color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    lender_clear.depth = true;
    ClearPolicy borrower_clear;
    borrower_clear.color = Colorf(0.0F, 0.0F, 1.0F, 1.0F); // BLUE: what a REJECTED fragment leaves behind
    borrower_clear.depth = false;                          // it never clears the depth it borrows

    const auto passOf = [](RenderTarget* target,
                           const ClearPolicy& clear,
                           const intrusive_ptr<Geometry>& geometry,
                           const intrusive_ptr<ShaderProgram>& program) {
        auto list = std::make_shared<std::vector<RenderCommand>>();
        list->push_back(commandOf(geometry, program));

        DrawDescription draw;
        draw.kind     = DrawDescription::Kind::Scene;
        draw.camera   = identityCamera();
        draw.commands = list;

        PassDraw pass;
        pass.target = target;
        pass.clear  = clear;
        pass.draws.push_back(std::move(draw));

        FrameDescription::Pass entry;
        entry.pass = vn::test::framePassForTest();
        entry.draw = std::move(pass);
        return entry;
    };

    // ONE FRAME, LENDER FIRST: the SDK's shareDepth rule is that the source renders earlier in the same frame.
    FrameDescription frame;
    frame.passes.push_back(passOf(lender.get(), lender_clear, left_half, red));
    frame.passes.push_back(passOf(borrower.get(), borrower_clear, everywhere, green));
    ASSERT_EQ(backend->executeFrame(frame, nullptr), vn::graphics::PresentOutcome::Skipped);

    ReadbackResult            why = ReadbackResult::Ok;
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(borrower.get(), 0, pixels, &why)) << "borrower readback refused: " << static_cast<int>(why);
    ASSERT_EQ(pixels.size(), std::size_t{ 32U * 32U } * 4U);
    // (8, 16) is inside the lender's half and (24, 16) is not; both are well away from every edge.
    EXPECT_EQ(pixelAt(pixels, 32, 8, 16), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the lender's depth rejected the borrower's farther fragment - a borrower with its OWN cleared depth would paint here";
    EXPECT_EQ(pixelAt(pixels, 32, 24, 16), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "nothing covered the right half, so the borrower's fragment reached its colour attachment";

    // THE LENDER'S OWN COLOUR: the half it drew is red, the other half keeps its clear colour - a borrower writes its
    // own attachments, never the lender's colour.
    std::vector<std::uint8_t> lender_pixels;
    ASSERT_TRUE(backend->readColorBuffer(lender.get(), 0, lender_pixels, &why)) << "lender readback refused: " << static_cast<int>(why);
    EXPECT_EQ(pixelAt(lender_pixels, 32, 8, 16), (std::array<int, 4>{ 255, 0, 0, 255 }));
    EXPECT_EQ(pixelAt(lender_pixels, 32, 24, 16), (std::array<int, 4>{ 0, 0, 0, 255 }));

    // THE SAME IMAGE, SEEN AS DEPTH: 0.5 where the lender drew, 0.375 where the borrower's fragment won.
    std::vector<float> depths;
    ASSERT_TRUE(backend->readDepthBuffer(lender.get(), depths, &why)) << "lender depth readback refused: " << static_cast<int>(why);
    ASSERT_EQ(depths.size(), std::size_t{ 32U * 32U });
    const auto depthAt = [&depths](int x, int y) { return depths[static_cast<std::size_t>(y) * 32U + static_cast<std::size_t>(x)]; };
    EXPECT_NEAR(depthAt(8, 16), 0.5F, 0.002F) << "the lender's own depth, written by its own pass";
    EXPECT_NEAR(depthAt(24, 16), 0.375F, 0.002F) << "the borrower wrote its depth INTO the lender's image";

    // The borrow's own refusal, unchanged: a borrowed depth is read through the lender (see RenderBackend).
    std::vector<float> borrowed_depths;
    EXPECT_FALSE(backend->readDepthBuffer(borrower.get(), borrowed_depths, &why));
    EXPECT_EQ(why, ReadbackResult::Unsupported);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}
