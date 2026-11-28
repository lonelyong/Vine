/**
 * @brief M2-M4's acceptance: content draws put a triangle (and its material, texture and lights) on the off-screen
 *        target, and a full-screen draw copies another target's pixels; the probe reads each of them.
 *
 * What this pins, and why each half matters:
 *   - a `DrawPacket` is enough to draw from: the camera snapshot, the command list and the program text cross
 *     the seam as values, so the backend needs nothing from a live scene or a live `ShaderProgram`;
 *   - `ShaderProgramDescription` compiles through glslang into a pipeline, and `GeometryDescription` becomes the
 *     vertex/index streams the pipeline reads - the first half of the test needs no device, the second needs one;
 *   - the pixel INSIDE the triangle is the shader's colour and the pixel outside is the pass' clear, which is what
 *     tells "the triangle was drawn" apart from "the target was cleared and nothing happened";
 *   - M4 adds the three content blocks and the full-screen path: the material's base colour and its base-colour
 *     texture, the ambient light, a full-screen copy of a target's pixels, and an ordered half-opacity draw (blend).
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
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
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/backend_vk/internal/ContentDraw.hpp>
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/backend_vk/internal/ShaderCompiler.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::BlendFactor;
using vn::graphics::BlendState;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::CullMode;
using vn::graphics::DepthPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::Light;
using vn::graphics::LightType;
using vn::graphics::LightValue;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PassRenderState;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderState;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;

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

/// @brief Samples the CENTRE of binding 0 of set 0: the full-screen path binds a source's attachments at 0..N-1.
constexpr const char8_t* kScreenSampleFragment = u8R"(
#version 450
layout(set = 0, binding = 0) uniform sampler2D vn_src;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(vn_src, vec2(0.5, 0.5));
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

/// @brief Writes which side the rasteriser says the fragment is on: red = front, green = back.
constexpr const char8_t* kFacingFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = gl_FrontFacing ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0);
}
)";

/// @brief Writes the VIEW-SPACE normal the engine's own vertex stage produced, as a positive colour.
///
/// The built-in forward / G-buffer vertex stages hand the shading an interpolated view-space normal at location 1;
/// writing its absolute value makes the pixel an exact statement of the direction the stage computed, so a placement
/// whose normal transform is wrong is read as bytes instead of being argued about through a lighting term.
constexpr const char8_t* kNormalProbeFragment = u8R"(
#version 450
layout(location = 1) in vec3 vn_view_normal;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(abs(normalize(vn_view_normal)), 1.0);
}
)";

/// @brief Writes nothing: a compute entry point (this backend stages no compute program, see ensureProgram).
constexpr const char8_t* kComputeStage = u8R"(
#version 450
layout(local_size_x = 1) in;

void main()
{
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

/// @brief Builds a program that declares ONE fragment stage: a whole full-screen program and no content program.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeFragmentOnlyProgram(const char8_t* fragment_source)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_fragment_only_program");
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(fragment_source), String(u8"main") });
    return program;
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
///
/// @param clockwise Author the triangle with its winding reversed, i.e. as the geometry's own BACK (see CullMode).
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(bool clockwise = false)
{
    std::vector<float> positions{
        -0.75F, -0.75F, 0.5F, // bottom left
        0.75F,  -0.75F, 0.5F, // bottom right
        0.0F,   0.75F,  0.5F, // top
    };
    std::vector<std::uint32_t> indices = clockwise ? std::vector<std::uint32_t>{ 0U, 2U, 1U } : std::vector<std::uint32_t>{ 0U, 1U, 2U };

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

/// @brief Builds a quad that fills the clip-space picture and is SYMMETRIC about x = 0, with per-vertex normals.
///
/// The symmetry is what makes a mirror witness possible: the placement `scale(-1, 1, 1)` maps the quad's covered
/// pixels onto themselves, so both placements can be sampled at the SAME pixel.
///
/// @param normals_away Author the normals as `-z` instead of `+z`, i.e. as a surface whose far side faces the light.
[[nodiscard]] intrusive_ptr<Geometry> makeFillingQuad(bool normals_away = false)
{
    const float nz       = normals_away ? -1.0F : 1.0F;
    auto        geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
      -1.0F,
      -1.0F,
      0.5F,
      1.0F,
      -1.0F,
      0.5F,
      1.0F,
      1.0F,
      0.5F,
      -1.0F,
      -1.0F,
      0.5F,
      1.0F,
      1.0F,
      0.5F,
      -1.0F,
      1.0F,
      0.5F,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
      0.0F,
      0.0F,
      nz,
      0.0F,
      0.0F,
      nz,
      0.0F,
      0.0F,
      nz,
      0.0F,
      0.0F,
      nz,
      0.0F,
      0.0F,
      nz,
      0.0F,
      0.0F,
      nz,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief The filling quad with an AUTHORED normal of its own (the geometry is only the coverage fixture).
///
/// The authored normal is deliberately independent of the quad's plane: what a vertex stage transforms is the
/// attribute, and a direction with a component on every axis a placement can scale separately is what makes a
/// non-uniform scale visible in the pixel.
///
/// @param nx X component of the authored normal.
/// @param ny Y component of the authored normal.
/// @param nz Z component of the authored normal.
[[nodiscard]] intrusive_ptr<Geometry> makeFillingQuadWithNormal(float nx, float ny, float nz)
{
    auto geometry = makeFillingQuad(false);
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(
        std::vector<float>{ nx, ny, nz, nx, ny, nz, nx, ny, nz, nx, ny, nz, nx, ny, nz, nx, ny, nz })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief Builds a program from the ENGINE'S OWN forward vertex stage and the normal probe fragment.
///
/// The stage is taken from the shipped program rather than from a copy of its text: what this program draws with is
/// byte-for-byte the vertex stage `forwardProgram()` ships, so a change there cannot leave the probe behind.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeNormalProbeProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_normal_probe_program");
    // The engine program is held for the whole walk: `stages()` is a view into it, and a temporary returned by
    // forwardProgram() would be gone before the first stage is copied.
    const intrusive_ptr<ShaderProgram> engine = vn::graphics::forwardProgram();
    for (const ShaderStage& stage : engine->stages()) {
        if (stage.type == ShaderStageType::Vertex) {
            program->addStage(stage);
        }
    }
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kNormalProbeFragment), String(u8"main") });
    return program;
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
[[nodiscard]] FrameDescription drawingFrame(RenderTarget* target, DrawPacket draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target        = target;
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);

    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief A frame with ONE pass that clears NOTHING and issues @p draw (the full-screen-pass shape).
[[nodiscard]] FrameDescription fullscreenFrame(RenderTarget* target, DrawPacket draw)
{
    FrameDescription frame;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target = target;
    // A full-screen draw writes its rectangle; the pass is an overlay over whatever the destination already holds.
    pass.state.depth = DepthPolicy::Disabled;
    pass.packets.push_back(std::move(draw));
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

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();
    command.material            = material;
    command.material_description = material->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    const PresentOutcome outcome = backend->executeFrame(drawingFrame(target.get(), std::move(draw)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the device presents nothing yet; drawing into a target is not presenting";

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok()) << "readback refused: " << static_cast<int>(read.status);
    const std::vector<std::uint8_t>& pixels = read.values;
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

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(DrawItem(geometry, nullptr, vn::math::Mat4d{}));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    const std::vector<std::uint8_t>& pixels = read.values;
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

TEST(VkContentRefusalTest, TheFirstReasonWinsAndOnlyACodeMakesARefusal)
{
    // THE VALUE THAT CARRIES A CAUSE (see ContentRefusal): a recording path meets the first failure that explains the
    // draw and must not let a later, consequential one overwrite it - the rule the recorder used to spell out with
    // `if (why.empty())` at every call site, now in one place - and "this draw was refused" and "this refusal has a
    // code" are the same question, so a sentence alone cannot be reported as a cause.
    vn::backend_vk::ContentRefusal refusal;
    EXPECT_FALSE(refusal.isRefused()) << "a fresh refusal is not a refusal";
    EXPECT_EQ(refusal.code(), nullptr) << "nothing was refused, so no cause is named";

    refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped, u8"vk.content.no_geometry", String(u8"first"));
    ASSERT_TRUE(refusal.isRefused());
    EXPECT_TRUE(std::u8string_view(refusal.code()) == u8"vk.content.no_geometry");
    EXPECT_EQ(refusal.category(), vn::graphics::DiagnosticCategory::ContentSkipped);

    // THE SECOND REASON IS A CONSEQUENCE, not the cause: a draw with no geometry also has no pipeline, and reporting the
    // pipeline would send a host to the wrong place.
    refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed, u8"vk.content.pipeline_refused", String(u8"second"));
    EXPECT_TRUE(std::u8string_view(refusal.code()) == u8"vk.content.no_geometry") << "a later failure replaced the cause";
    EXPECT_EQ(refusal.category(), vn::graphics::DiagnosticCategory::ContentSkipped) << "the category moved with it";
    EXPECT_TRUE(refusal.message().std_u8str_view() == u8"first") << "the sentence moved with it";

    // A CODE WITH NO SENTENCE IS STILL A REFUSAL (some causes need no prose), and clearing makes the value reusable -
    // which is what `record` does on entry, so a caller may hand the same value back for another draw.
    refusal.clear();
    EXPECT_FALSE(refusal.isRefused());
    EXPECT_EQ(refusal.code(), nullptr);
    EXPECT_TRUE(refusal.message().empty());
    refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped, u8"vk.content.nothing_to_draw", String{});
    EXPECT_TRUE(refusal.isRefused()) << "a code alone is a refusal: the sentence is optional, the cause is not";
}

TEST(VkContentRefusalTest, EveryCauseNamesAStableCodeAndTheDraftIsTheSharedShape)
{
    // A CAUSE IS AN ENUMERATOR, NOT A STRING (D-22, see ContentRefusalCause): every value must answer a
    // non-null `vk.content.*` code, and no two causes may share one - which is the runtime half of the
    // switch that has no `default` (a cause added without a case does not compile clean).
    std::vector<std::u8string_view> codes;
    for (std::uint32_t value = 0; value < static_cast<std::uint32_t>(vn::backend_vk::ContentRefusalCause::Count); ++value) {
        const auto           cause = static_cast<vn::backend_vk::ContentRefusalCause>(value);
        const char8_t* const code  = vn::backend_vk::contentRefusalCode(cause);
        ASSERT_NE(code, nullptr) << "cause " << value << " names no code";
        codes.emplace_back(code);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(std::adjacent_find(codes.begin(), codes.end()), codes.end()) << "two causes share one code: a host cannot tell them apart";

    // ONE SHAPE FOR EVERY BACKEND (see DiagnosticDraft): the draft carries the object and the limit the
    // SDK's value has, and `diagnostic()` hands all of it on under the severity the call site states.
    vn::backend_vk::ContentRefusal draft;
    draft.refuse(vn::graphics::DiagnosticCategory::TargetBuildFailed,
                 vn::backend_vk::contentRefusalCode(vn::backend_vk::ContentRefusalCause::NoProgram),
                 String(u8"GBuffer"),
                 vn::graphics::DiagnosticLimit{ 8, 4 },
                 String(u8"no program"));
    const vn::graphics::RenderDiagnostic diagnostic = draft.diagnostic(vn::graphics::DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.severity, vn::graphics::DiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.category, vn::graphics::DiagnosticCategory::TargetBuildFailed);
    EXPECT_TRUE(diagnostic.code.std_u8str_view() == u8"vk.content.no_program");
    EXPECT_TRUE(diagnostic.subject.std_u8str_view() == u8"GBuffer");
    ASSERT_TRUE(diagnostic.limit.has_value());
    EXPECT_EQ(diagnostic.limit->requested, 8U);
    EXPECT_EQ(diagnostic.limit->supported, 4U);
    EXPECT_TRUE(diagnostic.message.std_u8str_view() == u8"no program");
    EXPECT_EQ(diagnostic.origin, vn::graphics::DiagnosticOrigin::Device) << "a backend value is a device report";
}

TEST(VkContentDrawTest, ADrawWithNoProgramArrivesWithTheCodeOfItsCause)
{
    // A HOST ACTS ON THE CODE (audit A3): "a draw was skipped" covers causes with different repairs - no program, no
    // geometry, a primitive the geometry lacks, a pipeline the driver refused - and the code is what tells them apart
    // without parsing the sentence. This is the deterministic one: a command that names no program, on a device that was
    // never given a default one.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget> target   = makeTarget();
    const intrusive_ptr<Geometry>     geometry = makeTriangle();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(DrawItem(geometry, nullptr, vn::math::Mat4d{})); // no program, and none announced

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    (void)backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    bool saw_refusal = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        if (diagnostic.code.std_u8str_view() != u8"vk.content.no_program") {
            continue;
        }
        saw_refusal = true;
        EXPECT_EQ(diagnostic.category, vn::graphics::DiagnosticCategory::ContentSkipped)
            << "a missing program is skipped content, not a compile failure";
        EXPECT_NE(diagnostic.message.std_u8str_view().find(u8"program"), std::u8string_view::npos)
            << "the sentence must still say what a human needs";
    }
    EXPECT_TRUE(saw_refusal) << "the skipped draw did not arrive with the code of its cause";

    backend->shutdown();
}

TEST(VkContentDrawTest, AProgramWithNoVertexStageIsRefusedAsAProgramFault)
{
    // A PROGRAM IS EDITABLE TO ANY STAGE LIST (clearStages, replaceStages, a fragment-only file), so "is it drawable" is
    // a question asked where the program is USED, never a verdict cached on the program. A content draw states a vertex
    // and a fragment stage; a program that resolved no vertex module has no pipeline to give it, and the refusal has to
    // name the PROGRAM - reporting the blend/depth state (the pipeline code) sends a host to the wrong place, and handing
    // VK_NULL_HANDLE to the driver is not a refusal at all (review 19 Z16).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    const intrusive_ptr<RenderTarget>  target        = makeTarget();
    const intrusive_ptr<Geometry>      geometry      = makeTriangle();
    const intrusive_ptr<ShaderProgram> fragment_only = makeFragmentOnlyProgram(kFragmentStage);

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = fragment_only;
    command.program_description = fragment_only->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind        = DrawKind::Scene;
    draw.view.camera = identityCamera();
    draw.commands    = commands;
    (void)backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    bool saw_missing_stage = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        EXPECT_TRUE(diagnostic.code.std_u8str_view() != u8"vk.content.pipeline_refused")
            << "a program's missing stage was reported as a state the driver refused";
        if (diagnostic.code.std_u8str_view() != u8"vk.content.program_stage_missing") {
            continue;
        }
        saw_missing_stage = true;
        EXPECT_EQ(diagnostic.category, vn::graphics::DiagnosticCategory::CompileFailed) << "an incomplete program is a compile-side fault, not skipped content";
        EXPECT_NE(diagnostic.message.std_u8str_view().find(u8"vertex"), std::u8string_view::npos) << "the sentence must name the stage that is absent";
    }
    EXPECT_TRUE(saw_missing_stage) << "the draw with a vertex-less program was not refused with its own code";

    // NOTHING WAS DRAWN: the destination keeps the pass' clear colour, which is what "the draw was skipped" means.
    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 0, 255 }))
        << "a draw whose program has no vertex stage drew something anyway";

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

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;
    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the base-colour texture was not sampled at set 0 binding 1";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, ASamplerReadsTheMipLevelItsLodClampNames)
{
    // THE SAMPLER'S MIP STATE REACHES THE DEVICE: the SDK states `min_filter` and `mipmap_mode` as two axes
    // (WebGPU's minFilter/mipmapFilter), and Vulkan composes them the same way - `minFilter` filters WITHIN a level
    // and `mipmapMode` BETWEEN two (the spec's OpenGL-mapping note pairs VK_FILTER_LINEAR + MIPMAP_MODE_NEAREST with
    // GL_LINEAR_MIPMAP_NEAREST) - while `min_lod` / `max_lod` / `lod_bias` decide WHICH level is read; Vulkan always
    // mip-selects. Nothing pinned that those fields cross `samplerFor`: a mapping that dropped them would sample
    // another level and every picture would still look right. The clamps force the level here, so the pixel cannot
    // come from a derivative accident: level 1 is green, the base level red.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kTextureFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(2, 2, vn::imaging::PixelFormat::Rgba8Unorm, 2));
    const std::span<std::byte> base = image->mipData(0);
    for (std::size_t texel = 0; texel < base.size(); texel += 4U) {
        base[texel + 0U] = std::byte{ 255U };
        base[texel + 1U] = std::byte{ 0U };
        base[texel + 2U] = std::byte{ 0U };
        base[texel + 3U] = std::byte{ 255U };
    }
    const std::span<std::byte> level_one = image->mipData(1); // 1 x 1
    level_one[0]                          = std::byte{ 0U };
    level_one[1]                          = std::byte{ 255U };
    level_one[2]                          = std::byte{ 0U };
    level_one[3]                          = std::byte{ 255U };

    auto texture = intrusive_ptr<Texture2D>(new Texture2D(2, 2, vn::imaging::PixelFormat::Rgba8Unorm, 2));
    texture->setImage(image);

    const auto frameWith = [&](const vn::graphics::Sampler& sampler) {
        const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
        material->setTexture(TextureSlot::BaseColor, texture, sampler);

        DrawItem command(geometry, nullptr, vn::math::Mat4d{});
        command.program              = program;
        command.program_description  = program->description();
        command.material             = material;
        command.material_description = material->description();

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(command));

        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = commands;
        return drawingFrame(target.get(), std::move(draw));
    };

    // LEVEL 1: both clamps at 1, so every lookup reads the green level.
    vn::graphics::Sampler level_1;
    level_1.min_lod = 1.0F;
    level_1.max_lod = 1.0F;
    ASSERT_EQ(backend->executeFrame(frameWith(level_1), nullptr), PresentOutcome::Skipped);
    const ColorReadback one = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(one.ok());
    EXPECT_EQ(pixelAt(one.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "the LOD clamp named level 1 and the base level was sampled: the sampler's LOD range never reached the device";

    // THE CONTROL: the same chain, the same texture, the clamps at 0 - a sampler that pins the base level must read
    // it, so "always level 1" would fail here. (The spec states the apartness of that clamp too: maxLod 0 makes every
    // lookup a magnification, so the MAG filter is what it applies - the colour read is the base level's either way.)
    vn::graphics::Sampler level_0;
    level_0.min_lod = 0.0F;
    level_0.max_lod = 0.0F;
    ASSERT_EQ(backend->executeFrame(frameWith(level_0), nullptr), PresentOutcome::Skipped);
    const ColorReadback zero = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(zero.ok());
    EXPECT_EQ(pixelAt(zero.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "a sampler pinned to level 0 read another level";

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

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = program;
    command.program_description = program->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    LightValue ambient;
    ambient.enabled   = true;
    ambient.type      = LightType::Ambient;
    ambient.color     = Colorf(0.0F, 1.0F, 0.0F, 1.0F);
    ambient.intensity = 1.0F;

    DrawPacket draw;
    draw.kind        = DrawKind::Scene;
    draw.view.camera      = identityCamera();
    draw.commands    = commands;
    draw.light_values = vn::graphics::LightSet::create({ ambient });
    backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 255, 0, 255 }))
        << "the ambient light did not reach the light block";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AFullscreenCopySamplesTheSourceTarget)
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
    const intrusive_ptr<vn::graphics::ShaderProgram> copy = vn::graphics::fullscreenCopyProgram(0);

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene;
    command.program_description  = scene->description();
    command.material             = red;
    command.material_description = red->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket scene_draw;
    scene_draw.kind     = DrawKind::Scene;
    scene_draw.view.camera   = identityCamera();
    scene_draw.commands = commands;
    backend->executeFrame(drawingFrame(source.get(), std::move(scene_draw)));

    const intrusive_ptr<RenderTarget> destination = makeTarget();

    DrawPacket fullscreen_draw;
    fullscreen_draw.kind                 = DrawKind::Fullscreen;
    fullscreen_draw.view.camera               = identityCamera();
    fullscreen_draw.source               = source.get();
    fullscreen_draw.program              = copy;
    fullscreen_draw.program_description  = copy->description();
    const PresentOutcome outcome              = backend->executeFrame(fullscreenFrame(destination.get(), std::move(fullscreen_draw)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped);

    const ColorReadback read = backend->readColorImage(destination.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the full-screen copy did not sample the source's drawn pixel";
    EXPECT_EQ(pixelAt(read.values, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the full-screen copy did not sample the source's cleared corner";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AComputeStageIsRefusedRatherThanDropped)
{
    // NO STAGE IS DROPPED SILENTLY. This backend stages no compute program, and the loop used to `continue` past the
    // stage, so an entry that resolved NOTHING was cached as compiled and the draw was reported as a missing GRAPHICS
    // stage - which is not what is wrong with it (review 19 Z16). The refusal names the program's own cause, and it is
    // remembered like every other refusal, so the same program is reported once.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    std::vector<vn::graphics::RenderDiagnostic> reported;
    backend->setDiagnosticSink([&reported](const vn::graphics::RenderDiagnostic& diagnostic) { reported.push_back(diagnostic); });

    auto compute_only = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    compute_only->setName(u8"vk_compute_only_program");
    compute_only->addStage(ShaderStage{ ShaderStageType::Compute, String(kComputeStage), String(u8"main") });

    const intrusive_ptr<RenderTarget> target   = makeTarget();
    const intrusive_ptr<Geometry>     geometry = makeTriangle();

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program             = compute_only;
    command.program_description = compute_only->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind        = DrawKind::Scene;
    draw.view.camera = identityCamera();
    draw.commands    = commands;
    (void)backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

    bool saw_refusal = false;
    for (const vn::graphics::RenderDiagnostic& diagnostic : reported) {
        if (diagnostic.code.std_u8str_view() != u8"vk.content.program_refused") {
            continue;
        }
        saw_refusal = true;
        EXPECT_NE(diagnostic.message.std_u8str_view().find(u8"compute"), std::u8string_view::npos)
            << "the sentence must say that compute is what this backend does not stage";
    }
    EXPECT_TRUE(saw_refusal) << "a compute-only program was dropped silently instead of refused";

    backend->shutdown();
}

TEST(VkContentDrawTest, AFragmentOnlyProgramIsAWholeFullScreenProgram)
{
    // THE OTHER SIDE OF THE SAME RULE (review 19 Z16): a full-screen draw states its own vertex stage (the engine's
    // canonical triangle), so a program with ONE fragment stage is COMPLETE for it. A rule written where the program is
    // COMPILED - "an entry with no vertex module is failed" - would refuse this draw for ever: the program is not broken,
    // the content use of it was. The requirement therefore belongs to the pipeline that states the stages.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>                source        = makeTarget();
    const intrusive_ptr<ShaderProgram>               scene         = makeProgram();
    const intrusive_ptr<Geometry>                    geometry      = makeTriangle();
    const intrusive_ptr<Material>                    red           = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<vn::graphics::ShaderProgram> fragment_only = makeFragmentOnlyProgram(kScreenSampleFragment);

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene;
    command.program_description  = scene->description();
    command.material             = red;
    command.material_description = red->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket scene_draw;
    scene_draw.kind        = DrawKind::Scene;
    scene_draw.view.camera = identityCamera();
    scene_draw.commands    = commands;
    backend->executeFrame(drawingFrame(source.get(), std::move(scene_draw)));

    const intrusive_ptr<RenderTarget> destination = makeTarget();

    DrawPacket fullscreen_draw;
    fullscreen_draw.kind                = DrawKind::Fullscreen;
    fullscreen_draw.view.camera         = identityCamera();
    fullscreen_draw.source              = source.get();
    fullscreen_draw.program             = fragment_only;
    fullscreen_draw.program_description = fragment_only->description();
    const PresentOutcome outcome        = backend->executeFrame(fullscreenFrame(destination.get(), std::move(fullscreen_draw)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped);

    const ColorReadback read = backend->readColorImage(destination.get(), 0);
    ASSERT_TRUE(read.ok());
    // THE ONE TEXEL THIS STAGE SAMPLES (uv 0.5, 0.5) lands on every pixel, so one probe proves the pass drew: the
    // destination was never named by a scene packet, so red there is the fragment-only program's own work.
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the fragment-only program was refused for a draw that states its own vertex stage";
    EXPECT_EQ(pixelAt(read.values, 2, 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the fragment-only program's full-screen pass did not cover the whole picture";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AFullscreenCopyIsDrawnWhateverThePassDepthPolicySays)
{
    // THE PASS' DEPTH POLICY IS ABOUT ITS CONTENT, NOT ABOUT THE FULL-SCREEN CALL. A full-screen draw is the
    // canonical triangle at z = 0.0, which under this engine's reverse-Z convention is the FAR plane; a full-screen packet
    // that inherited the pass' TestAndWrite policy against a destination that carries depth would be discarded at
    // every pixel. The app found this the hard way (2026-11-14): a window pass' render pass clears D24 to 0 and the
    // deferred chain's composite BORROWS the G-buffer's depth, so present and the G-buffer previews "recorded and
    // drew nothing" while the HUD content (DepthPolicy::Disabled) stayed visible.
    //
    // DEFAULT PASS STATE ON PURPOSE (see fullscreenFrame, which states Disabled explicitly): the shape under test is the
    // one the engine's own chain passes hand over.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>                source   = makeTarget();
    const intrusive_ptr<ShaderProgram>               scene    = makeProgram();
    const intrusive_ptr<Geometry>                    geometry = makeTriangle();
    const intrusive_ptr<Material>                    red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<vn::graphics::ShaderProgram> copy     = vn::graphics::fullscreenCopyProgram(0);

    DrawItem command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene;
    command.program_description  = scene->description();
    command.material             = red;
    command.material_description = red->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket scene_draw;
    scene_draw.kind     = DrawKind::Scene;
    scene_draw.view.camera   = identityCamera();
    scene_draw.commands = commands;
    backend->executeFrame(drawingFrame(source.get(), std::move(scene_draw)));

    // The destination carries depth (makeTarget attaches D24) and is never drawn into before: its depth is the
    // render pass' clear (far, 0.0), which is exactly what a TestAndWrite full-screen packet fails against.
    const intrusive_ptr<RenderTarget> destination = makeTarget();

    DrawPacket fullscreen_draw;
    fullscreen_draw.kind                = DrawKind::Fullscreen;
    fullscreen_draw.view.camera              = identityCamera();
    fullscreen_draw.source              = source.get();
    fullscreen_draw.program             = copy;
    fullscreen_draw.program_description = copy->description();

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target = destination.get();
    pass.packets.push_back(std::move(fullscreen_draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    EXPECT_EQ(backend->executeFrame(frame), PresentOutcome::Skipped);

    const ColorReadback read = backend->readColorImage(destination.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the full-screen packet was discarded by a depth test it does not have";
    EXPECT_EQ(pixelAt(read.values, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the full-screen copy did not sample the source's cleared corner";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, TheFragmentStageSeesTheSdksFrontFace)
{
    // THE RULE TWO-SIDED SHADING AND CULLING BOTH STAND ON (review 17 W3, review 18 Y1): `gl_FrontFacing` must mean the
    // SDK's counter-clockwise rule - for EVERY placement. The backend flips Y in the projection, so the winding Vulkan
    // judges is not the winding the SDK wrote down, and a placement with a negative determinant flips it again; a fixed
    // front-face constant would get the second case wrong, and this case states both halves in pixels instead of an
    // argument about a flipped coordinate system.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kFacingFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    // The SAME triangle as its geometry's own back: reversed indices, so the authored back is what faces the camera.
    const intrusive_ptr<Geometry> authored_back = makeTriangle(true);

    const auto render = [&](const intrusive_ptr<Geometry>& shape, const vn::math::Mat4d& model, const RenderState& authored) {
        DrawItem draw_item(shape, nullptr, model);
        draw_item.program              = program;
        draw_item.program_description  = program->description();
        draw_item.authored_state       = authored;

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(draw_item));

        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = commands;

        FrameDescription frame;
        FrameDescription::Pass entry;
        entry.pass = vn::test::framePassForTest();
        PassDraw pass;
        pass.target                   = target.get();
        pass.attachment_policy        = AttachmentPolicy{};
        pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        pass.packets.push_back(std::move(draw));
        entry.draw = std::move(pass);
        frame.passes.push_back(std::move(entry));
        (void)backend->executeFrame(frame);

        const ColorReadback read = backend->readColorImage(target.get(), 0);
        EXPECT_TRUE(read.ok());
        return pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    };

    // The triangle winds counter-clockwise in the picture (bottom left -> bottom right -> top), so the SDK calls the
    // side the identity camera sees a FRONT face.
    const std::array<int, 4> front = render(geometry, vn::math::Mat4d{}, RenderState{});
    EXPECT_EQ(front, (std::array<int, 4>{ 255, 0, 0, 255 })) << "the SDK's front face reached the stage as a front face";

    // THE PLACEMENT'S HANDEDNESS IS PART OF THE DRAW (DrawItem::reversesWinding): a mirror reverses the winding, and a
    // backend that kept one fixed front face for it made every correctly authored surface a back face - which is what
    // turned a mirrored solid's lit side into its far side. The authored front is still the front here.
    const auto mirrored = vn::math::scale<double>(-1.0, 1.0, 1.0);
    EXPECT_EQ(render(geometry, mirrored, RenderState{}), front) << "a mirrored placement is still the SDK's front face";

    // The other value of the pair still works: an AUTHORED back is a back, mirrored or not (the two flips cancel).
    const std::array<int, 4> back = render(authored_back, vn::math::Mat4d{}, RenderState{});
    EXPECT_EQ(back, (std::array<int, 4>{ 0, 255, 0, 255 })) << "the geometry's own back is a back face";
    EXPECT_EQ(render(authored_back, mirrored, RenderState{}), back) << "a mirrored authored back is still a back face";

    // CULLING FOLLOWS THE SAME WORD, BOTH WAYS ROUND: `Back` keeps the authored front (whatever the placement did to
    // the winding) and drops the authored back; `Front` does the opposite. A mirrored solid with `Back` therefore shows
    // its OUTSIDE - the inside is exactly what a fixed-front-face backend used to leave visible.
    RenderState cull_back;
    cull_back.cullMode = CullMode::Back;
    EXPECT_EQ(render(geometry, vn::math::Mat4d{}, cull_back), front) << "CullMode::Back keeps the SDK's front face";
    EXPECT_EQ(render(geometry, mirrored, cull_back), front) << "CullMode::Back keeps a mirrored placement's front too";
    EXPECT_EQ(render(authored_back, vn::math::Mat4d{}, cull_back), (std::array<int, 4>{ 0, 0, 0, 255 })) << "CullMode::Back drops the geometry's own back";

    RenderState cull_front;
    cull_front.cullMode = CullMode::Front;
    EXPECT_EQ(render(geometry, vn::math::Mat4d{}, cull_front), (std::array<int, 4>{ 0, 0, 0, 255 })) << "CullMode::Front drops the SDK's front face";
    EXPECT_EQ(render(geometry, mirrored, cull_front), (std::array<int, 4>{ 0, 0, 0, 255 })) << "CullMode::Front drops a mirrored placement's front face too";
    EXPECT_EQ(render(authored_back, vn::math::Mat4d{}, cull_front), back) << "CullMode::Front keeps the geometry's back";
}

TEST(VkContentDrawTest, AMirroredPlacementIsLitAsItsUnmirroredTwin)
{
    // Y1 (review 18): two-sided shading turns a BACK face's normal toward the viewer (`vnFacingNormal`), which is right
    // only while "front" means the AUTHORED front. A mirror reverses the winding and leaves the authored normal pointing
    // outward (a reflection is orthogonal), so with a fixed front face every visible face of a mirrored solid was
    // shaded as the solid's FAR side - the ambient term only, with culling off.
    //
    // Three placements of one white quad say it in pixels: the lit one, its mirror image over x = 0 (equal - the quad
    // is symmetric, so the same pixel is covered by both), and a quad authored with its normals away from the light
    // (darker, so the equality cannot pass as two unlit frames agreeing).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target  = makeTarget();
    const intrusive_ptr<ShaderProgram> program = vn::graphics::forwardProgram();
    const intrusive_ptr<Material>      white   = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    const intrusive_ptr<Geometry>      facing  = makeFillingQuad(false);
    const intrusive_ptr<Geometry>      away    = makeFillingQuad(true);

    // A light that travels along -z, so a surface facing +z is fully lit (`vnEvaluateLight` negates the direction).
    auto light = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    light->setColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    light->setIntensity(1.0F);
    std::vector<LightValue>         lights{ light->value() };
    const vn::graphics::LightSetPtr light_set = vn::graphics::LightSet::create(std::move(lights));

    const auto render = [&](const intrusive_ptr<Geometry>& shape, const vn::math::Mat4d& model) {
        DrawItem draw_item(shape, white, model);
        draw_item.program              = program;
        draw_item.program_description  = program->description();
        draw_item.material             = white;
        draw_item.material_description = white->description();

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(draw_item));

        DrawPacket draw;
        draw.kind         = DrawKind::Scene;
        draw.view.camera  = identityCamera();
        draw.commands     = commands;
        draw.light_values = light_set;

        FrameDescription       frame;
        FrameDescription::Pass entry;
        entry.pass = vn::test::framePassForTest();
        PassDraw pass;
        pass.target                   = target.get();
        pass.attachment_policy        = AttachmentPolicy{};
        pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        pass.packets.push_back(std::move(draw));
        entry.draw = std::move(pass);
        frame.passes.push_back(std::move(entry));
        (void)backend->executeFrame(frame);

        const ColorReadback read = backend->readColorImage(target.get(), 0);
        EXPECT_TRUE(read.ok());
        return pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    };

    const std::array<int, 4> lit      = render(facing, vn::math::Mat4d{});
    const std::array<int, 4> mirrored = render(facing, vn::math::scale<double>(-1.0, 1.0, 1.0));
    const std::array<int, 4> unlit    = render(away, vn::math::Mat4d{});

    EXPECT_EQ(mirrored, lit) << "a mirrored placement is shaded as its unmirrored twin, not from behind";
    EXPECT_LT(unlit[1], lit[1]) << "the fixture can tell lit from unlit: a normal turned away from the light is darker";
    EXPECT_GT(lit[1], 0) << "the lit case is not the cleared target either";
}

TEST(VkContentDrawTest, ANonUniformlyScaledDrawTransformsItsNormalByTheInverseTranspose)
{
    // REVIEW 11 V2 / REVIEW 18 Y8: both built-in vertex stages handed the shading `mat3(model_view) * normal`, which
    // is the normal transform only while the placement is rigid or UNIFORMLY scaled. A scale VECTOR is legal
    // (PositionAttitudeTransform states one, and a shear is what a non-uniform parent scale does to a rotated child),
    // and an instance matrix can carry one without any node stating it - so a legal draw was lit from a direction its
    // surface does not have, with nothing to report it.
    //
    // THE PIXEL IS THE STAGE'S OWN ANSWER: the ENGINE'S forward vertex stage drives a fragment that writes
    // `abs(normalize(vn_view_normal))`, so the readback states the direction that stage computed, in bytes, with no
    // lighting term in the way.
    //
    // ONE authored normal (0, 1/sqrt2, 1/sqrt2), three placements:
    //   - stretched on y only                      -> the inverse transpose turns it toward +z: (0, 16, 254)
    //   - the same stretch behind a 30-degree turn  -> still toward +z, mixed:           (0, 113, 228)
    //   - the turn alone (rigid)                   -> unchanged by the rule:             (0, 66, 246)
    // The middle placement is what separates the inverse TRANSPOSE from a plain inverse (which answers (0, 58, 248));
    // `mat3(model_view)` - today's spelling - answers (0, 212, 141) on it.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeNormalProbeProgram();
    const intrusive_ptr<Geometry>      geometry = makeFillingQuadWithNormal(0.0F, 0.7071068F, 0.7071068F);

    const auto render = [&](const vn::math::Mat4d& model) {
        DrawItem draw_item(geometry, nullptr, model);
        draw_item.program             = program;
        draw_item.program_description = program->description();

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(draw_item));

        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = commands;
        backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

        const ColorReadback read = backend->readColorImage(target.get(), 0);
        EXPECT_TRUE(read.ok());
        return pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    };

    const vn::math::Mat4d stretched      = vn::math::scale<double>(1.0, 16.0, 1.0);
    const vn::math::Mat4d turned         = vn::math::rotate(vn::math::Vec3d(1.0, 0.0, 0.0), 0.5235987755982988);
    const std::array<int, 4> on_y        = render(stretched);
    const std::array<int, 4> turned_bent = render(turned * stretched);
    const std::array<int, 4> rigid       = render(turned);

    EXPECT_NEAR(on_y[1], 16, 2) << "the y-only stretch left the normal where the raw 3x3 put it (a normal must turn "
                                   "toward the stretched axis' inverse)";
    EXPECT_NEAR(on_y[2], 254, 2) << "the inverse-transposed normal is almost +z for a 16x y stretch";
    EXPECT_NEAR(turned_bent[1], 113, 2) << "a rotation behind the stretch must compose with the transpose, not cancel "
                                           "it (the plain-inverse spelling answers ~58 here)";
    EXPECT_NEAR(turned_bent[2], 228, 2) << "the composed direction is the inverse transpose of turn * stretch";
    EXPECT_NEAR(rigid[1], 66, 2) << "a rigid placement was already right and the fix must not move it";
    EXPECT_NEAR(rigid[2], 246, 2) << "a rigid placement's normal is the authored one turned by the rotation alone";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U);
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

    DrawItem first(geometry, nullptr, vn::math::Mat4d{});
    first.program              = program;
    first.program_description  = program->description();
    first.material             = red;
    first.material_description = red->description();

    DrawItem second(geometry, nullptr, vn::math::Mat4d{});
    second.program              = program;
    second.program_description  = program->description();
    second.material             = green;
    second.material_description = green->description();
    // OPACITY ALONE BLENDS (review 17 W2): the command authors NO blend state and no StateNode stands over it, so
    // the resolved blend can only come from the transparency class the sort uses - the documented rule this case
    // exists to pin. The half-opacity draw used to carry an explicit BlendState here, which is how the rule's
    // absence survived a pixel test.
    second.opacity = 0.5F;

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(first));
    commands->push_back(std::move(second));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassRenderState        state;
    state.depth = DepthPolicy::Disabled;
    PassDraw pass;
    pass.target = target.get();
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.state  = state;
    pass.packets.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    backend->executeFrame(frame);

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok());
    const std::array<int, 4> centre = pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    EXPECT_LE(std::abs(centre[0] - 127), 2) << "the destination's red did not survive the half-opacity draw";
    EXPECT_LE(std::abs(centre[1] - 128), 2) << "the blended draw's green did not reach the pixel";
    EXPECT_EQ(centre[2], 0);
    // Alpha is blended with the SAME factors (BlendState states one pair): 0.5 * 0.5 + 1.0 * 0.5 = 0.75.
    EXPECT_LE(std::abs(centre[3] - 191), 2);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AStateChangeIsACommandNotAPipeline)
{
    // `T54`: cull / polygon / depth / blend are DYNAMIC state, so ONE compiled pipeline serves both pictures and the
    // second frame issues commands instead of compiling a variant. The pixel is what proves the command ran: with
    // blend the second draw mixes over the first (127 / 128 / 191), without it the second draw REPLACES it
    // (0 / 255 / 128, the fragment's own output).
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kAlphaFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    const intrusive_ptr<Material>      green    = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));
    auto* device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const auto drawOnce = [&](bool blend) {
        DrawItem first(geometry, nullptr, vn::math::Mat4d{});
        first.program              = program;
        first.program_description  = program->description();
        first.material             = red;
        first.material_description = red->description();

        DrawItem second(geometry, nullptr, vn::math::Mat4d{});
        second.program              = program;
        second.program_description  = program->description();
        second.material             = green;
        second.material_description = green->description();
        // OPACITY BELOW 1 OPENS THE BLEND BY ITSELF (review 17 W2), so the "blend off" call draws a FULL-OPACITY
        // command that authors `enabled = false` - the content's explicit word, which the transparency merge respects.
        // What the case proves is unchanged: a blend transition is commands, not a pipeline variant.
        second.opacity = blend ? 0.5F : 1.0F;
        RenderState authored;
        authored.blend = vn::graphics::BlendState{ blend,
                                                  vn::graphics::BlendFactor::SrcAlpha,
                                                  vn::graphics::BlendFactor::OneMinusSrcAlpha };
        second.authored_state = authored;

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(first));
        commands->push_back(std::move(second));

        DrawPacket draw;
        draw.kind       = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands   = commands;

        FrameDescription frame;
        FrameDescription::Pass entry;
        entry.pass  = vn::test::framePassForTest();
        PassRenderState state;
        state.depth = DepthPolicy::Disabled;
        PassDraw pass;
        pass.target       = target.get();
        pass.attachment_policy        = AttachmentPolicy{};
        pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        pass.state        = state;
        pass.packets.push_back(std::move(draw));
        entry.draw = std::move(pass);
        frame.passes.push_back(std::move(entry));
        backend->executeFrame(frame);

        const ColorReadback read = backend->readColorImage(target.get(), 0);
        EXPECT_TRUE(read.ok());
        return pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    };

    const std::array<int, 4> blended = drawOnce(true);
    EXPECT_LE(std::abs(blended[0] - 127), 2) << "blend was not applied";
    EXPECT_LE(std::abs(blended[1] - 128), 2) << "the blended draw's green did not reach the pixel";
    const std::uint64_t pipelines_after_blend = device->contentPipelines();

    const std::array<int, 4> replaced = drawOnce(false);
    EXPECT_EQ(replaced[0], 0) << "blend stayed on: the second draw did not replace the first";
    EXPECT_EQ(replaced[1], 255) << "the second draw's green did not replace the first";
    EXPECT_EQ(device->contentPipelines(), pipelines_after_blend)
        << "turning blend off is a command, not a pipeline variant (T54)";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

/// @brief A fragment sampling the base-colour slot as a CUBE along +X: which face answers is the pixel's colour.
constexpr const char8_t* kCubePosXFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 1) uniform samplerCube vn_baseColorMap;
void main() { out_color = texture(vn_baseColorMap, vec3(1.0, 0.0, 0.0)); }
)";

/// @brief The same along -X (see kCubePosXFragment).
constexpr const char8_t* kCubeNegXFragment = u8R"(
#version 450
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 1) uniform samplerCube vn_baseColorMap;
void main() { out_color = texture(vn_baseColorMap, vec3(-1.0, 0.0, 0.0)); }
)";

TEST(VkContentDrawTest, ACubeMapIsUploadedWithEveryFaceAndSampled)
{
    // SIX LAYERS, ONE IMAGE: a cube map is the one texture whose upload crosses array layers, and a barrier that names
    // only layer 0 leaves the rest UNDEFINED - the validation layer reports the copies at submit time and the shader
    // then reads faces that are still TRANSFER_DST (both measured in the app, on the demo's sky cube). Each face is a
    // distinct colour, so the sampled pixel says WHICH face the direction picked: the layout AND the face order in one
    // assertion.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target   = makeTarget();
    const intrusive_ptr<Geometry>     geometry = makeTriangle();

    constexpr int                             kFaceSize = 4;
    const intrusive_ptr<vn::graphics::CubeMap> cube(new vn::graphics::CubeMap(kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::uint8_t                        face_colors[6][4]{
        { 255U, 0U, 0U, 255U },   // PosX: red
        { 0U, 255U, 0U, 255U },   // NegX: green
        { 0U, 0U, 255U, 255U },   // PosY: blue
        { 255U, 255U, 0U, 255U }, // NegY: yellow
        { 255U, 0U, 255U, 255U }, // PosZ: magenta
        { 0U, 255U, 255U, 255U }, // NegZ: cyan
    };
    for (int face = 0; face < cube->layerCount(); ++face) {
        auto                       image  = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(kFaceSize, kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t texel = 0; texel < pixels.size(); ++texel) {
            pixels[texel] = static_cast<std::byte>(face_colors[face][texel % 4U]);
        }
        cube->setLayer(face, intrusive_ptr<const vn::imaging::Image>(image));
    }

    const auto sampledCentre = [&](const char8_t* fragment) {
        const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
        material->setTexture(TextureSlot::BaseColor, cube);

        const intrusive_ptr<ShaderProgram> program = makeProgramWith(fragment);
        DrawItem                      command(geometry, nullptr, vn::math::Mat4d{});
        command.program              = program;
        command.program_description  = program->description();
        command.material             = material;
        command.material_description = material->description();

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(command));

        DrawPacket draw;
        draw.kind     = DrawKind::Scene;
        draw.view.camera   = identityCamera();
        draw.commands = commands;
        backend->executeFrame(drawingFrame(target.get(), std::move(draw)));

        const ColorReadback read = backend->readColorImage(target.get(), 0);
        EXPECT_TRUE(read.ok());
        return pixelAt(read.values, kTargetSize / 2, kTargetSize / 2);
    };

    EXPECT_EQ(sampledCentre(kCubePosXFragment), (std::array<int, 4>{ 255, 0, 0, 255 })) << "+X must sample the PosX face";
    EXPECT_EQ(sampledCentre(kCubeNegXFragment), (std::array<int, 4>{ 0, 255, 0, 255 })) << "-X must sample the NegX face";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "every face must be in SHADER_READ_ONLY, not only layer 0 (see ContentImages::recordUpload)";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkContentDrawTest, AShadowedFullFullscreenPassGetsTheEngineShadowBindings)
{
    // THE ENGINE'S SHADOW ABI IN A FULL-SCREEN SET: the deferred lighting program's shadowed variant declares set 0
    // binding 7 (the map) and 8 (VineShadowBlock) - the app draws exactly this program as a full-screen pass. A screen
    // set that does not offer the pair fails pipeline creation (VUID-VkGraphicsPipelineCreateInfo-layout-07988) and
    // then every draw of it, which is what the app measured.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // The G-buffer stand-in: one colour attachment and one drawn frame - a full-screen draw samples a target the device
    // has BUILT, and the bindings past its single attachment read the white fallback.
    const intrusive_ptr<RenderTarget>  source      = makeTarget();
    const intrusive_ptr<RenderTarget>  destination = makeTarget();
    const intrusive_ptr<ShaderProgram> scene       = makeProgram();
    const intrusive_ptr<Geometry>      geometry    = makeTriangle();
    {
        DrawItem command(geometry, nullptr, vn::math::Mat4d{});
        command.program             = scene;
        command.program_description = scene->description();
        auto commands               = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(command));

        DrawPacket scene_draw;
        scene_draw.kind     = DrawKind::Scene;
        scene_draw.view.camera   = identityCamera();
        scene_draw.commands = commands;
        backend->executeFrame(drawingFrame(source.get(), std::move(scene_draw)));
    }

    const intrusive_ptr<ShaderProgram> lit = vn::graphics::shadowedDeferredLightProgram();
    ASSERT_NE(lit, nullptr);

    DrawPacket fullscreen_draw;
    fullscreen_draw.kind                = DrawKind::Fullscreen;
    fullscreen_draw.view.camera              = identityCamera();
    fullscreen_draw.source              = source.get();
    fullscreen_draw.program             = lit;
    fullscreen_draw.program_description = lit->description();
    EXPECT_EQ(backend->executeFrame(fullscreenFrame(destination.get(), std::move(fullscreen_draw))), PresentOutcome::Skipped);

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U)
        << "the full-screen set must offer the engine's shadow pair (see FrameScratch::fullscreenSetFor)";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    const auto report = backend->frameReport();
    ASSERT_TRUE(report.counters.has_value());
    EXPECT_EQ(report.counters->draw_packets, 1U) << "the full-screen pass reached the queue";
    backend->shutdown();
}

/**
 * @brief THE SET CACHE KEY NAMES THE SAMPLER: one texture read through two authored samplers is two sets.
 *
 * @details
 * - A descriptor set binds a view AND a sampler (`VkDescriptorImageInfo` writes a combined image sampler), and two
 *   materials may name ONE texture through two `Sampler` values (`Material::setTexture(slot, texture, sampler)`). A key
 *   that stopped at the view served the first material's sampler to the second: silent wrongness, not a cache miss - so
 *   the key must name everything the cached value depends on (`G4`).
 * - THE WITNESS IS THE BUILD COUNT, not a pixel: the discriminating fact is "a second set was needed", and a filter
 *   difference on a 1x1 texture is (correctly) invisible in the picture.
 */
