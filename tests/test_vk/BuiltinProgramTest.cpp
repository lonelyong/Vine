/**
 * @brief The ENGINE's own programs on this backend: the builtin forward text compiled under the variant its drawable
 *        needs, and the instance stream it reads when a command is placed more than once.
 *
 * WHAT THIS PINS, and what only a device can. `VariantTest` pins the RULES (which names a drawable asks for) and that
 * the names reach the text. What is left is the picture: the engine's text declares the material block, five texture
 * slots, the lights, the draw block AND the shadow map + block (its `forwardFragmentSource()` inserts that ABI
 * unconditionally), so "the engine's programs run here" means every one of those bindings, the vertex layout the
 * text declares (positions, normals, colour, texcoord, instance matrix) and the variant defines all agree. Each case
 * below is one of those facts expressed as a pixel.
 *
 * The device cases skip on a machine with no Vulkan device, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/String.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameStats.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/Residency.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::BufferSlice;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DepthMode;
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
using vn::graphics::ResidencyKind;
using vn::graphics::ResidencyRow;
using vn::graphics::ShaderProgram;
using vn::graphics::Texture2D;
using vn::graphics::TextureSlot;
using vn::graphics::Topology;
using vn::intrusive_ptr;

namespace
{

constexpr int kTargetSize = 64;

/// @brief A 64x64 RGBA8 + D24 target (the shape every device case in this suite uses).
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_builtin_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A triangle in clip space with the channels the engine's vertex stages read.
///
/// POSITIONS AND NORMALS ARE ALWAYS FED: the engine's forward vertex stage declares both unconditionally, and a
/// backend that leaves a declared attribute unfed draws undefined values (validation says so, and the picture would
/// be the driver's interpretation). The colour and texcoord channels are the variant switches.
[[nodiscard]] intrusive_ptr<Geometry> makeTriangle(bool with_colour, bool with_texcoords2, float half_extent = 0.75F)
{
    const float x = half_extent;
    auto        geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -x,   -0.75F, 0.5F,
        x,    -0.75F, 0.5F,
        0.0F, 0.25F,  0.5F,
    })));
    geometry->setNormals(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
        0.0F, 0.0F, 1.0F,
    })));
    if (with_colour) {
        // An AUTHORED red colour: the shaded albedo is the material's white times this.
        geometry->setColors(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
            1.0F, 0.0F, 0.0F, 1.0F,
            1.0F, 0.0F, 0.0F, 1.0F,
            1.0F, 0.0F, 0.0F, 1.0F,
        })));
    }
    if (with_texcoords2) {
        geometry->setTexcoords2(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
            0.0F, 0.0F,
            1.0F, 0.0F,
            0.5F, 1.0F,
        })));
    }
    geometry->bumpRevision();
    return geometry;
}

[[nodiscard]] intrusive_ptr<Material> makeMaterial(const Colorf& colour, intrusive_ptr<Texture2D> map = nullptr)
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(colour);
    if (map != nullptr) {
        material->setTexture(TextureSlot::BaseColor, std::move(map));
    }
    return material;
}

/// @brief A 1x1 map of one colour (no filtering freedom, so "sampled" is a byte-exact claim).
[[nodiscard]] intrusive_ptr<Texture2D> makePixel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
{
    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0]                         = static_cast<std::byte>(r);
    texel[1]                         = static_cast<std::byte>(g);
    texel[2]                         = static_cast<std::byte>(b);
    texel[3]                         = std::byte{ 255U };
    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    return texture;
}

/// @brief The engine's UNLIT forward program: the surface colour is the answer, so each case's pixel is the variant's
///        own evidence (a lighting term would blur "the define arrived" into "the lights were packed").
[[nodiscard]] intrusive_ptr<ShaderProgram> unlitProgram()
{
    auto program = vn::graphics::unlitForwardProgram();
    EXPECT_NE(program, nullptr) << "the engine ships its unlit forward program";
    return program;
}

/// @brief One command of @p geometry through @p program, shaded with @p material.
[[nodiscard]] RenderCommand commandOf(const intrusive_ptr<Geometry>& geometry, const intrusive_ptr<ShaderProgram>& program, const intrusive_ptr<Material>& material)
{
    RenderCommand command(geometry, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    return command;
}

/// @brief A frame with ONE pass: it clears @p target black and issues @p commands through the identity camera.
[[nodiscard]] FrameDescription frameWith(RenderTarget* target, const std::shared_ptr<const std::vector<RenderCommand>>& commands)
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = camera;
    draw.commands = commands;

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass = vn::test::framePassForTest();
    PassDraw pass;
    pass.target       = target;
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.state.depth  = DepthMode::TestAndWrite;
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback of the builtin target.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

/// @brief The live count of the Streams row, or 0 when the report keeps no such row.
[[nodiscard]] std::uint32_t streamsLive(const vn::graphics::FrameReport& report)
{
    if (!report.retention.has_value() || !report.retention->residency.has_value()) {
        return 0U;
    }
    for (const ResidencyRow& row : report.retention->residency->rows()) {
        if (row.kind == ResidencyKind::Streams) {
            return row.live;
        }
    }
    return 0U;
}

} // namespace

TEST(VkBuiltinProgramTest, TheEngineUnlitProgramDrawsItsMaterialColour)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget> target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program = unlitProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle(false, false);
    const intrusive_ptr<Material>      red      = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    backend->executeFrame(frameWith(target.get(), std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ commandOf(geometry, program, red) })));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the engine's unlit program did not shade the centre with the material's base colour";
    EXPECT_EQ(pixelAt(pixels, 2, 2), (std::array<int, 4>{ 0, 0, 0, 255 })) << "the corner is the pass' clear";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U)
        << "the engine's own text has to compile here, shadow ABI and all";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkBuiltinProgramTest, ABaseColorMapCompilesAndSamplesTheTexturedVariant)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = unlitProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle(false, true);
    const intrusive_ptr<Material>      material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F), makePixel(0U, 0U, 255U));

    backend->executeFrame(frameWith(target.get(), std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ commandOf(geometry, program, material) })));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 0, 0, 255, 255 }))
        << "the engine's `VN_TEXTURED` variant did not sample the base-colour map";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkBuiltinProgramTest, AVertexColourChannelModulatesTheAlbedoOnItsOwnVariant)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = unlitProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle(true, false); // feeds the red colour channel
    const intrusive_ptr<Material>      material = makeMaterial(Colorf(1.0F, 1.0F, 1.0F, 1.0F));

    backend->executeFrame(frameWith(target.get(), std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ commandOf(geometry, program, material) })));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), (std::array<int, 4>{ 255, 0, 0, 255 }))
        << "the engine's `VN_VERTEX_COLOR` variant did not modulate the white material with the authored colour";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkBuiltinProgramTest, EveryInstanceIsDrawnWhereItsMatrixPlacesIt)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // A NARROW triangle placed twice: -0.5 and +0.5 on x. The engine's `VN_INSTANCED` vertex stage multiplies the
    // instance matrix into the drawable's model, so the two copies must land where the matrices say and the
    // un-instanced position must stay clear.
    const intrusive_ptr<RenderTarget>  target   = makeTarget();
    const intrusive_ptr<ShaderProgram> program  = unlitProgram();
    const intrusive_ptr<Geometry>      geometry = makeTriangle(false, false, 0.25F);
    const intrusive_ptr<Material>      material = makeMaterial(Colorf(1.0F, 0.0F, 0.0F, 1.0F));

    // Two column-major translation matrices (four vec4 columns each).
    auto instances = intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        -0.5F, 0.0F, 0.0F, 1.0F,
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.5F, 0.0F, 0.0F, 1.0F,
    }));

    RenderCommand command = commandOf(geometry, program, material);
    command.instance_transforms = BufferSlice<float>::whole(instances);
    command.first_instance      = 0U;
    command.instance_count      = 2U;

    backend->executeFrame(frameWith(target.get(), std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ command })));

    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, 12, 54), (std::array<int, 4>{ 255, 0, 0, 255 })) << "the first instance is not where its matrix places it";
    EXPECT_EQ(pixelAt(pixels, 51, 54), (std::array<int, 4>{ 255, 0, 0, 255 })) << "the second instance is not where its matrix places it";
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, 54), (std::array<int, 4>{ 0, 0, 0, 255 }))
        << "the un-instanced position drew something: the instance matrices were ignored";
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::CompileFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);

    // THE INSTANCE STREAM IS A STREAM (see RenderDevice::releaseContent(Buffer)): releasing its buffer drops one live
    // row, and the next frame pays for it by uploading it again - with the same picture.
    const std::uint32_t before = streamsLive(backend->frameReport());
    ASSERT_GE(before, 3U) << "positions, indices and the instance stream are three live streams";
    EXPECT_TRUE(backend->releaseContent(*instances));
    EXPECT_EQ(streamsLive(backend->frameReport()), before - 1U) << "the released instance stream stayed resident";

    backend->executeFrame(frameWith(target.get(), std::make_shared<const std::vector<RenderCommand>>(std::vector<RenderCommand>{ command })));
    ASSERT_TRUE(backend->readColorBuffer(target.get(), 0, pixels));
    EXPECT_EQ(pixelAt(pixels, 12, 54), (std::array<int, 4>{ 255, 0, 0, 255 })) << "the re-uploaded instance stream drew nothing";
    EXPECT_EQ(pixelAt(pixels, 51, 54), (std::array<int, 4>{ 255, 0, 0, 255 }));
    EXPECT_EQ(streamsLive(backend->frameReport()), before) << "the next frame did not bring the instance stream back";

    backend->shutdown();
}
