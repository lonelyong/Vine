/**
 * @brief ADR-2026-11-14's second half (backlog T39): a warm-up copies the textures it staged, and the frame that
 *        follows uploads nothing.
 *
 * What this pins, and why each half matters:
 *   - `prepareFrame` hands the images it staged to the session's transfer task and waits for that batch, so a
 *     texture's bytes land during the warm-up instead of in the first frame that samples it - the promise the vk
 *     backend already keeps (`VkPrepareUploadTest`), now kept here through the transfer step the frame itself uses;
 *   - the report says what the call built: `PrepareReport::uploads` counts the streams AND the texture
 *     (`FrameUploads`' two kinds), not only the streams;
 *   - the staged host bytes are gone once the copies have run (see `MaterialImages::staged`), which is what stops
 *     a descriptor set compiled later from copying the same bytes a second time;
 *   - zero work means zero submit: a second prepare of the same frame answers zeros;
 *   - the picture proves it: the pixel the frame draws is the texture's colour, which can be sampled only once the
 *     upload has run.
 *
 * The device cases skip on a machine with no device session, like every device case in this repository.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>

#include <vine/backend_vsg/internal/BackendContent.hpp>
#include <vine/backend_vsg/internal/MaterialImages.hpp>
#include <vine/backend_vsg/internal/VsgBackend.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::String;
using vn::graphics::CameraSnapshot;
using vn::graphics::ClearPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
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

/// @brief The texel the texture is filled with, and therefore the colour the sampled pixel must be.
constexpr std::array<int, 4> kTextureColour{ 0, 0, 255, 255 };

/// @brief The content ABI's vertex stage: the camera push plus the per-draw block (see ShaderAbi / ContentPush).
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

/// @brief Samples the base-colour slot: the NAME is what tells the backend this is a material map.
constexpr const char8_t* kTextureFragment = u8R"(
#version 450
layout(set = 0, binding = 1) uniform sampler2D vn_baseColorMap;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texture(vn_baseColorMap, vec2(0.5, 0.5));
}
)";

/// @brief Everything one textured frame needs, held together so the frame's borrowed pointers stay valid.
struct TexturedScene {
    intrusive_ptr<RenderTarget>  target;
    intrusive_ptr<ShaderProgram> program;
    intrusive_ptr<Geometry>      geometry;
    intrusive_ptr<Material>      material;
    intrusive_ptr<Texture2D>     texture;
    FrameDescription             frame;
};

/// @brief A 64x64 RGBA8 + D24 target.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vsg_t39_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief The two-stage program whose fragment half samples the base-colour slot.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeTextureProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vsg_t39_program");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kTextureFragment), String(u8"main") });
    return program;
}

/// @brief A triangle in clip space, indexed, as the one command of its geometry (z = 0.5: reverse-Z's mid band).
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
    primitive.indices  = Primitive::IndexSpan::whole(intrusive_ptr<const vn::Buffer<std::uint32_t>>(
        new vn::Buffer<std::uint32_t>(std::move(indices))));
    geometry->clearPrimitives();
    geometry->addPrimitive(primitive);
    geometry->bumpRevision();
    return geometry;
}

/// @brief A 1x1 RGBA8 texture whose single texel is @ref kTextureColour, installed as the base-colour slot.
[[nodiscard]] intrusive_ptr<Texture2D> makeTexture()
{
    auto image = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    const std::span<std::byte> texel = image->mipData(0);
    texel[0]                         = std::byte{ static_cast<std::uint8_t>(kTextureColour[0]) };
    texel[1]                         = std::byte{ static_cast<std::uint8_t>(kTextureColour[1]) };
    texel[2]                         = std::byte{ static_cast<std::uint8_t>(kTextureColour[2]) };
    texel[3]                         = std::byte{ static_cast<std::uint8_t>(kTextureColour[3]) };

    auto texture = intrusive_ptr<Texture2D>(new Texture2D(1, 1, vn::imaging::PixelFormat::Rgba8Unorm));
    texture->setImage(image);
    return texture;
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

/// @brief Builds the scene both cases prepare: one off-screen target and one draw whose material samples a texture.
[[nodiscard]] TexturedScene makeTexturedScene()
{
    TexturedScene scene;
    scene.target   = makeTarget();
    scene.program  = makeTextureProgram();
    scene.geometry = makeTriangle();
    scene.texture  = makeTexture();

    scene.material = intrusive_ptr<Material>(new Material());
    scene.material->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    scene.material->setTexture(TextureSlot::BaseColor, scene.texture);

    RenderCommand command(scene.geometry, nullptr, vn::math::Mat4d{});
    command.program              = scene.program;
    command.program_description  = scene.program->description();
    command.material             = scene.material;
    command.material_description = scene.material->description();

    auto commands = std::make_shared<std::vector<RenderCommand>>();
    commands->push_back(std::move(command));

    DrawDescription draw;
    draw.kind     = DrawDescription::Kind::Scene;
    draw.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = scene.target.get();
    pass.clear        = ClearPolicy{};
    pass.clear->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.draws.push_back(std::move(draw));
    entry.draw = std::move(pass);

    scene.frame.passes.push_back(std::move(entry));
    return scene;
}

/// @brief The RGBA8 pixel at (@p x, @p y) of a packed readback.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

} // namespace

TEST(PrepareUploadTest, APreparedTexturedFrameCopiesItsTextureAndUploadsNothingWhenItRuns)
{
    const vn::graphics::DeviceDesc desc{ DevicePresentation::None, nullptr };
    vn::intrusive_ptr<vn::backend_vsg::VsgBackend> backend(new vn::backend_vsg::VsgBackend(desc));
    ASSERT_TRUE(backend->initialize()) << "this machine has no device session the backend can come up on";

    const TexturedScene scene = makeTexturedScene();
    const auto* const images  = vn::backend_vsg::detail::BackendContentAccess::images(*backend);
    ASSERT_NE(images, nullptr) << "a live session owns the image cache its frames sample";

    // NOTHING IS STAGED YET: the texture has not been acquired by any walk.
    EXPECT_EQ(images->count(), 0U);
    EXPECT_EQ(images->staged(), 0U);

    // THE WARM-UP PAYS FOR EVERYTHING, TEXTURES INCLUDED - the number this ADR exists to make visible. Two streams
    // (positions + indices) and one texture image.
    const PrepareReport prepared = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(prepared.prepared()) << "outcome " << static_cast<int>(prepared.outcome);
    EXPECT_EQ(prepared.uploads, 3U) << "2 stream uploads (positions + indices) + 1 texture image";
    EXPECT_EQ(prepared.passes, 1U);
    EXPECT_EQ(prepared.draws, 1U);

    // THE BYTES LANDED: the image is resident in the cache, and its host-side staging is gone - which is the fact a
    // later descriptor set reads when it decides whether to copy.
    EXPECT_EQ(images->count(), 1U) << "the material's base-colour map was built";
    EXPECT_EQ(images->staged(), 0U) << "the warm-up copied the staged bytes and gave them back";

    // PREPARE IS NOT A FRAME: the transfer batch it submitted moved nothing on the frame clock.
    const FrameReport after_prepare = backend->frameReport();
    ASSERT_TRUE(after_prepare.progress.has_value());
    EXPECT_EQ(after_prepare.progress->submitted, 0U) << "the transfer batch is not a frame";
    EXPECT_EQ(after_prepare.progress->presented, 0U);
    EXPECT_FALSE(after_prepare.uploads.has_value()) << "no frame has run, so no frame's upload numbers exist";

    // THE FRAME ITSELF UPLOADS NOTHING: the copies ran in the preparation, so this frame only draws.
    EXPECT_EQ(backend->executeFrame(scene.frame, nullptr), PresentOutcome::Skipped);
    const FrameReport ran = backend->frameReport();
    ASSERT_TRUE(ran.uploads.has_value()) << "a frame ran, so this device has numbers for it";
    EXPECT_EQ(ran.uploads->stream_uploads, 0U) << "the prepared streams were reused";
    EXPECT_EQ(ran.uploads->texture_uploads, 0U) << "the prepared texture was reused";
    EXPECT_EQ(images->staged(), 0U) << "the frame staged nothing either";

    // THE PROOF THAT THE UPLOAD LANDED DURING PREPARE: the sampled pixel is the texture's colour.
    std::vector<std::uint8_t> pixels;
    ASSERT_TRUE(backend->readColorBuffer(scene.target.get(), 0, pixels));
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);
    EXPECT_EQ(pixelAt(pixels, kTargetSize / 2, kTargetSize / 2), kTextureColour)
        << "the base-colour texture was not resident when the frame sampled it";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(PrepareUploadTest, ASecondPrepareCopiesNothing)
{
    const vn::graphics::DeviceDesc desc{ DevicePresentation::None, nullptr };
    vn::intrusive_ptr<vn::backend_vsg::VsgBackend> backend(new vn::backend_vsg::VsgBackend(desc));
    ASSERT_TRUE(backend->initialize()) << "this machine has no device session the backend can come up on";

    const TexturedScene scene = makeTexturedScene();

    const PrepareReport first = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(first.prepared()) << "outcome " << static_cast<int>(first.outcome);
    ASSERT_GT(first.uploads, 0U) << "the first prepare had to build the content";

    // ZERO WORK, ZERO SUBMIT: nothing is built and nothing is staged, so the second call has nothing to copy and
    // submits nothing - the shape of the frame asked for is still reported, because that is a fact about the frame,
    // not the build.
    const PrepareReport again = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(again.prepared());
    EXPECT_EQ(again.uploads, 0U) << "an unchanged frame uploads nothing";
    EXPECT_EQ(again.halves, 0U);
    EXPECT_EQ(again.pipelines, 0U);
    EXPECT_EQ(again.passes, first.passes) << "the shape of the frame asked for does not move";
    EXPECT_EQ(again.draws, first.draws);

    // AND NEITHER PREPARE WAS A FRAME, however much it submitted.
    const FrameReport after_both = backend->frameReport();
    ASSERT_TRUE(after_both.progress.has_value());
    EXPECT_EQ(after_both.progress->submitted, 0U);
    EXPECT_EQ(after_both.progress->presented, 0U);
    EXPECT_FALSE(after_both.uploads.has_value()) << "no frame has run, so no frame's upload numbers exist";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}