TEST(VkContentDrawTest, TwoMaterialsReadingOneTextureThroughTwoSamplersAreTwoSets)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = makeProgramWith(kTextureFragment);
    const intrusive_ptr<Geometry>      geometry = makeTriangle();

    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0] = std::byte{ 255U };
    texel[1] = std::byte{ 255U };
    texel[2] = std::byte{ 255U };
    texel[3] = std::byte{ 255U };
    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);

    vn::graphics::Sampler nearest;
    nearest.min_filter = vn::graphics::Sampler::Filter::Nearest;
    nearest.mag_filter = vn::graphics::Sampler::Filter::Nearest;
    const vn::graphics::Sampler linear{}; // the defaults: linear minification and magnification

    const auto frameWith = [&](const vn::graphics::Sampler& sampler) {
        const intrusive_ptr<Material> material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
        material->setTexture(TextureSlot::BaseColor, texture, sampler);

        DrawItem command(geometry, nullptr, vn::math::Mat4d{});
        command.program              = program;
        command.program_description  = program->description();
        command.material             = material;
        command.material_description = material->description();

        auto commands = std::make_shared<std::vector<DrawItem>>();
        commands->push_back(std::move(command));

        DrawPacket draw;
        draw.kind         = DrawKind::Scene;
        draw.view.camera  = identityCamera();
        draw.commands     = commands;
        return drawingFrame(target.get(), std::move(draw));
    };

    (void)backend->executeFrame(frameWith(nearest), nullptr);
    const std::uint64_t after_nearest = device->contentCacheCounters().set_builds;

    (void)backend->executeFrame(frameWith(linear), nullptr);
    const std::uint64_t after_linear = device->contentCacheCounters().set_builds;

    EXPECT_EQ(after_linear - after_nearest, 1U)
        << "the second material names another sampler, so the set it binds is another value";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

