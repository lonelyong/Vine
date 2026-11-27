/**
 * @brief Y9: the BACKGROUND's depth contract - what the sky is at, which camera it belongs to, and which shell of the
 *        box the rays leave through.
 *
 * WHAT THE TWO CASES PIN, and why each is a different question:
 *   - THE FAR PLANE (perspective): the background is drawn from a UNIT box, while the content is a quad FIFTY units
 *     away and well inside the far plane. The quad must win, and the sky must fill the rest of the frame. A sky that
 *     sat at the box's own distance (the pre-Y9 shader) would cover that quad and would be a tiny square in the middle
 *     of the frame, because the camera here stands five units from the box - both halves of "the box's size and the
 *     camera's distance are not part of the picture" are in these two assertions.
 *   - THE CULLED NEAR SHELL (orthographic): the camera's matrices are the identity, so every vertex has w = 1 - an
 *     orthographic view, where nothing is clipped away by w. Both shells of the box then land on the same pixels at the
 *     same depth, and the depth test cannot choose between them: only `CullMode::Front` can. The map gives the -Z face
 *     and the +Z (antipodal) face different colours, so "the picture is the hemisphere facing away from the eye" is not
 *     an opinion - it is a pixel that must not be there.
 *
 * THE SKY IS STATED AS THE SDK STATES IT: `skyboxProgram()` for the text and `skyboxRenderState()` for the state, folded
 * onto the command the way content folds it (`DrawItem::authored_state`). The box is the shape the chain's builder makes
 * (a unit direction box), so the pairing under test is the one a host or a chain actually draws with.
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
#include <vine/geom/spatial/BoxSolid.hpp>
#include <vine/geom/spatial/SolidTessellation.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/DepthPolicy.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderState.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

#include "PlanTestSupport.hpp"

using vn::BufferSlice;
using vn::Colorf;
using vn::intrusive_ptr;
using vn::String;
using vn::graphics::AttachmentPolicy;
using vn::graphics::CameraSnapshot;
using vn::graphics::ColorReadback;
using vn::graphics::DepthPolicy;
using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::DrawItem;
using vn::graphics::DrawKind;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::Geometry;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::PresentOutcome;
using vn::graphics::Primitive;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::TextureSlot;

namespace
{

constexpr int kTargetSize = 64;

/// @brief The clear the frame states, chosen so it is none of the six map faces: a pixel that is still this colour is a
///        pixel the sky did not cover.
constexpr std::array<int, 4> kClear{ 0, 0, 0, 255 };

/// @brief The colours of the cube map's six faces, in `CubeMap::Face` order (Vulkan's layer order).
constexpr std::uint8_t kFaceColors[6][4]{
    { 255U, 0U, 0U, 255U },       // PosX: red
    { 0U, 255U, 0U, 255U },       // NegX: green
    { 0U, 0U, 255U, 255U },       // PosY: blue
    { 255U, 255U, 0U, 255U },     // NegY: yellow
    { 255U, 0U, 255U, 255U },     // PosZ: magenta - the ANTIPODAL face of a camera looking down -Z
    { 0U, 255U, 255U, 255U },     // NegZ: cyan - the face a camera looking down -Z must sample
};
constexpr std::array<int, 4> kAntipodal{ 255, 0, 255, 255 };
constexpr std::array<int, 4> kForward{ 0, 255, 255, 255 };
/// @brief The colour the content quad writes (a solid fragment stage, see kSolidFragmentStage).
constexpr std::array<int, 4> kContent{ 0, 255, 0, 255 };

/// @brief 60 degrees in radians: `vn::math::perspective` takes the vertical field of view in radians.
constexpr double kFovY = 1.04719755119659758;

/// @brief A 64x64 RGBA8 + D24 target: the depth attachment is what the background's test needs.
[[nodiscard]] intrusive_ptr<RenderTarget> makeTarget()
{
    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->setName(String(u8"vk_background_target"));
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D24);
    target->setSize(kTargetSize, kTargetSize);
    return target;
}

/// @brief A camera whose matrices are identities: every vertex keeps w = 1, which is the ORTHOGRAPHIC shape.
[[nodiscard]] CameraSnapshot identityCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};
    return camera;
}

/// @brief A perspective camera standing five units back from the world origin, looking at it.
[[nodiscard]] CameraSnapshot perspectiveCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.eye        = vn::math::Vec3d(0.0, 0.0, 5.0);
    camera.view       = vn::math::lookAt(vn::math::Point3d(0.0, 0.0, 5.0), vn::math::Point3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));
    camera.projection = vn::math::perspective<double>(kFovY, 1.0, 0.1, 100.0);
    return camera;
}

/// @brief A cube map whose six faces are six different colours, so a sampled pixel says WHICH face was chosen.
[[nodiscard]] intrusive_ptr<vn::graphics::CubeMap> makeCubeMap()
{
    constexpr int                              kFaceSize = 4;
    const intrusive_ptr<vn::graphics::CubeMap> cube(new vn::graphics::CubeMap(kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
    for (int face = 0; face < cube->layerCount(); ++face) {
        auto                       image  = intrusive_ptr<vn::imaging::Image>(new vn::imaging::Image(kFaceSize, kFaceSize, vn::imaging::PixelFormat::Rgba8Unorm));
        const std::span<std::byte> pixels = image->mipData(0);
        for (std::size_t texel = 0; texel < pixels.size(); ++texel) {
            pixels[texel] = static_cast<std::byte>(kFaceColors[face][texel % 4U]);
        }
        cube->setLayer(face, intrusive_ptr<const vn::imaging::Image>(image));
    }
    return cube;
}

/// @brief The unit DIRECTION BOX the chain's builder makes: positions plus a three-scalar texcoord carrying the corner
///        vector the fragment stage samples the map with.
///
/// @note The winding is the tessellation's ("counter-clockwise seen from outside"), which is the convention CullMode
///       speaks about - the reason this test builds the box instead of writing vertices by hand.
[[nodiscard]] intrusive_ptr<Geometry> makeDirectionBox()
{
    const vn::geom::spatial::Triangulationf box = vn::geom::spatial::tessellateSolid(vn::geom::spatial::BoxSolidf{ 1.0F, 1.0F, 1.0F });
    auto                                     geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(vn::graphics::packAttribute(box.positions));
    geometry->setTexcoords3(vn::graphics::packAttribute(box.positions));
    Primitive draw;
    draw.indices = Primitive::IndexSpan::whole(vn::graphics::packIndices(box.indices));
    // setPositions states a non-indexed default draw; the indexed one replaces it (see Geometry::setPositions).
    geometry->setPrimitive(0U, std::move(draw));
    geometry->bumpRevision();
    return geometry;
}

/// @brief A quad facing +Z (the camera), fifty units in front of the eye, large enough to cover the centre of the frame.
[[nodiscard]] intrusive_ptr<Geometry> makeQuad()
{
    constexpr float kHalf   = 15.0F;
    constexpr float kDepth  = -45.0F; // the camera stands at z = +5, so this is 50 units in front of it
    auto            geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{
        -kHalf, -kHalf, kDepth,
        kHalf,  -kHalf, kDepth,
        kHalf,  kHalf,  kDepth,
        -kHalf, -kHalf, kDepth,
        kHalf,  kHalf,  kDepth,
        -kHalf, kHalf,  kDepth,
    })));
    geometry->bumpRevision();
    return geometry;
}

/// @brief The vertex stage every content program is written against: the 128-byte push range, and nothing else.
constexpr const char8_t* kSolidVertexStage = u8R"(
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

/// @brief A solid green fragment stage: the content's colour, so "the quad is visible" is one comparison.
constexpr const char8_t* kSolidFragmentStage = u8R"(
#version 450
layout(location = 0) out vec4 vn_out_color;
void main()
{
    vn_out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

/// @brief The solid content program (see the two stages above).
[[nodiscard]] intrusive_ptr<ShaderProgram> makeSolidProgram()
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"vk_background_content");
    program->addStage(ShaderStage{ ShaderStageType::Vertex, String(kSolidVertexStage), String(u8"main") });
    program->addStage(ShaderStage{ ShaderStageType::Fragment, String(kSolidFragmentStage), String(u8"main") });
    return program;
}

/// @brief The BACKGROUND command: the SDK's box, program and state, exactly as a host or the chain states them.
[[nodiscard]] DrawItem backgroundCommand(const intrusive_ptr<Geometry>& box, const intrusive_ptr<vn::graphics::CubeMap>& cube)
{
    const intrusive_ptr<ShaderProgram> program = vn::graphics::skyboxProgram();
    auto                               material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf::kWhite);
    material->setTexture(TextureSlot::BaseColor, cube);

    DrawItem command(box, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    // THE OTHER HALF OF THE PAIR: the state `skyboxProgram()` is only correct with (see skyboxRenderState).
    command.authored_state = vn::graphics::skyboxRenderState();
    return command;
}

/// @brief The CONTENT command: the quad through the solid program, with no authored state (the pass decides).
[[nodiscard]] DrawItem contentCommand(const intrusive_ptr<Geometry>& quad, const intrusive_ptr<ShaderProgram>& program)
{
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf(0.0F, 1.0F, 0.0F, 1.0F));

    DrawItem command(quad, nullptr, vn::math::Mat4d{});
    command.program              = program;
    command.program_description  = program->description();
    command.material             = material;
    command.material_description = material->description();
    return command;
}

/// @brief One scene pass that clears to @p kClear and issues @p commands through @p camera.
[[nodiscard]] FrameDescription backgroundFrame(RenderTarget* target, const CameraSnapshot& camera, std::vector<DrawItem> commands)
{
    auto list = std::make_shared<std::vector<DrawItem>>(std::move(commands));

    DrawPacket draw;
    draw.kind          = DrawKind::Scene;
    draw.view.camera   = camera;
    draw.commands      = std::move(list);

    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass  = vn::test::framePassForTest();
    entry.order = 0;

    PassDraw pass;
    pass.target                  = target;
    pass.attachment_policy       = AttachmentPolicy{};
    pass.attachment_policy->color = Colorf(0.0F, 0.0F, 0.0F, 1.0F);
    pass.attachment_policy->clear_depth = true;
    pass.state.depth             = DepthPolicy::TestAndWrite;
    pass.packets.push_back(std::move(draw));

    entry.draw = std::move(pass);
    frame.passes.push_back(std::move(entry));
    return frame;
}

/// @brief The pixel at (@p x, @p y) of a read-back frame, as rgba.
[[nodiscard]] std::array<int, 4> pixelAt(const std::vector<std::uint8_t>& pixels, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x)) * 4U;
    return { pixels[index + 0U], pixels[index + 1U], pixels[index + 2U], pixels[index + 3U] };
}

} // namespace

TEST(VkBackgroundTest, TheBackgroundFollowsTheCameraAndHoldsTheFarPlane)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>         target = makeTarget();
    const intrusive_ptr<vn::graphics::CubeMap> cube   = makeCubeMap();
    const intrusive_ptr<Geometry>             box    = makeDirectionBox();
    const intrusive_ptr<Geometry>             quad   = makeQuad();
    const intrusive_ptr<ShaderProgram>        solid  = makeSolidProgram();

    // THE CONTENT IS FIFTY UNITS AWAY, the background's box is ONE, and the camera stands FIVE units from the box's
    // origin: every one of those three numbers has to stop mattering for the picture to be right.
    std::vector<DrawItem> commands;
    commands.push_back(contentCommand(quad, solid));
    commands.push_back(backgroundCommand(box, cube));

    const PresentOutcome outcome = backend->executeFrame(backgroundFrame(target.get(), perspectiveCamera(), std::move(commands)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the device presents nothing yet; drawing into a target is not presenting";

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok()) << "readback refused: " << static_cast<int>(read.status);
    ASSERT_EQ(read.values.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    // THE FAR PLANE: the quad at 50 units beats the background. A sky drawn at the box's own distance would be nearer
    // than this quad and would cover it.
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), kContent)
        << "the content 50 units away did not survive the background: the sky is not at the far plane";

    // THE CAMERA IT FOLLOWS: the frame is the map, not the clear. The camera stands five units from the box's origin,
    // so a box that did NOT follow the eye would be a small square in the middle of the frame and this pixel would still
    // hold the clear colour. It is also the OUTGOING direction that is sampled (-Z), not the antipodal one.
    EXPECT_EQ(pixelAt(read.values, 8, kTargetSize / 2), kForward) << "the background is not under the camera: the box did not follow the eye";
    EXPECT_EQ(pixelAt(read.values, 2, 2), kForward) << "the background does not reach the corner of the frame";
    EXPECT_NE(pixelAt(read.values, 2, 2), kClear) << "the corner of the frame is still the clear colour";
}

TEST(VkBackgroundTest, AnOrthographicViewKeepsTheShellItsRaysLeaveThrough)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    const intrusive_ptr<RenderTarget>         target = makeTarget();
    const intrusive_ptr<vn::graphics::CubeMap> cube   = makeCubeMap();
    const intrusive_ptr<Geometry>             box    = makeDirectionBox();

    std::vector<DrawItem> commands;
    commands.push_back(backgroundCommand(box, cube));

    // THE IDENTITY CAMERA IS THE ORTHOGRAPHIC CASE: every vertex keeps w = 1, so nothing is clipped away by w and BOTH
    // shells of the box land on the same pixels at the same depth. The depth test cannot separate them - the cull can.
    const PresentOutcome outcome = backend->executeFrame(backgroundFrame(target.get(), identityCamera(), std::move(commands)));
    EXPECT_EQ(outcome, PresentOutcome::Skipped) << "the device presents nothing yet; drawing into a target is not presenting";

    const ColorReadback read = backend->readColorImage(target.get(), 0);
    ASSERT_TRUE(read.ok()) << "readback refused: " << static_cast<int>(read.status);
    ASSERT_EQ(read.values.size(), static_cast<std::size_t>(kTargetSize * kTargetSize) * 4U);

    // THE RAYS LEAVE THROUGH THE -Z FACE: the camera looks down -Z, so the centre pixel is the map's -Z face.
    EXPECT_EQ(pixelAt(read.values, kTargetSize / 2, kTargetSize / 2), kForward)
        << "an orthographic view sampled a face other than the one its ray leaves through";

    // AND THE ANTIPODAL HEMISPHERE IS NOT IN THE PICTURE ANYWHERE: the +Z face is what the near shell would show, and it
    // is the face that a wrong (or missing) cull puts on the screen - the depth test cannot reject it.
    for (int y = 0; y < kTargetSize; ++y) {
        for (int x = 0; x < kTargetSize; ++x) {
            const std::array<int, 4> pixel = pixelAt(read.values, x, y);
            ASSERT_NE(pixel, kAntipodal) << "the background shows the hemisphere facing away from the eye at (" << x << ", " << y << ")";
            ASSERT_NE(pixel, kClear) << "the background left the clear colour at (" << x << ", " << y << ")";
        }
    }
}
