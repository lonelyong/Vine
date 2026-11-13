/**
 * @brief M2's acceptance: one content draw puts a triangle on the off-screen target, and the probe reads it.
 *
 * What this pins, and why each half matters:
 *   - a `DrawDescription` is enough to draw from: the camera snapshot, the command list and the program text cross
 *     the seam as values, so the backend needs nothing from a live scene or a live `ShaderProgram`;
 *   - `ShaderProgramDescription` compiles through glslang into a pipeline, and `GeometryDescription` becomes the
 *     vertex/index streams the pipeline reads - the first half of the test needs no device, the second needs one;
 *   - the pixel INSIDE the triangle is the shader's colour and the pixel outside is the pass' clear, which is what
 *     tells "the triangle was drawn" apart from "the target was cleared and nothing happened".
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
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/backend_vk/internal/ShaderCompiler.hpp>

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
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

constexpr int kTargetSize = 64;

/// @brief The vertex stage the content ABI serves: set 0 binding 0 is the view block, binding 1 the draw block.
constexpr const char8_t* kVertexStage = u8R"(
#version 450
layout(location = 0) in vec3 in_position;

layout(set = 0, binding = 0, std140) uniform VineViewBlock
{
    mat4 view;
    mat4 inv_view;
    mat4 proj;
    mat4 view_proj;
    vec4 cam_pos;
    vec4 frame;
} vn_view;

layout(set = 0, binding = 1, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;

void main()
{
    gl_Position = vn_view.view_proj * vn_draw.model * vec4(in_position, 1.0);
}
)";

/// @brief A fragment stage that writes one colour, so "drawn" and "cleared" are two different pixels.
constexpr const char8_t* kFragmentStage = u8R"(
#version 450
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

/// @brief Builds the two-stage program above (fresh object per call, like every SDK factory).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m2_triangle");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kFragmentStage), String(u8"main") });
    return program;
}

/// @brief Builds a triangle in clip space, indexed, as the one command of its geometry.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.0F, // bottom left
        0.75F,  -0.75F, 0.0F, // bottom right
        0.0F,   0.75F,  0.0F, // top
    };
    std::vector<std::uint32_t> indices{ 0U, 1U, 2U };

    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));

    // Installing positions stated a default non-indexed draw; the triangle is drawn through its index stream, so the
    // default is replaced instead of appended to (see Geometry::addPrimitive).
    Primitive primitive;
    primitive.topology = Topology::Triangles;
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 64x64 RGBA8 + D24 target.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m2_target"));
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

/// @brief A frame with ONE pass that clears @p target and issues @p draw.
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawDescription draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = nullptr;
    entry.order = 0;

    PassDraw pass;
    pass.target        = target;
    pass.clear         = ClearPolicy{};
    pass.clear->color  = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
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

} // namespace

TEST(VkShaderCompilerTest, ATriangleStageCompilesToSpirv)
{
    // THE DEVICE-FREE HALF OF M2. glslang needs no Vulkan object, so "the program's text becomes SPIR-V" is judged
    // here - before any device exists - and the device case below only has to judge the picture.
    const ShaderStage vertex{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") };
    vn::backend_vk::SpirvModule module;
    String                    why;
    ASSERT_TRUE(vn::backend_vk::compileStage(vertex, module, why)) << why.as_std_str();
    EXPECT_FALSE(module.empty());
    // SPIR-V's magic number: 0x07230203, little-endian.
    ASSERT_GE(module.words.size(), 1U);
    EXPECT_EQ(module.words.front(), 0x07230203U);
}

TEST(VkShaderCompilerTest, ABadSourceIsRefusedWithALog)
{
    const ShaderStage broken{ ShaderStageType::Fragment, String(u8"#version 450\nvoid main() { this is not glsl }"), String(u8"main") };
    vn::backend_vk::SpirvModule module;
    String                    why;
    EXPECT_FALSE(vn::backend_vk::compileStage(broken, module, why));
    EXPECT_TRUE(module.empty());
    EXPECT_FALSE(why.empty()) << "a refused stage must say why; a silent refusal is a shader nobody can fix";
}

TEST(VkContentDrawTest, ATriangleIsDrawnIntoTheTargetAndProbed)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target  = makeTarget();
    const intrusive_ptr<ShaderProgram> program = makeProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;

    const PresentOutcome outcome = backend->executeFrame(drawingFrame(target.get(), std::move(draw)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the device presents nothing yet; drawing into a target is not presenting";

    std::vector<std::uint8_t> pixels;
    ReadbackResult            why = ReadbackResult::Failed;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels, &why)) << "readback refused: " << static_cast<int>(why);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    // THE TRIANGLE'S COLOUR, where the triangle is: the clip-space triangle covers the centre of the picture.
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the pixel inside the triangle is not the fragment stage's colour";

    // THE CLEAR COLOUR, where the triangle is not: the top-left corner is outside a triangle whose apex is at y = 0.75.
    EXPECT_EQ(pixelAt(pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the pixel outside the triangle is not the pass' clear";

    // VALIDATION'S VERDICT ON THE WHOLE PATH (compile, upload, pipeline, draw, readback).
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the validation layer reported something while the triangle was drawn and read back";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "the content path skipped a draw it should have recorded";

    backend->shutdown();
}

TEST(VkContentDrawTest, AProgramLessDrawUsesTheDevicesDefaultProgram)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // THE DEFAULT PATH: the command names no program at all, so the device's announcement is what shades it - the
    // shape a rendering host relies on (`RenderDevice::setDefaultContentProgram`).
    const intrusive_ptr<ShaderProgram> program = makeProgram();
    backend->setDefaultContentProgram(program);

    const intrusive_ptr<RenderTarget> target   = makeTarget();
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(RenderCommand(geometry, nullptr, vn::math::Mat4d{}));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;

    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the default content program was not what shaded a program-less command";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);

    backend->shutdown();
}