/**
 * @brief A PASS' ENTRY IS ONE DEPENDENCY, whatever the pass writes.
 *
 * WHY THIS CASE EXISTS. Vulkan describes "these attachments are ordered after what came before" as ONE dependency holding
 * N image barriers; recording one dependency per attachment says the same thing with N commands. The count is published
 * (`Device::recordingNumbers`) because a batching change is otherwise invisible: the picture is identical either way, so
 * only a number can tell "one command with three barriers" from "three commands with one".
 */
TEST(VkContentDrawTest, APassEntryIsOneDependencyWhateverItWrites)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    // THREE COLOUR ATTACHMENTS, no depth: the pass' entry owes one barrier per attachment.
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_dependency_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(kTargetSize, kTargetSize);

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass        = vn::test::framePassForTest();
    PassDraw pass;
    pass.target       = target.get();
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    entry.draw        = std::move(pass);
    frame.passes.push_back(std::move(entry));

    const vn::backend_vk::Device::RecordingNumbers before = device->recordingNumbers();
    (void)backend->executeFrame(frame, nullptr);
    const vn::backend_vk::Device::RecordingNumbers after = device->recordingNumbers();

    EXPECT_EQ(after.barrier_dependencies - before.barrier_dependencies, 1U)
        << "a pass' entry is ONE dependency, however many attachments it states";
    EXPECT_EQ(after.barriers - before.barriers, 3U) << "that dependency carries one barrier per colour attachment";

    backend->shutdown();
}

