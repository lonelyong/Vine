/**
 * @brief M7's acceptance for ADR-2026-11-14: a warm-up submits ONE transfer-only batch of its own, and the frame that
 *        follows it then uploads nothing.
 *
 * What this pins, and why each half matters:
 *   - `prepareFrame` may record and submit a command buffer holding nothing but transfers, so a texture's bytes land
 *     during the warm-up instead of in the first frame that samples it (the ADR's decision B);
 *   - that batch is NOT a frame: `FrameProgress::submitted` / `presented` do not move and `FrameReport::uploads`
 *     stays disengaged, so "the content was prepared" and "a frame ran" remain two different answers;
 *   - zero work means zero submit: a second prepare of the same frame answers zeros and submits nothing;
 *   - the picture proves it: the pixel the frame draws is the texture's colour, which can be sampled only once the
 *     upload has run - a report that claimed the upload and a picture that never got it would disagree.
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
#include <vine/backend_vk/internal/Device.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/FrameReport.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

#include "PlanTestSupport.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::FrameReport;
using vn::graphics::Geometry;
using vn::graphics::PassDraw;
using vn::graphics::PrepareReport;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::ReadbackStatus;
using vn::graphics::RenderBackendRegistry;
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

/// @brief The texel the texture is filled with, and therefore the colour the sampled pixel must be.
constexpr std::array<int, 4> kTextureColour{ 0, 0, 255, 255 };

/// @brief The M4 content ABI's vertex stage: the camera push plus the per-draw block (set 1 binding 0).
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

/// @brief Everything one textured frame needs, held together so the frame's borrowed pointers stay valid.
struct TexturedScene {
    intrusive_ptr<RenderTarget>  target;
    intrusive_ptr<ShaderProgram> program;
    intrusive_ptr<Geometry>      geometry;
    intrusive_ptr<vn::graphics::Material> material;
    intrusive_ptr<Texture2D>     texture;
    FrameDescription             frame;
};

/// @brief A 64x64 RGBA8 + D24 target.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_m7_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief The two-stage program whose fragment half samples the first texture slot.
[[nodiscard]] intrusive_ptr<ShaderProgram> makeTextureProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_m7_program");
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
    camera.eye        = vn::math::Vec3d( 0.0, 0.0, 0.0 );
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

    scene.material = intrusive_ptr<vn::graphics::Material>(new vn::graphics::Material());
    scene.material->setBaseColor(Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    scene.material->setTexture(TextureSlot::BaseColor, scene.texture);

    DrawItem command(scene.geometry, nullptr, vn::math::Mat4d{});
    command.program             = scene.program;
    command.program_description = scene.program->description();
    command.material            = scene.material;
    command.material_description = scene.material->description();

    auto commands = std::make_shared<std::vector<DrawItem>>();
    commands->push_back(std::move(command));

    DrawPacket draw;
    draw.kind     = DrawKind::Scene;
    draw.view.camera   = identityCamera();
    draw.commands = commands;

    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target       = scene.target.get();
    pass.attachment_policy        = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.packets.push_back(std::move(draw));
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

TEST(VkPrepareUploadTest, APreparedTexturedFrameUploadsNothingWhenItRuns)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const TexturedScene scene = makeTexturedScene();

    // THE WARM-UP PAYS FOR EVERYTHING, TEXTURES INCLUDED - the number this ADR exists to make visible. Two streams
    // (positions + indices) and one texture image: `VkResidencyTest.PrepareBuildsWhatTheFrameNeedsAndSubmitsNothing`
    // reports 2 for the same scene WITHOUT a texture, so the third upload is the texture, not a stream.
    const PrepareReport prepared = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(prepared.prepared()) << "outcome " << static_cast<int>(prepared.outcome);
    EXPECT_EQ(prepared.uploads, 3U) << "2 stream uploads (positions + indices) + 1 texture image";
    EXPECT_EQ(prepared.passes, 1U);
    EXPECT_EQ(prepared.draw_packets, 1U);

    // PREPARE IS NOT A FRAME: the transfer batch it submitted moved nothing on the frame clock, and no frame's upload
    // numbers exist yet (see FrameProgress / FrameReport).
    const FrameReport after_prepare = backend->frameReport();
    ASSERT_TRUE(after_prepare.progress.has_value());
    EXPECT_EQ(after_prepare.progress->submitted, 0U) << "the transfer batch is not a frame";
    EXPECT_EQ(after_prepare.progress->presented, 0U);
    EXPECT_FALSE(after_prepare.uploads.has_value()) << "no frame has run, so no frame's upload numbers exist";

    // THE FRAME ITSELF UPLOADS NOTHING: the copies ran in the preparation, so this frame only draws.
    ASSERT_EQ(backend->executeFrame(scene.frame, nullptr), PresentOutcome::Skipped);
    const FrameReport ran = backend->frameReport();
    ASSERT_TRUE(ran.uploads.has_value()) << "a frame ran, so this device has numbers for it";
    EXPECT_EQ(ran.uploads->stream_uploads, 0U) << "the prepared streams were reused";
    EXPECT_EQ(ran.uploads->texture_uploads, 0U) << "the prepared texture was reused";

    // THE PROOF THAT THE UPLOAD LANDED DURING PREPARE: the sampled pixel is the texture's colour. A copy that never ran
    // would leave an image nothing wrote, not this colour.
    const ColorReadback read = backend->readColorImage(scene.target.get(), 0);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.values.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), kTextureColour) << "the base-colour texture was not resident when the frame sampled it";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    backend->shutdown();
}

TEST(VkPrepareUploadTest, ASecondPrepareSubmitsNothing)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const TexturedScene scene = makeTexturedScene();

    const PrepareReport first = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(first.prepared()) << "outcome " << static_cast<int>(first.outcome);
    ASSERT_GT(first.uploads, 0U) << "the first prepare had to build the content";

    // ZERO WORK, ZERO SUBMIT: nothing is built, so the second call records no copy and submits no command buffer -
    // the shape of the frame asked for is still reported, because that is a fact about the frame, not the build.
    const PrepareReport again = backend->prepareFrame(scene.frame);
    ASSERT_TRUE(again.prepared());
    EXPECT_EQ(again.uploads, 0U) << "an unchanged frame uploads nothing";
    EXPECT_EQ(again.halves, 0U);
    EXPECT_EQ(again.pipelines, 0U);
    EXPECT_EQ(again.passes, first.passes) << "the shape of the frame asked for does not move";
    EXPECT_EQ(again.draw_packets, first.draw_packets);

    // AND NEITHER PREPARE WAS A FRAME, however much it submitted (see FrameProgress).
    const FrameReport after_both = backend->frameReport();
    ASSERT_TRUE(after_both.progress.has_value());
    EXPECT_EQ(after_both.progress->submitted, 0U);
    EXPECT_EQ(after_both.progress->presented, 0U);
    EXPECT_FALSE(after_both.uploads.has_value()) << "no frame has run, so no frame's upload numbers exist";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}

TEST(VkPrepareUploadTest, APrepareWhoseSubmitFailsPublishesNothingAndTheNextFrameUploadsAgain)
{
    // T66 AT BOTH HALVES OF THE PUBLICATION RULE: a texture becomes RESIDENT when its copy is PROVEN, not when its
    // copy is recorded (see ContentImages: batchCompleted / batchAbandoned). A submission that fails between the two:
    //   - must leave the image table as it was, or the next frame finds a revision it believes is resident, binds an
    //     image nobody filled, and never uploads it again (the defect T66 records);
    //   - must leave the frame's LAYOUT RECORDS where the last executed frame left them (P17), so a readback answers
    //     NotReady instead of a picture nobody can vouch for - and the frame after it draws normally.
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr) << "the registry hands out the vk backend this case is about";

    const TexturedScene scene = makeTexturedScene();

    // THE SUBMISSION IS REFUSED (see Device::refuseNextSubmit): the batch records three copies and runs none.
    device->refuseNextSubmit();
    const PrepareReport refused = backend->prepareFrame(scene.frame);
    EXPECT_TRUE(refused.prepared()) << "a queue that refused the batch is not a preparation defect";
    EXPECT_EQ(refused.uploads, 3U) << "what the call BUILT is still what it reports (2 streams + the texture)";

    // THE FRAME THAT FOLLOWS OWNS EVERY UPLOAD: nothing the failed batch copied became resident.
    ASSERT_EQ(backend->executeFrame(scene.frame, nullptr), PresentOutcome::Skipped);
    const FrameReport ran = backend->frameReport();
    ASSERT_TRUE(ran.uploads.has_value()) << "a frame ran, so this device has numbers for it";
    // A GEOMETRY STREAM IS FILLED BY THE HOST (host-visible coherent memory), so its bytes are there whether or not a
    // submission ran - the fence decides who may RELY on them, not whether they exist. A TEXTURE is the other kind: its
    // image is filled by a copy the GPU records, so a batch that never ran leaves nothing to sample.
    EXPECT_EQ(ran.uploads->stream_uploads, 0U) << "a host-filled stream does not wait for a submission";
    EXPECT_EQ(ran.uploads->texture_uploads, 1U) << "a texture the failed batch never filled was published as resident";

    // AND THE PICTURE IS THE TEXTURE'S: the upload the frame recorded itself ran.
    const ColorReadback read = backend->readColorImage(scene.target.get(), 0);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), kTextureColour)
        << "the frame sampled a texture the failed preparation believed it had filled";

    // NOW A FRAME THAT NEVER REACHES THE QUEUE (P17): its commands are dropped, and the records it advanced go back to
    // `Undefined` - which the readback's NotReady hint is the one observable form of.
    device->refuseNextSubmit();
    ASSERT_EQ(backend->executeFrame(scene.frame, nullptr), PresentOutcome::Skipped);
    const ColorReadback after_refusal = backend->readColorImage(scene.target.get(), 0);
    EXPECT_FALSE(after_refusal.ok()) << "a frame that never ran left a picture a reader can vouch for";
    EXPECT_EQ(after_refusal.status, ReadbackStatus::NotReady);

    // AND THE SAME FRAME RUNS AGAIN, CLEARING WHAT THE REWOUND RECORD SAYS IS UNDEFINED.
    ASSERT_EQ(backend->executeFrame(scene.frame, nullptr), PresentOutcome::Skipped);
    const ColorReadback reread = backend->readColorImage(scene.target.get(), 0);
    ASSERT_TRUE(reread.ok());
    EXPECT_EQ(pixelAt(reread.values, kTargetSize / 2, kTargetSize / 2), kTextureColour) << "the frame after the rewound one did not draw again";

    EXPECT_EQ(backend->diagnosticCount(vn::graphics::DiagnosticCategory::InitFailed), 0U);
    backend->shutdown();
}
