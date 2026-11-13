/**
 * @brief M2-M4's acceptance: content draws put a triangle (and its material, texture and lights) on the off-screen
 *        target, and a full-screen draw copies another target's pixels; the probe reads each of them.
 *
 * What this pins, and why each half matters:
 *   - a `DrawDescription` is enough to draw from: the camera snapshot, the command list and the program text cross
 *     the seam as values, so the backend needs nothing from a live scene or a live `ShaderProgram`;
 *   - `ShaderProgramDescription` compiles through glslang into a pipeline, and `GeometryDescription` becomes the
 *     vertex/index streams the pipeline reads - the first half of the test needs no device, the second needs one;
 *   - the pixel INSIDE the triangle is the shader's colour and the pixel outside is the pass' clear, which is what
 *     tells "the triangle was drawn" apart from "the target was cleared and nothing happened";
 *   - M4 adds the three content blocks and the full-screen path: the material's base colour and its base-colour
 *     texture, the ambient light, a screen copy of a target's pixels, and an ordered half-opacity draw (blend).
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
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/backend_vk/internal/ShaderCompiler.hpp>

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
using vn::graphics::LightType;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PassRenderState;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::ReadbackResult;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderCommand;
using vn::graphics::RenderState;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;
using vn::graphics::BlendFactor;
using vn::graphics::BlendState;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The M4 content ABI: the camera pair is the 128-byte push, the per-draw block is set 1 binding 0.
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
    // Reading the draw block pins that set 1 binding 0 is serviced; the default point size is 1.
    gl_PointSize = vn_draw.params.y;
}
)";

/// @brief A fragment stage that writes the material's base colour: set 0 binding 0 is the material block.
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

/// @brief Writes the packed ambient term: binding 6 is the light block.
constexpr const char8_t* kLightFragment = u8R"(
#version 450
layout(set = 0, binding = 6, std140) uniform VineLightsBlock
{
    vec4 ambient;
} vn_lights;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(vn_lights.ambient.rgb * vn_lights.ambient.a, 1.0);
}
)";

/// @brief Writes the material's colour with the drawable's opacity as alpha (the blend case).
constexpr const char8_t* kAlphaFragment = u8R"(
#version 450
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 base_color;
    vec4 emissive;
    vec4 params;
    uvec4 flags;
} vn_material;

layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(vn_material.base_color.rgb, vn_draw.params.x * vn_material.base_color.a);
}
)";

/// @brief Builds the two-stage program above (fresh object per call, like every SDK factory).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgramWith(const char8_t* fragment_source)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m4_program");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_source), String(u8"main") });
    return program;
}

/// @brief Builds the material-base-colour program.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeProgram()
{
    return makeProgramWith(kFragmentStage);
}

/// @brief Builds an opaque material of one colour.
[[nodiscard]] intrusive_ptr<vn::graphics::Material> makeMaterial(const Colorf& color)
{
    auto material = intrusive_ptr<vn::graphics::Material>(new vn::graphics::Material());
    material->setBaseColor(color);
    return material;
}

/// @brief Builds a triangle in clip space, indexed, as the one command of its geometry.
///
/// The z values are 0.5, not 0: the backend renders reverse-Z (the far plane is 0, which is what a depth buffer is
/// cleared to), and the default pass policy TESTS depth - a fragment at the far plane would fail every comparison.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle()
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F, // bottom left
        0.75F,  -0.75F, 0.5F, // bottom right
        0.0F,   0.75F,  0.5F, // top
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

/// @brief A frame with ONE pass that clears NOTHING and issues @p draw (the screen-pass shape).
[[nodiscard]] FrameDescription screenFrame(RenderTarget* target, DrawDescription draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = nullptr;
    entry.order = 0;

    PassDraw pass;
    pass.target = target;
    // A full-screen draw writes its rectangle; the pass is an overlay over whatever the destination already holds.
    pass.state.depth = DepthMode::Disabled;
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
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    command.material            = material;
    command.material_description = material->description();

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
    // The DEFAULT material: its values are the block's defaults (0.8 grey), so a command with no material shades
    // exactly like content with a default one - the property the shared defaults exist for.
    const std::array<int, 4> centre = pixelAt(pixels, kTargetSize / 2, kTargetSize / 2);
    EXPECT_LE(std::abs(centre[0] - 204), 1) << "the default material's base colour was not what shaded the command";
    EXPECT_LE(std::abs(centre[1] - 204), 1);
    EXPECT_LE(std::abs(centre[2] - 204), 1);
    EXPECT_EQ(centre[3], 255);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);

    backend->shutdown();
}

TEST(VkContentDrawTest, ABaseColorTextureIsUploadedAndSampled)
{
    // THE SAMPLING HALF OF M4: a material's base-colour slot is uploaded on the frame's own command buffer and bound
    // at set 0 binding 1. The shader writes what it samples, so a wrong binding/format/layout is a wrong pixel.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kTextureFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    auto                          image    = intrusive_ptr<vn::imaging::Image>(
        new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0]                         = std::byte{ 0U };
    texel[1]                         = std::byte{ 0U };
    texel[2]                         = std::byte{ 255U };
    texel[3]                         = std::byte{ 255U };
    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    material->setTexture(TextureSlot::BaseColor, texture);

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;
    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the base-colour texture was not sampled at set 0 binding 1";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, TheLightBlockCarriesTheAmbientTerm)
{
    // THE LIGHT BLOCK (set 0 binding 6): an announced ambient light fills ambient = rgb + intensity, and the shader
    // writes that product. It also pins the view-space convention: a light nobody placed still gets an ambient fill
    // only when the block is empty, and this announcement replaces it.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kLightFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    LightValue ambient;
    ambient.enabled   = true;
    ambient.type      = LightType::Ambient;
    ambient.color     = Colorf(0.0F, 1.0F, 0.0F, 1.0F);
    ambient.intensity = 1.0F;

    DrawDescription draw;
    draw.kind        = DrawDescription::Kind::Scene;
    draw.camera      = identityCamera();
    draw.commands    = commands;
    draw.light_values = std::vector<LightValue>{ ambient };
    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "the ambient light did not reach the light block";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AScreenCopySamplesTheSourceTarget)
{
    // THE FULL-SCREEN PATH: the fragment stage is a screen program's, the vertex stage is the engine's full-screen
    // triangle, and the source's colour attachments arrive at set 0 bindings 0..N-1. The copy must reproduce the
    // source's inside-triangle colour AND its cleared corner, which is what tells "sampled the target" from
    // "cleared the destination".
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>          source      = makeTarget();
    const intrusive_ptr<ShaderProgram>         scene       = makeProgram();
    const intrusive_ptr<Geometry>              geometry    = makeTriangle();
    const intrusive_ptr<Material>              red         = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<vn::graphics::ShaderProgram> copy = vn::graphics::screenCopyProgram(0);

    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene;
    command.program_description  = scene->description();
    command.material             = red;
    command.material_description = red->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription scene_draw;
    scene_draw.kind     = DrawDescription::Kind::Scene;
    scene_draw.camera   = identityCamera();
    scene_draw.commands = commands;
    backend->executeFrame(drawingFrame(source.get(), std::move(scene_draw)));

    const intrusive_ptr<RenderTarget> destination = makeTarget();

    DrawDescription screen_draw;
    screen_draw.kind                 = DrawDescription::Kind::Screen;
    screen_draw.camera               = identityCamera();
    screen_draw.source               = source.get();
    screen_draw.program              = copy;
    screen_draw.program_description  = copy->description();
    const PresentOutcome outcome     = backend->executeFrame(screenFrame(destination.get(), std::move(screen_draw)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(destination.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the screen copy did not sample the source's drawn pixel";
    EXPECT_EQ(pixelAt(pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 }))
        << "the screen copy did not sample the source's cleared corner";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, ABlendedDrawFollowsTheEngineOrder)
{
    // TRANSPARENCY: the engine decides the order, the backend draws it in that order with the resolved blend. The
    // second (green, half-opacity) draw lands over the first (red), so the centre is the 50/50 mix - which is what
    // "ordered, blended" looks like in the pixels. Depth is off (the usual policy for a translucent pass).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kAlphaFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    const intrusive_ptr<Material> red   = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<Material> green = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    RenderCommand first(geometry, nullptr, vn::math::Mat4d{});
    first.program              = program;
    first.program_description  = program->description();
    first.material             = red;
    first.material_description = red->description();

    RenderCommand second(geometry, nullptr, vn::math::Mat4d{});
    second.program              = program;
    second.program_description  = program->description();
    second.material             = green;
    second.material_description = green->description();
    second.opacity              = 0.5F;
    RenderState authored;
    authored.blend = vn::graphics::BlendState{ true, vn::graphics::BlendFactor::SrcAlpha, vn::graphics::BlendFactor::OneMinusSrcAlpha };
    second.authored_state = authored;

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(first));
    commands->push_back(std::move(second));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    PassRenderState        state;
    state.depth = DepthMode::Disabled;
    PassDraw pass;
    pass.target = target.get();
    pass.clear  = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.state  = state;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    backend->executeFrame(frame);

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    const std::array<int, 4> centre = pixelAt(pixels, kTargetSize / 2, kTargetSize / 2);
    EXPECT_LE(std::abs(centre[0] - 127), 2) << "the destination's red did not survive the half-opacity draw";
    EXPECT_LE(std::abs(centre[1] - 128), 2) << "the blended draw's green did not reach the pixel";
    EXPECT_EQ(centre[2], 0);
    // Alpha is blended with the SAME factors (BlendState states one pair): 0.5 * 0.5 + 1.0 * 0.5 = 0.75.
    EXPECT_LE(std::abs(centre[3] - 191), 2);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}