/**
 * @brief A STATE COMMAND IS ISSUED WHEN THE STATE CHANGES, not once per draw.
 *
 * WHY THIS CASE EXISTS. `T54` made every state item a command (no pipeline per state change), which is right - but a draw
 * that states what the command buffer already holds still pays a driver call. Two identical commands in one pass must
 * therefore cost what one costs, and `state_commands` is the number that says so: the viewport and the scissor are PASS
 * facts, so a pass with N draws used to state the same rectangle N times.
 */
TEST(VkContentDrawTest, ARepeatedStateIsNotARepeatedCommand)
{
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
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    // TWO IDENTICAL COMMANDS: same pipeline, same resolved state, same rectangle - the only difference between the two
    // frames below is how many times the pass draws.
    const auto frameWith = [&](const std::size_t command_count) {
        auto commands = std::make_shared<std::vector<DrawItem>>();
        for (std::size_t index = 0; index < command_count; ++index) {
            DrawItem command(geometry, nullptr, vn::math::Mat4d{});
            command.program              = program;
            command.program_description  = program->description();
            command.material             = material;
            command.material_description = material->description();
            commands->push_back(std::move(command));
        }
        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = commands;
        return drawingFrame(target.get(), std::move(draw));
    };

    (void)backend->executeFrame(frameWith(1U), nullptr);
    const std::uint64_t one_draw = device->recordingNumbers().state_commands;
    EXPECT_GT(one_draw, 0U) << "a drawn frame states something";

    (void)backend->executeFrame(frameWith(2U), nullptr);
    const std::uint64_t two_draws = device->recordingNumbers().state_commands;

    EXPECT_EQ(two_draws - one_draw, one_draw)
        << "the second command of the pass states the same pipeline, rectangle and raster state, so it costs no state "
        << "command (measured " << (two_draws - one_draw) << " against " << one_draw << ")";

    backend->shutdown();
}

