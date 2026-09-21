/**
 * @brief The forward LIGHT BLOCK: the lights a drawing call announced reach its shader in VIEW space
 * (see `.ai/design/vsg-reimplementation.md`, milestone M5c).
 *
 * Why this is a picture and not a byte comparison. A light that never reached the shader and a scene lit only by
 * the ambient fill look the same to every counter: the draw is recorded, the block is bound, the pipeline is the
 * same one. So the device case below shades a FOUR-BAND picture in ONE pass, with one drawing call per band, and
 * each band's light list is chosen so that only the correct packing can produce its colour:
 *
 *   * band 0 announces a sun pointing AT the viewer (world +Z, camera at +Z): the band must be ambient + sun;
 *   * band 1 announces the same sun the other way (world -Z): the band must be ambient only - the direction's
 *     SIGN is the claim, and a packing that lost it would light both bands;
 *   * bands 2 and 3 announce a light the block cannot carry (disabled): both must be the ambient FILL, and the
 *     two of them together must produce exactly ONE diagnostic - the drop report is an episode, not a per-call
 *     sentence.
 *
 * The bands are also what pins "the block is per DRAWING CALL": one pass, four calls, four different pictures -
 * a per-pass block would paint all four bands with the last announcement's lights.
 *
 * Device-free by construction: the packing is a function of the announced lights and the camera, so its arithmetic
 * (the world -> view rotation, the slot order, the fill, the count) is pinned without a device first.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/LightBlock.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::graphics::Camera;
using vine::graphics::Geometry;
using vine::graphics::Light;
using vine::graphics::LightType;
using vine::graphics::Material;
using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::buildGeometryFacts;
using vine::vsg::buildMaterialFacts;
using vine::vsg::buildProgramFacts;
using vine::vsg::buildScreenProgramFacts;
using vine::vsg::ContentDraw;
using vine::vsg::ContentFacts;
using vine::vsg::ContentPass;
using vine::vsg::ContentPipeline;
using vine::vsg::FactMiss;
using vine::vsg::GeometryFacts;
using vine::vsg::kLightDirectionalSlots;
using vine::vsg::MaterialFacts;
using vine::vsg::OffscreenTarget;
using vine::vsg::LightPushBlock;
using vine::vsg::packLightBlock;
using vine::vsg::packLightPushBlock;
using vine::vsg::PassContent;
using vine::vsg::ProgramFacts;
using vine::vsg::StreamUploads;
using vine::vsg::VineLightsBlock;
using vine::vsg::VsgExecutor;
using vine::vsg::core::CameraSnapshot;
using vine::vsg::core::CompiledDraw;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::DrawKind;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::LightRef;
using vine::vsg::core::Observe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::VariantPool;

namespace
{

constexpr std::uint32_t kSize = 64;

/// @brief The device case's clear colour: what a band that drew nothing would show.
constexpr float kGreen[4]{ 0.0F, 0.75F, 0.0F, 1.0F };

/// @brief One announced light as the frame remembers it (the plan's own type).
LightRef makeLight(LightType type, double r, double g, double b, float intensity, bool enabled = true)
{
    LightRef light;
    light.enabled   = enabled;
    light.type      = type;
    light.color     = vine::Colorf(r, g, b, 1.0);
    light.intensity = intensity;
    return light;
}

/// @brief A camera at +Z looking at the origin, up +Y: its view matrix is the world's own axes.
///
/// Built here rather than snapshotted from a `Camera` because the recorder's snapshot helper is the recorder's
/// own business (private); the device case below does announce a real `Camera`, so the two sources meet there.
CameraSnapshot cameraAtPlusZ()
{
    CameraSnapshot camera;
    camera.present = true;
    camera.view    = vine::math::Mat4d{};  // identity: right = +X, up = +Y, backward = +Z
    camera.eye     = vine::math::Vec3d(0.0, 0.0, 5.0);
    return camera;
}

/// @brief A camera at the origin looking along +X, up +Y: its right is world +Z and its backward world -X.
CameraSnapshot cameraLookingAlongPlusX()
{
    CameraSnapshot camera;
    camera.present      = true;
    camera.view         = vine::math::Mat4d{};
    camera.view(0, 0)   = 0.0;
    camera.view(0, 2)   = 1.0;   // right = +Z
    camera.view(2, 0)   = -1.0;  // backward = -X
    camera.view(2, 2)   = 0.0;
    return camera;
}

/// @brief The fragments of the device case: the block's ambient plus every lit directional, against a normal
/// facing the viewer.
///
/// No `normalize` in the loop on purpose: the packing normalizes the direction, so an unnormalized value arriving
/// in the shader means the packing lost that step - and the announced direction is deliberately not unit length.
/// A zero direction contributes nothing (and must not produce a NaN).
const char* const kLightingFragment =
    "layout(set = 0, binding = 3, std140) uniform VineLightsBlock {\n"
    "    vec4 ambient;\n"
    "    vec4 sun_dir[3];\n"
    "    vec4 sun_color[3];\n"
    "} lights;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main() {\n"
    "    vec3 n = vec3(0.0, 0.0, 1.0);\n"
    "    vec3 lit = lights.ambient.rgb * lights.ambient.a;\n"
    "    for (int i = 0; i < 3; ++i) {\n"
    "        lit += lights.sun_color[i].rgb * lights.sun_color[i].a * max(dot(n, lights.sun_dir[i].xyz), 0.0);\n"
    "    }\n"
    "    outColor = vec4(lit, 1.0);\n"
    "}\n";

}  // namespace

TEST(LightBlockTest, TheBlockCarriesTheLightsInViewSpace)
{
    // The camera sits at +Z looking at the origin, so view space and world space share their axes - which is
    // exactly why the interesting case below uses a ROTATED camera: with this one a packing that forgot the
    // rotation would look correct.
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    const LightRef lights[]{
        makeLight(LightType::Ambient, 0.2, 0.3, 0.4, 0.5F),
        makeLight(LightType::Directional, 0.8, 0.4, 0.2, 2.0F),
        makeLight(LightType::Directional, 0.1, 0.5, 0.9, 1.0F),
    };
    LightRef sun  = lights[1];
    LightRef side = lights[2];
    sun.has_direction  = true;
    sun.direction      = vine::math::Vec3d(0.0, 0.0, 1.0);  // towards the viewer.
    side.has_direction = true;
    side.direction     = vine::math::Vec3d(2.0, 0.0, 0.0);  // NOT unit length: the packing normalizes it.

    const LightRef announced[]{ lights[0], sun, side };
    VineLightsBlock block;
    const std::size_t represented = packLightBlock(announced, camera, block);

    EXPECT_EQ(represented, 3U) << "one ambient plus two directional lights are all packed";
    EXPECT_FLOAT_EQ(block.ambient[0], 0.2F);
    EXPECT_FLOAT_EQ(block.ambient[1], 0.3F);
    EXPECT_FLOAT_EQ(block.ambient[2], 0.4F);
    EXPECT_FLOAT_EQ(block.ambient[3], 0.5F) << "the ambient slot carries rgb + intensity";

    // Slot 0, in announcement order: a world +Z direction stays view +Z for this camera.
    EXPECT_NEAR(block.dirs[0][0], 0.0F, 1e-6F);
    EXPECT_NEAR(block.dirs[0][1], 0.0F, 1e-6F);
    EXPECT_NEAR(block.dirs[0][2], 1.0F, 1e-6F);
    EXPECT_FLOAT_EQ(block.dirs[0][3], 0.0F) << "the direction's w is unused";
    EXPECT_FLOAT_EQ(block.cols[0][0], 0.8F);
    EXPECT_FLOAT_EQ(block.cols[0][3], 2.0F) << "rgb + intensity, like the ambient slot";

    // Slot 1: a world +X direction lands on view +X, and the length-2 direction comes out UNIT length - the
    // shading compares the direction against a normal, so an unnormalized one would scale the light.
    EXPECT_NEAR(block.dirs[1][0], 1.0F, 1e-6F);
    EXPECT_NEAR(block.dirs[1][1], 0.0F, 1e-6F);
    EXPECT_NEAR(block.dirs[1][2], 0.0F, 1e-6F);
    EXPECT_FLOAT_EQ(block.cols[1][1], 0.5F);

    // Nothing announced a third directional light, so the last slot stays empty.
    EXPECT_FLOAT_EQ(block.dirs[2][2], 0.0F);
    EXPECT_FLOAT_EQ(block.cols[2][0], 0.0F);
}

TEST(LightBlockTest, TheDirectionsAreRotatedIntoViewSpace)
{
    // The camera looks along +X instead of -Z, so a light that travels along the world's +X axis travels AWAY
    // from this camera: its view-space direction is view -Z. A packing that forgot the rotation would leave the
    // world direction in the block, and this is the case that tells the two apart.
    const CameraSnapshot looking_along_x = cameraLookingAlongPlusX();
    ASSERT_TRUE(looking_along_x.present);

    LightRef along;  // world +X
    along.enabled       = true;
    along.type          = LightType::Directional;
    along.color         = vine::Colorf(1.0, 1.0, 1.0, 1.0);
    along.intensity     = 1.0F;
    along.has_direction = true;
    along.direction     = vine::math::Vec3d(1.0, 0.0, 0.0);

    LightRef against = along;
    against.direction = vine::math::Vec3d(-1.0, 0.0, 0.0);

    LightRef up = along;
    up.direction = vine::math::Vec3d(0.0, 1.0, 0.0);

    const LightRef announced[]{ along, against, up };
    VineLightsBlock block;
    ASSERT_EQ(packLightBlock(announced, looking_along_x, block), 3U);

    EXPECT_NEAR(block.dirs[0][2], -1.0F, 1e-6F) << "a light travelling along +X goes AWAY from this camera";
    EXPECT_NEAR(block.dirs[0][0], 0.0F, 1e-6F);
    EXPECT_NEAR(block.dirs[1][2], 1.0F, 1e-6F) << "and the opposite light travels towards it";
    EXPECT_NEAR(block.dirs[2][1], 1.0F, 1e-6F) << "the camera's up is still the world's up";
    EXPECT_NEAR(block.dirs[2][2], 0.0F, 1e-6F) << "with no leakage between the axes";
}

TEST(LightBlockTest, DisabledAndUnusableLightsAreNotPacked)
{
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    LightRef disabled_ambient   = makeLight(LightType::Ambient, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightRef disabled_sun       = makeLight(LightType::Directional, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightRef point              = makeLight(LightType::Point, 1.0, 0.0, 0.0, 1.0F);  // a kind the block has no slot for
    LightRef sun                = makeLight(LightType::Directional, 0.25, 0.5, 0.75, 1.0F);
    sun.has_direction           = true;
    sun.direction               = vine::math::Vec3d(0.0, 0.0, 1.0);

    const LightRef announced[]{ disabled_ambient, disabled_sun, point, sun };
    VineLightsBlock block;
    const std::size_t represented = packLightBlock(announced, camera, block);

    EXPECT_EQ(represented, 1U) << "only the enabled directional light is lit";
    EXPECT_NEAR(block.dirs[0][2], 1.0F, 1e-6F) << "and it is the one in slot 0";
    EXPECT_NEAR(block.dirs[1][2], 0.0F, 1e-6F);
    EXPECT_NEAR(block.cols[0][0], 0.25F, 1e-6F);

    // No enabled ambient light was announced, so the block carries the fill - but the fill is NOT a light: the
    // count above stays about what the host announced.
    EXPECT_FLOAT_EQ(block.ambient[0], 0.15F);
    EXPECT_FLOAT_EQ(block.ambient[1], 0.15F);
    EXPECT_FLOAT_EQ(block.ambient[2], 0.15F);
    EXPECT_FLOAT_EQ(block.ambient[3], 1.0F);
}

TEST(LightBlockTest, AnEmptyAnnouncementIsLitByTheFillAndCountsNothing)
{
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    VineLightsBlock block;
    const std::size_t represented = packLightBlock(std::span<const LightRef>{}, camera, block);

    EXPECT_EQ(represented, 0U) << "nothing was announced, so nothing is dropped either";
    EXPECT_FLOAT_EQ(block.ambient[0], 0.15F) << "and the scene is still visible instead of multiplied by zero";
    for (std::size_t slot = 0; slot < kLightDirectionalSlots; ++slot)
    {
        EXPECT_FLOAT_EQ(block.dirs[slot][0], 0.0F);
        EXPECT_FLOAT_EQ(block.dirs[slot][3], 0.0F);
    }
}

TEST(LightBlockTest, OnlyThreeDirectionalLightsGetSlots)
{
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    LightRef first  = makeLight(LightType::Directional, 0.1, 0.0, 0.0, 1.0F);
    LightRef second = makeLight(LightType::Directional, 0.2, 0.0, 0.0, 1.0F);
    LightRef third  = makeLight(LightType::Directional, 0.3, 0.0, 0.0, 1.0F);
    LightRef fourth = makeLight(LightType::Directional, 0.4, 0.0, 0.0, 1.0F);
    for (LightRef* light : { &first, &second, &third, &fourth })
    {
        light->has_direction = true;
        light->direction     = vine::math::Vec3d(0.0, 0.0, 1.0);
    }

    const LightRef announced[]{ first, second, third, fourth };
    VineLightsBlock block;
    EXPECT_EQ(packLightBlock(announced, camera, block), 3U)
        << "the block holds three: the caller reports the difference once per episode";
    EXPECT_FLOAT_EQ(block.cols[0][0], 0.1F);
    EXPECT_FLOAT_EQ(block.cols[1][0], 0.2F);
    EXPECT_FLOAT_EQ(block.cols[2][0], 0.3F) << "in announcement order, and the fourth has no slot";
}

TEST(LightBlockTest, ACameraThatWasNeverEstablishedLeavesTheBlockEmpty)
{
    // No camera means no view space to light in: the block stays empty (black), NOT the ambient fill. That is the
    // reference behaviour and it is visible - a pass that announced no camera is unlit rather than lit by a guess.
    const CameraSnapshot none;
    ASSERT_FALSE(none.present);

    LightRef sun                = makeLight(LightType::Directional, 1.0, 1.0, 1.0, 1.0F);
    LightRef ambient            = makeLight(LightType::Ambient, 0.5, 0.5, 0.5, 1.0F);
    sun.has_direction           = true;
    sun.direction               = vine::math::Vec3d(0.0, 0.0, 1.0);
    const LightRef announced[]{ ambient, sun };

    VineLightsBlock block;
    block.ambient[0] = 9.0F;  // a value the packing must overwrite
    EXPECT_EQ(packLightBlock(announced, none, block), 0U);
    EXPECT_FLOAT_EQ(block.ambient[0], 0.0F) << "every field is written, the fill included";
    EXPECT_FLOAT_EQ(block.dirs[0][2], 0.0F);
}

TEST(LightBlockTest, TheLightsReachTheFragmentStagePerDrawingCall)
{
    // THE PICTURE. One pass, four drawing calls, four bands of 16 pixels; each band's light list is chosen so that
    // only a correct packing, bound PER CALL, can explain its colour (see the file note).
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        layout.clear_color[index] = kGreen[index];
    }
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);

    // The camera every call announces: at +Z, looking at the origin. A world +Z light therefore travels TOWARDS
    // the viewer, which is the direction that lights the fragment stage's fixed normal.
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));

    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(location = 0) in vec3 position;\n"
            "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(kLightingFragment));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::create(created.device, *storage);
    ASSERT_NE(descriptors, nullptr);

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> pipelines =
        ContentPipeline::create(descriptors->layout(), std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                program_facts.shaders, settings);
    ASSERT_NE(pipelines, nullptr);

    // Four bands: one triangle each, its base along the TOP rows of the target (NDC y = -1) and its apex at the
    // bottom (NDC y = +1) - the framebuffer's origin is the upper left, so NDC y = +1 lands on the last row.
    const auto make_band = [](float left, float right) {
        auto indices = vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
        auto geometry      = vine::intrusive_ptr<Geometry>(new Geometry());
        const float centre = (left + right) * 0.5F;
        geometry->setPositions(vine::intrusive_ptr<const vine::Buffer<float>>(new vine::Buffer<float>(
            std::vector<float>{ left, -1.0F, 0.0F, right, -1.0F, 0.0F, centre, 1.0F, 0.0F })));
        geometry->setIndices(indices);
        geometry->setRevision(1U);
        return geometry;
    };
    const vine::intrusive_ptr<Geometry> bands[]{ make_band(-1.0F, -0.5F), make_band(-0.5F, 0.0F),
                                                 make_band(0.0F, 0.5F), make_band(0.5F, 1.0F) };

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    GeometryFacts                        geometry_facts[4];
    std::vector<vine::vsg::ChannelFacts> geometry_channels[4];
    for (std::size_t index = 0; index < 4U; ++index)
    {
        ASSERT_EQ(buildGeometryFacts(*bands[index], geometry_facts[index], geometry_channels[index]), FactMiss::None);
    }
    // The four bands share one program and one vertex layout, so they are one compiled half.
    ASSERT_TRUE(geometry_facts[0].layout == geometry_facts[1].layout);

    const ProgramFacts  programs[]{ program_facts };
    const GeometryFacts geometries[]{ geometry_facts[0], geometry_facts[1], geometry_facts[2], geometry_facts[3] };
    const MaterialFacts materials[]{ material_facts };
    ContentFacts        facts;
    facts.programs   = programs;
    facts.geometries = geometries;
    facts.materials  = materials;

    // The four announcements. Bands 0 and 1 differ ONLY in the sun's sign; bands 2 and 3 announce a light the block
    // cannot carry (a disabled one) and must be the ambient fill - with ONE report between them.
    vine::intrusive_ptr<Light> ambient = Light::createAmbient();
    ambient->setColor(vine::Colorf(0.1, 0.1, 0.1, 1.0));
    ambient->setIntensity(1.0F);
    vine::intrusive_ptr<Light> sun_towards = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, 1.0));
    sun_towards->setColor(vine::Colorf(0.4, 0.0, 0.0, 1.0));
    sun_towards->setIntensity(1.0F);
    vine::intrusive_ptr<Light> sun_away = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    sun_away->setColor(vine::Colorf(0.4, 0.0, 0.0, 1.0));
    sun_away->setIntensity(1.0F);
    vine::intrusive_ptr<Light> disabled_ambient = Light::createAmbient();
    disabled_ambient->setColor(vine::Colorf(0.9, 0.9, 0.9, 1.0));
    disabled_ambient->setEnabled(false);
    vine::intrusive_ptr<Light> disabled_sun = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, 1.0));
    disabled_sun->setColor(vine::Colorf(0.9, 0.9, 0.9, 1.0));
    disabled_sun->setEnabled(false);

    const Light* towards_lights[]{ ambient.get(), sun_towards.get() };
    const Light* away_lights[]{ ambient.get(), sun_away.get() };
    const Light* disabled_ambient_lights[]{ disabled_ambient.get() };
    const Light* disabled_sun_lights[]{ disabled_sun.get() };

    const vine::intrusive_ptr<RenderTarget> handle(new RenderTarget());
    TargetFacts                             target_facts;
    target_facts.target        = handle.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape  = target->shape();
    target_facts.current       = target->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    const auto command_of = [&](std::size_t index) {
        RenderCommand command;
        command.geometry = bands[index];
        command.material = material;
        command.program  = program;
        return command;
    };
    const RenderCommand commands[]{ command_of(0), command_of(1), command_of(2), command_of(3) };

    ClearPolicy clear;
    clear.color = true;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        clear.color_value[index] = kGreen[index];
    }

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(handle.get());
    recorder.setClearPolicy(clear);
    recorder.setLights(std::span<const Light* const>(towards_lights, 2U));
    recorder.render(std::span<const RenderCommand>(&commands[0], 1U), &camera);
    recorder.setLights(std::span<const Light* const>(away_lights, 2U));
    recorder.render(std::span<const RenderCommand>(&commands[1], 1U), &camera);
    recorder.setLights(std::span<const Light* const>(disabled_ambient_lights, 1U));
    recorder.render(std::span<const RenderCommand>(&commands[2], 1U), &camera);
    recorder.setLights(std::span<const Light* const>(disabled_sun_lights, 1U));
    recorder.render(std::span<const RenderCommand>(&commands[3], 1U), &camera);
    recorder.endPass();
    recorder.endFrame();

    // A local command vector per call: the recorder takes a span, and the temporary above is one command long.
    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    const std::span<const vine::vsg::core::CompiledDraw> draws = frame.passes[0].draws;
    ASSERT_EQ(draws.size(), 4U);
    EXPECT_EQ(draws[0].lights.size(), 2U) << "the plan carries each call's own announcement";
    EXPECT_EQ(draws[3].lights.size(), 1U) << "the fourth call announced one unusable light";

    storage->beginFrame();
    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   draws_recorder(*pipelines, pool, entry_points);
    StateRegistry registry(pool);

    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        DrawKind::Content, program.get(), program_facts.revision, geometry_facts[0].layout, pipelines.get(),
        &draws_recorder } };
    ContentPass::Scope              scope;
    scope.entries     = halves;
    scope.registry    = &registry;
    scope.storage     = storage.get();
    scope.descriptors = descriptors.get();
    scope.uploads     = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "nothing may be refused: every band is a complete content draw";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ChannelIgnored), 1U)
        << "two calls announced an unusable light and the drop report is one episode, not one sentence per call";

    VsgExecutor executor(diagnostics);
    executor.addTarget(handle.get(), target.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{ PassContent{ 1U, node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    // The four bands, read back: ambient + sun towards the viewer, ambient alone when the sun points away, and
    // the block's fill for the two calls whose light the block could not carry.
    const auto near = [](std::uint8_t byte, double value) {
        return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0;
    };
    const auto band_of = [&](int x) { return target->probe().pixel(x, 32); };
    const Rgba8 lit = band_of(8);
    EXPECT_TRUE(near(lit.r, 0.5) && near(lit.g, 0.1) && near(lit.b, 0.1))
        << "band 0 is ambient + the sun pointing at the viewer, got (" << static_cast<int>(lit.r) << ", "
        << static_cast<int>(lit.g) << ", " << static_cast<int>(lit.b) << ")";

    const Rgba8 away = band_of(24);
    EXPECT_TRUE(near(away.r, 0.1) && near(away.g, 0.1) && near(away.b, 0.1))
        << "band 1 is ambient alone: the same sun the other way lights nothing, got (" << static_cast<int>(away.r)
        << ", " << static_cast<int>(away.g) << ", " << static_cast<int>(away.b) << ")";

    const Rgba8 fill = band_of(40);
    EXPECT_TRUE(near(fill.r, 0.15) && near(fill.g, 0.15) && near(fill.b, 0.15))
        << "band 2 announced a disabled light, so it is the block's ambient fill, got (" << static_cast<int>(fill.r)
        << ", " << static_cast<int>(fill.g) << ", " << static_cast<int>(fill.b) << ")";

    const Rgba8 fill_again = band_of(56);
    EXPECT_TRUE(near(fill_again.r, 0.15) && near(fill_again.g, 0.15) && near(fill_again.b, 0.15))
        << "band 3 announced another unusable light and is the fill as well, got (" << static_cast<int>(fill_again.r)
        << ", " << static_cast<int>(fill_again.g) << ", " << static_cast<int>(fill_again.b) << ")";

    // The gaps between the bands are the pass' own clear, which is what makes the four values above a PICTURE:
    // the bands are not "the whole target painted somehow" - they sit on a background the shading never wrote.
    const Rgba8 background = target->probe().pixel(2, 32);
    EXPECT_TRUE(background.g > 150 && background.r < 40 && background.b < 40)
        << "outside the four triangles the pass keeps its clear, got (" << static_cast<int>(background.r) << ", "
        << static_cast<int>(background.g) << ", " << static_cast<int>(background.b) << ")";
}

TEST(LightBlockTest, ThePushBlockIsTheSameLightsInThePushesOwnLayout)
{
    // The full-screen path spends its whole 128-byte push range on the lights (it needs no view matrices, so unlike
    // the forward path it has nothing else to spend it on), and the values have to be the ones the forward UBO
    // carries: the SDK's two lighting programs light the same scene, so a second walk with different rules would
    // shade a scene differently depending on which pass draws it.
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    LightRef      ambient = makeLight(LightType::Ambient, 0.2, 0.3, 0.4, 0.5F);
    LightRef      sun     = makeLight(LightType::Directional, 0.8, 0.4, 0.2, 2.0F);
    sun.has_direction     = true;
    sun.direction         = vine::math::Vec3d(0.0, 0.0, 1.0);
    const LightRef announced[]{ ambient, sun };

    LightPushBlock    push;
    const std::size_t represented = packLightPushBlock(announced, camera, push);

    VineLightsBlock ubo;
    ASSERT_EQ(packLightBlock(announced, camera, ubo), represented) << "one packing, two layouts";
    EXPECT_EQ(represented, 2U);
    EXPECT_EQ(push.ambient, ubo.ambient);
    for (std::size_t slot = 0; slot < kLightDirectionalSlots; ++slot)
    {
        EXPECT_EQ(push.dirs[slot], ubo.dirs[slot]) << "slot " << slot << " of the directions";
        EXPECT_EQ(push.cols[slot], ubo.cols[slot]) << "slot " << slot << " of the colours";
    }

    // The reservation stays zero: no shipped program reads it, and a value nobody reads is a promise without an
    // effect (see LightPushBlock's declaration).
    for (const float value : push.projparms)
    {
        EXPECT_FLOAT_EQ(value, 0.0F) << "projparms is reserved and stays zero";
    }

    // The ABI itself: the size and the offsets the GLSL block declares.
    EXPECT_EQ(sizeof(LightPushBlock), 128U);
    EXPECT_EQ(offsetof(LightPushBlock, projparms), 16U);
    EXPECT_EQ(offsetof(LightPushBlock, dirs), 32U);
    EXPECT_EQ(offsetof(LightPushBlock, cols), 80U);

    // Same rules as the UBO: no camera means an empty push (not the fill), an empty list means the fill.
    LightPushBlock empty;
    EXPECT_EQ(packLightPushBlock(announced, CameraSnapshot{}, empty), 0U);
    for (const float value : empty.ambient)
    {
        EXPECT_FLOAT_EQ(value, 0.0F) << "no camera: nothing to rotate into, so nothing is pushed";
    }
    LightPushBlock filled;
    EXPECT_EQ(packLightPushBlock(std::span<const LightRef>{}, camera, filled), 0U);
    EXPECT_FLOAT_EQ(filled.ambient[0], 0.15F) << "an empty announcement keeps the scene visible";
}

TEST(LightBlockTest, ThePushReachesTheFragmentStagePerFullScreenCall)
{
    // THE PICTURE. Three full-screen calls in ONE pass, each with its own viewport (a third of the target) and its
    // own light list: a sun pointing at the viewer, the same sun reversed, and no announcement at all. The fragment
    // stage reads the PUSH (its only input), so each band's colour is its own call's announcement:
    //
    //   band 0: ambient + the sun towards the viewer -> 0.5 / 0.1 / 0.1
    //   band 1: the same light the other way         -> 0.1 / 0.1 / 0.1 (ambient alone)
    //   band 2: nothing announced                    -> 0.15 (the fill, so the band is visible at all)
    //
    // A push recorded once per PASS (all three calls sharing the first announcement), a zeroed push, or one pushed
    // to the wrong stage moves at least one band, and the destination's clear (blue) is what a band that drew
    // nothing would show.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    constexpr float kBlue[4]{ 0.0F, 0.0F, 0.75F, 1.0F };
    OffscreenTarget::Layout destination_layout;
    destination_layout.width  = kSize;
    destination_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        destination_layout.clear_color[index] = kBlue[index];
    }
    std::unique_ptr<OffscreenTarget> destination = OffscreenTarget::create(created.device, destination_layout);
    ASSERT_NE(destination, nullptr);

    // The program: the engine's canonical full-screen vertex stage (buildScreenProgramFacts composes it) with a
    // fragment stage that shades from the push.
    const vine::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vine::String(reinterpret_cast<const char8_t*>(
            "layout(push_constant, std140) uniform PushConstants {\n"
            "    vec4 ambient;\n"
            "    vec4 projparms;\n"
            "    vec4 sun_dir[3];\n"
            "    vec4 sun_color[3];\n"
            "} pc;\n"
            "layout(location = 0) out vec4 outColor;\n"
            "void main() {\n"
            "    vec3 n = vec3(0.0, 0.0, 1.0);\n"
            "    vec3 lit = pc.ambient.rgb * pc.ambient.a;\n"
            "    for (int i = 0; i < 3; ++i) {\n"
            "        lit += pc.sun_color[i].rgb * pc.sun_color[i].a * max(dot(n, pc.sun_dir[i].xyz), 0.0);\n"
            "    }\n"
            "    outColor = vec4(lit, 1.0);\n"
            "}\n"));
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildScreenProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::createScreen(program_facts.shaders);
    ASSERT_NE(pipelines, nullptr);

    // The camera every call announces: at +Z looking at the origin, so a world +Z direction is a light towards the
    // viewer (which lights the fixed normal) and the reversed one does not.
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));

    vine::intrusive_ptr<Light> ambient = Light::createAmbient();
    ambient->setColor(vine::Colorf(0.1, 0.1, 0.1, 1.0));
    vine::intrusive_ptr<Light> towards = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, 1.0));
    towards->setColor(vine::Colorf(0.4, 0.0, 0.0, 1.0));
    vine::intrusive_ptr<Light> away = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    away->setColor(vine::Colorf(0.4, 0.0, 0.0, 1.0));

    const Light* band0_lights[]{ ambient.get(), towards.get() };
    const Light* band1_lights[]{ ambient.get(), away.get() };

    const vine::intrusive_ptr<RenderTarget> handle(new RenderTarget());
    TargetFacts                             target_facts;
    target_facts.target        = handle.get();
    target_facts.wanted.width  = static_cast<int>(kSize);
    target_facts.wanted.height = static_cast<int>(kSize);
    target_facts.wanted.shape  = destination->shape();
    target_facts.current       = destination->instance();
    const std::vector<TargetFacts> target_table{ target_facts };

    FrameArena    arena{ 64 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);
    recorder.setRenderTarget(handle.get());
    const std::uint32_t band_width = kSize / 3U;  // 21 / 21 / 22: the last band takes the remainder
    recorder.setViewport(0, 0, static_cast<int>(band_width), static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>(band0_lights, 2U));
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.setViewport(static_cast<int>(band_width), 0, static_cast<int>(band_width), static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>(band1_lights, 2U));
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.setViewport(static_cast<int>(band_width * 2U), 0, static_cast<int>(kSize - band_width * 2U),
                         static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>{});  // the backend default: the fill
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 3U);
    EXPECT_EQ(frame.passes[0].draws[0].lights.size(), 2U) << "each call carries its own announcement";
    EXPECT_EQ(frame.passes[0].draws[2].lights.size(), 0U);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::create(created.device, *storage);
    ASSERT_NE(descriptors, nullptr);

    storage->beginFrame();
    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   draws(*pipelines, pool, entry_points);
    StateRegistry registry(pool);

    const ContentPass::Scope::Entry halves[]{ ContentPass::Scope::Entry{
        DrawKind::Screen, program_facts.program, program_facts.revision, {}, pipelines.get(), &draws } };
    ContentPass::Scope              scope;
    scope.entries     = halves;
    scope.registry    = &registry;
    scope.storage     = storage.get();
    scope.descriptors = descriptors.get();
    scope.uploads     = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  node;
    const ContentFacts           facts;  // a screen call reads no tables: its half is looked up by program identity
    ASSERT_TRUE(content.record(frame.passes[0], facts, destination->shape().compatibility(), {}, view_block, node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U);
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ChannelIgnored), 0U)
        << "the drop report belongs to the content path: a full-screen call packs what fits and says nothing";

    VsgExecutor executor(diagnostics);
    executor.addTarget(handle.get(), destination.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{ PassContent{ 1U, node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double value) {
        return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0;
    };
    const auto band = [&](int x) { return destination->probe().pixel(x, 32); };

    const Rgba8 lit = band(8);
    EXPECT_TRUE(near(lit.r, 0.5) && near(lit.g, 0.1) && near(lit.b, 0.1))
        << "band 0 is the push's ambient + the sun towards the viewer, got (" << static_cast<int>(lit.r) << ", "
        << static_cast<int>(lit.g) << ", " << static_cast<int>(lit.b) << ")";

    const Rgba8 away_band = band(24);
    EXPECT_TRUE(near(away_band.r, 0.1) && near(away_band.g, 0.1) && near(away_band.b, 0.1))
        << "band 1 is ambient alone: the same push layout, the direction reversed, got ("
        << static_cast<int>(away_band.r) << ", " << static_cast<int>(away_band.g) << ", "
        << static_cast<int>(away_band.b) << ")";

    const Rgba8 fill = band(54);
    EXPECT_TRUE(near(fill.r, 0.15) && near(fill.g, 0.15) && near(fill.b, 0.15))
        << "band 2 announced nothing and is the fill, got (" << static_cast<int>(fill.r) << ", "
        << static_cast<int>(fill.g) << ", " << static_cast<int>(fill.b) << ")";
}