/**
 * @brief THE STATE SURVIVES A PIPELINE BIND, because the dynamic set is a backend constant.
 *
 * WHY THIS CASE EXISTS (`K6`, review 9). `bindPipeline` used to reset the whole state shadow, on the reasoning that a
 * pipeline MAY declare a different dynamic set. Every pipeline this backend builds comes from `buildPipeline`, which
 * declares the same twelve dynamic states, and Vulkan resets no dynamic state on a bind - so the reset re-stated the
 * viewport, the scissor and the whole raster half for a pipeline that reads exactly the values the buffer holds. Two
 * commands with DIFFERENT programs therefore cost one state command (the bind itself), not two statements of
 * everything. A mutation that restores the reset makes the deltas below read `2 * one_draw` (two commands) and
 * `4 * one_draw` (four commands) instead of `one_draw + 1` and `one_draw + 3`.
 */
TEST(VkContentDrawTest, TwoAlternatingPipelinesRestateNoState)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    const intrusive_ptr<RenderTarget>           target   = makeTarget();
    const intrusive_ptr<Geometry>               geometry = makeTriangle();
    const intrusive_ptr<ShaderProgram>          first    = makeProgram();
    const intrusive_ptr<ShaderProgram>          second   = makeProgramWith(kAlphaFragment);
    const intrusive_ptr<vn::graphics::Material> material = makeMaterial(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    // TWO PIPELINES, ONE STATE: the commands alternate their programs and share everything else, so the only thing a
    // later command can need is a bind.
    const auto frameWith = [&](const std::size_t command_count) {
        auto commands = std::make_shared<std::vector<DrawItem>>();
        for (std::size_t index = 0; index < command_count; ++index) {
            DrawItem command(geometry, nullptr, vn::math::Mat4d{});
            command.program              = (index % 2U) == 0U ? first : second;
            command.program_description  = command.program->description();
            command.material             = material;
            command.material_description = material->description();
            commands->push_back(std::move(command));
        }
        DrawPacket draw;
        draw.kind        = DrawKind::Scene;
        draw.view.camera = identityCamera();
        draw.commands    = commands;
        return drawingFrame(target.get(), std::move(draw));
    };

    (void)backend->executeFrame(frameWith(1U), nullptr);
    const std::uint64_t one_draw = device->recordingNumbers().state_commands;
    ASSERT_GT(one_draw, 0U) << "a drawn frame states something";

    (void)backend->executeFrame(frameWith(2U), nullptr);
    const std::uint64_t two_draws = device->recordingNumbers().state_commands;
    EXPECT_EQ(two_draws - one_draw, one_draw + 1U)
        << "the second command binds a different pipeline and states nothing else (measured " << (two_draws - one_draw)
        << " against " << one_draw << " + 1)";

    (void)backend->executeFrame(frameWith(4U), nullptr);
    const std::uint64_t four_draws = device->recordingNumbers().state_commands;
    // FOUR COMMANDS, FOUR BINDS (each differs from its predecessor) AND ONE STATEMENT OF EVERYTHING ELSE: the relation
    // is `one_draw + (commands - 1)`, because `one_draw` already carries the first bind.
    EXPECT_EQ(four_draws - two_draws, one_draw + 3U)
        << "two more alternating binds cost two state commands, not two statements (measured "
        << (four_draws - two_draws) << " against " << one_draw << " + 3)";

    backend->shutdown();
}

/**
 * @brief THE FRAME NAMES ITSELF AND EVERY PASS to a capture tool.
 *
 * WHY THIS CASE EXISTS. A capture without labels is one anonymous command stream: the host's `FrameReport` names the
 * passes and the report's timing attributes them, but the tool a developer opens to look at a frame sees neither. The
 * names are the frame's own (`RenderPass::name`), so a label count of `passes + 1` is the whole contract - and a session
 * without `VK_EXT_debug_utils` names nothing, which is why the case skips on the advertised answer rather than on the
 * observation.
 */
TEST(VkContentDrawTest, AFrameAndItsPassesAreNamedToACaptureTool)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);
    if (!device->debugMarkers()) {
        GTEST_SKIP() << "this session did not resolve VK_EXT_debug_utils, so it names nothing to a tool";
    }

    const intrusive_ptr<RenderTarget> first  = makeTarget();
    const intrusive_ptr<RenderTarget> second = makeTarget();

    FrameDescription frame;
    for (RenderTarget* target : { first.get(), second.get() }) {
        FrameDescription::Pass entry;
        entry.pass = vn::test::framePassForTest();
        PassDraw pass;
        pass.target       = target;
        pass.attachment_policy        = AttachmentPolicy{};
        pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
        entry.draw        = std::move(pass);
        frame.passes.push_back(std::move(entry));
    }

    const std::uint64_t before = device->recordingNumbers().debug_labels;
    (void)backend->executeFrame(frame, nullptr);
    const std::uint64_t after = device->recordingNumbers().debug_labels;

    EXPECT_EQ(after - before, 3U) << "one region for the frame and one for each of its two passes";

    backend->shutdown();
}
