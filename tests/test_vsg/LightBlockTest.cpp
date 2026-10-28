/**
 * @brief The LIGHT BLOCK: the lights a drawing call announced reach its shader in VIEW space, on BOTH drawing
 * paths (see `.ai/design/vsg-backend.md`, milestone M5c, and ADR-2026-10-24-light-block-typed-lights).
 *
 * Why this is a picture and not a byte comparison. A light that never reached the shader and a scene lit only by
 * the ambient fill look the same to every counter: the draw is recorded, the block is bound, the pipeline is the
 * same one. So the device case below shades a FOUR-BAND picture in ONE pass, with one drawing call per band, and
 * each band's light list is chosen so that only the correct packing can produce its colour:
 *
 *   * band 0 announces a sun pointing AT the viewer (world +Z, camera at +Z): the band must be ambient + sun;
 *   * band 1 announces the same sun the other way (world -Z): the band must be ambient only - the direction's
 *     SIGN is the claim, and a packing that lost it would light both bands;
 *   * bands 2 and 3 announce a DISABLED light: both must be the ambient FILL, and the two of them together must
 *     produce no drop report at all - a disabled light is not a dropped one.
 *
 * The bands are also what pins "the block is per DRAWING CALL": one pass, four calls, four different pictures -
 * a per-pass block would paint all four bands with the last announcement's lights.
 *
 * Device-free by construction: the packing is a function of the announced lights and the camera, so its arithmetic
 * (the world -> view rotation, the slot order, the fill, the count) is pinned without a device first.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/FrameRecorder.hpp>
#include <vine/graphics/backend/StateRegistry.hpp>
#include <vine/graphics/backend/VariantPool.hpp>
#include <vine/vsg/internal/BlockDescriptors.hpp>
#include <vine/vsg/internal/BlockStorage.hpp>
#include <vine/vsg/internal/ContentDraw.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/ContentPass.hpp>
#include <vine/vsg/internal/ContentPipeline.hpp>
#include <vine/vsg/internal/ContentSources.hpp>
#include <vine/vsg/internal/Device.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>
#include <vine/vsg/internal/LightBlock.hpp>
#include <vine/vsg/internal/OffscreenTarget.hpp>
#include <vine/vsg/internal/StreamUploads.hpp>
#include <vine/vsg/internal/VsgExecutor.hpp>

#include "TestGeometryBuilders.hpp"

using vn::graphics::Camera;
using vn::graphics::Diagnostics;
using vn::graphics::Geometry;
using vn::graphics::kMaxLights;
using vn::graphics::Light;
using vn::graphics::LightType;
using vn::graphics::Material;
using vn::graphics::RenderCommand;
using vn::graphics::RenderTarget;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::backend::CameraSnapshot;
using vn::graphics::backend::ClearValues;
using vn::graphics::backend::CompiledDraw;
using vn::graphics::backend::CompiledFrame;
using vn::graphics::backend::DrawKind;
using vn::graphics::backend::FrameArena;
using vn::graphics::backend::FrameCompiler;
using vn::graphics::backend::FrameFacts;
using vn::graphics::backend::FrameRecorder;
using vn::graphics::backend::FrameToken;
using vn::graphics::backend::LightRef;
using vn::graphics::backend::Observe;
using vn::graphics::backend::Rgba8;
using vn::graphics::backend::StateRegistry;
using vn::graphics::backend::TargetFacts;
using vn::graphics::backend::VariantPool;
using vn::test::setPrimitiveIndices;
using vn::vsg::AbiBinding;
using vn::vsg::AbiBlockRole;
using vn::vsg::BlockDescriptors;
using vn::vsg::BlockStorage;
using vn::vsg::buildGeometryFacts;
using vn::vsg::buildMaterialFacts;
using vn::vsg::buildProgramFacts;
using vn::vsg::buildScreenProgramFacts;
using vn::vsg::ContentDraw;
using vn::vsg::ContentFacts;
using vn::vsg::ContentPass;
using vn::vsg::ContentPipeline;
using vn::vsg::FactMiss;
using vn::vsg::GeometryFacts;
using vn::vsg::kLightKindEmpty;
using vn::vsg::MaterialFacts;
using vn::vsg::OffscreenTarget;
using vn::vsg::packLightBlock;
using vn::vsg::PassContent;
using vn::vsg::ProgramAbi;
using vn::vsg::ProgramFacts;
using vn::vsg::StreamUploads;
using vn::vsg::VineLightsBlock;
using vn::vsg::VsgExecutor;

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
    light.color     = vn::Colorf(r, g, b, 1.0);
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
    camera.view    = vn::math::Mat4d{}; // identity: right = +X, up = +Y, backward = +Z
    camera.eye     = vn::math::Vec3d(0.0, 0.0, 5.0);
    return camera;
}

/// @brief A camera at the origin looking along +X, up +Y: its right is world +Z and its backward world -X.
CameraSnapshot cameraLookingAlongPlusX()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vn::math::Mat4d{};
    camera.view(0, 0) = 0.0;
    camera.view(0, 2) = 1.0;  // right = +Z
    camera.view(2, 0) = -1.0; // backward = -X
    camera.view(2, 2) = 0.0;
    return camera;
}

/// @brief The fragments of the device case: the block's ambient plus every lit light, against a normal facing the
/// viewer.
///
/// The directional branch mirrors the ABI's rule (the packing normalizes the direction, so an unnormalized value
/// arriving in the shader means the packing lost that step - and the announced direction is deliberately not unit
/// length), and the positional branch is what makes a POSITION observable: a point light at view (0, 0, 1) lights
/// the surface with `1 / (d^2 + 1)` of its colour, which no packing that forgot the translation can reproduce.
/// A zero direction contributes nothing (and must not produce a NaN).
const char* const kLightingFragment = "layout(set = 0, binding = 3, std140) uniform VineLightsBlock {\n"
                                      "    vec4 ambient;\n"
                                      "    vec4 position_kind[8];\n"
                                      "    vec4 direction_range[8];\n"
                                      "    vec4 color_intensity[8];\n"
                                      "    vec4 cone[8];\n"
                                      "} lights;\n"
                                      "layout(location = 0) out vec4 outColor;\n"
                                      "void main() {\n"
                                      "    vec3 n = vec3(0.0, 0.0, 1.0);\n"
                                      "    vec3 lit = lights.ambient.rgb * lights.ambient.a;\n"
                                      "    for (int i = 0; i < 8; ++i) {\n"
                                      "        float kind = lights.position_kind[i].w;\n"
                                      "        vec3 L = vec3(0.0);\n"
                                      "        float attenuation = 0.0;\n"
                                      "        if (kind > 0.5 && kind < 1.5) {\n"
                                      "            L = normalize(-lights.direction_range[i].xyz);\n"
                                      "            attenuation = 1.0;\n"
                                      "        } else if (kind > 1.5 && kind < 2.5) {\n"
                                      "            vec3 to_light = lights.position_kind[i].xyz;\n"
                                      "            float distance = length(to_light);\n"
                                      "            L = to_light / distance;\n"
                                      "            attenuation = 1.0 / (distance * distance + 1.0);\n"
                                      "        }\n"
                                      "        lit += lights.color_intensity[i].rgb * lights.color_intensity[i].a *\n"
                                      "               max(dot(n, L), 0.0) * attenuation;\n"
                                      "    }\n"
                                      "    outColor = vec4(lit, 1.0);\n"
                                      "}\n";

} // namespace

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
    LightRef sun       = lights[1];
    LightRef side      = lights[2];
    sun.has_direction  = true;
    sun.direction      = vn::math::Vec3d(0.0, 0.0, 1.0); // towards the viewer.
    side.has_direction = true;
    side.direction     = vn::math::Vec3d(2.0, 0.0, 0.0); // NOT unit length: the packing normalizes it.

    const LightRef    announced[]{ lights[0], sun, side };
    VineLightsBlock   block;
    const std::size_t represented = packLightBlock(announced, camera, block);

    EXPECT_EQ(represented, 3U) << "one ambient plus two directional lights are all packed";
    EXPECT_FLOAT_EQ(block.ambient[0], 0.2F);
    EXPECT_FLOAT_EQ(block.ambient[1], 0.3F);
    EXPECT_FLOAT_EQ(block.ambient[2], 0.4F);
    EXPECT_FLOAT_EQ(block.ambient[3], 0.5F) << "the ambient slot carries rgb + intensity";

    // Slot 0, in announcement order: a world +Z direction stays view +Z for this camera. `w` of the direction slot
    // is the RANGE (0 = no cutoff, which is what a directional light states).
    EXPECT_NEAR(block.direction_range[0][0], 0.0F, 1e-6F);
    EXPECT_NEAR(block.direction_range[0][1], 0.0F, 1e-6F);
    EXPECT_NEAR(block.direction_range[0][2], 1.0F, 1e-6F);
    EXPECT_FLOAT_EQ(block.direction_range[0][3], 0.0F) << "a directional light has no range";
    EXPECT_FLOAT_EQ(block.position_kind[0][0], 0.0F) << "a directional light has no position";
    EXPECT_FLOAT_EQ(block.position_kind[0][3], static_cast<float>(LightType::Directional)) << "the kind is the LightType value the shading switches on";
    EXPECT_FLOAT_EQ(block.color_intensity[0][0], 0.8F);
    EXPECT_FLOAT_EQ(block.color_intensity[0][3], 2.0F) << "rgb + intensity, like the ambient slot";

    // Slot 1: a world +X direction lands on view +X, and the length-2 direction comes out UNIT length - the
    // shading compares the direction against a normal, so an unnormalized one would scale the light.
    EXPECT_NEAR(block.direction_range[1][0], 1.0F, 1e-6F);
    EXPECT_NEAR(block.direction_range[1][1], 0.0F, 1e-6F);
    EXPECT_NEAR(block.direction_range[1][2], 0.0F, 1e-6F);
    EXPECT_FLOAT_EQ(block.color_intensity[1][1], 0.5F);

    // Nothing announced a third light, so the last slot says UNUSED - not "an ambient of its own", which is what a
    // zeroed kind would read as.
    EXPECT_FLOAT_EQ(block.position_kind[2][3], kLightKindEmpty);
    EXPECT_FLOAT_EQ(block.direction_range[2][2], 0.0F);
    EXPECT_FLOAT_EQ(block.color_intensity[2][0], 0.0F);
}

TEST(LightBlockTest, TheDirectionsAreRotatedIntoViewSpace)
{
    // The camera looks along +X instead of -Z, so a light that travels along the world's +X axis travels AWAY
    // from this camera: its view-space direction is view -Z. A packing that forgot the rotation would leave the
    // world direction in the block, and this is the case that tells the two apart.
    const CameraSnapshot looking_along_x = cameraLookingAlongPlusX();
    ASSERT_TRUE(looking_along_x.present);

    LightRef along; // world +X
    along.enabled       = true;
    along.type          = LightType::Directional;
    along.color         = vn::Colorf(1.0, 1.0, 1.0, 1.0);
    along.intensity     = 1.0F;
    along.has_direction = true;
    along.direction     = vn::math::Vec3d(1.0, 0.0, 0.0);

    LightRef against  = along;
    against.direction = vn::math::Vec3d(-1.0, 0.0, 0.0);

    LightRef up  = along;
    up.direction = vn::math::Vec3d(0.0, 1.0, 0.0);

    const LightRef  announced[]{ along, against, up };
    VineLightsBlock block;
    ASSERT_EQ(packLightBlock(announced, looking_along_x, block), 3U);

    EXPECT_NEAR(block.direction_range[0][2], -1.0F, 1e-6F) << "a light travelling along +X goes AWAY from this camera";
    EXPECT_NEAR(block.direction_range[0][0], 0.0F, 1e-6F);
    EXPECT_NEAR(block.direction_range[1][2], 1.0F, 1e-6F) << "and the opposite light travels towards it";
    EXPECT_NEAR(block.direction_range[2][1], 1.0F, 1e-6F) << "the camera's up is still the world's up";
    EXPECT_NEAR(block.direction_range[2][2], 0.0F, 1e-6F) << "with no leakage between the axes";
}

TEST(LightBlockTest, DisabledAndUnusableLightsAreNotPacked)
{
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    LightRef disabled_ambient = makeLight(LightType::Ambient, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightRef disabled_sun     = makeLight(LightType::Directional, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightRef unplaced_point   = makeLight(LightType::Point, 1.0, 0.0, 0.0, 1.0F); // no position: lights nowhere
    LightRef sun              = makeLight(LightType::Directional, 0.25, 0.5, 0.75, 1.0F);
    sun.has_direction         = true;
    sun.direction             = vn::math::Vec3d(0.0, 0.0, 1.0);

    const LightRef    announced[]{ disabled_ambient, disabled_sun, unplaced_point, sun };
    VineLightsBlock   block;
    const std::size_t represented = packLightBlock(announced, camera, block);

    EXPECT_EQ(represented, 1U) << "only the enabled, usable light is lit";
    EXPECT_NEAR(block.direction_range[0][2], 1.0F, 1e-6F) << "and it is the one in slot 0";
    EXPECT_FLOAT_EQ(block.position_kind[1][3], kLightKindEmpty) << "the next slot was never used";
    EXPECT_NEAR(block.color_intensity[0][0], 0.25F, 1e-6F);

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

    VineLightsBlock   block;
    const std::size_t represented = packLightBlock(std::span<const LightRef>{}, camera, block);

    EXPECT_EQ(represented, 0U) << "nothing was announced, so nothing is dropped either";
    EXPECT_FLOAT_EQ(block.ambient[0], 0.15F) << "and the scene is still visible instead of multiplied by zero";
    for (std::size_t slot = 0; slot < kMaxLights; ++slot) {
        EXPECT_FLOAT_EQ(block.position_kind[slot][3], kLightKindEmpty) << "slot " << slot << " says UNUSED";
        EXPECT_FLOAT_EQ(block.direction_range[slot][0], 0.0F);
        EXPECT_FLOAT_EQ(block.color_intensity[slot][0], 0.0F);
    }
}

TEST(LightBlockTest, APointLightCarriesItsViewSpacePositionAndRange)
{
    // A camera at world +Z looking at the origin, i.e. the identity rotation PLUS a translation: world (0, 0, 4)
    // lands at view (0, 0, -1). A packing that rotated but forgot to TRANSLATE would put it at (0, 0, 4), which is
    // the bug this case measures (see the file note: it only looks right while the camera sits at the origin).
    CameraSnapshot camera = cameraAtPlusZ();
    camera.view(2, 3)     = -5.0; // p_view = p_world + (0, 0, -5)
    ASSERT_TRUE(camera.present);

    LightRef lamp     = makeLight(LightType::Point, 0.9, 0.6, 0.3, 1.5F);
    lamp.has_position = true;
    lamp.position     = vn::math::Vec3d(0.0, 0.0, 4.0);
    lamp.range        = 12.0F;

    const LightRef  announced[]{ lamp };
    VineLightsBlock block;
    ASSERT_EQ(packLightBlock(announced, camera, block), 1U);

    EXPECT_NEAR(block.position_kind[0][0], 0.0F, 1e-6F);
    EXPECT_NEAR(block.position_kind[0][1], 0.0F, 1e-6F);
    EXPECT_NEAR(block.position_kind[0][2], -1.0F, 1e-6F) << "the view matrix's translation is part of the position";
    EXPECT_FLOAT_EQ(block.position_kind[0][3], static_cast<float>(LightType::Point));
    EXPECT_FLOAT_EQ(block.direction_range[0][3], 12.0F) << "the range rides in the direction slot's w";
    EXPECT_FLOAT_EQ(block.color_intensity[0][1], 0.6F);
    EXPECT_FLOAT_EQ(block.color_intensity[0][3], 1.5F);
}

TEST(LightBlockTest, ASpotLightCarriesItsConeAsCosines)
{
    // The block stores the cone as COSINES, so the shading does no trigonometry per fragment; the two angles are
    // degrees in the content API (see Light::setOuterConeAngle).
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    LightRef spot           = makeLight(LightType::Spot, 1.0, 1.0, 1.0, 1.0F);
    spot.has_position       = true;
    spot.position           = vn::math::Vec3d(0.0, 0.0, 2.0);
    spot.has_direction      = true;
    spot.direction          = vn::math::Vec3d(0.0, 0.0, -1.0);
    spot.range              = 8.0F;
    spot.inner_cone_degrees = 15.0;
    spot.outer_cone_degrees = 30.0;

    const LightRef  announced[]{ spot };
    VineLightsBlock block;
    ASSERT_EQ(packLightBlock(announced, camera, block), 1U);

    EXPECT_FLOAT_EQ(block.position_kind[0][3], static_cast<float>(LightType::Spot));
    EXPECT_NEAR(block.cone[0][0], std::cos(15.0 * 3.14159265358979323846 / 180.0), 1e-6F);
    EXPECT_NEAR(block.cone[0][1], std::cos(30.0 * 3.14159265358979323846 / 180.0), 1e-6F);
    EXPECT_LT(block.cone[0][1], block.cone[0][0]) << "the outer edge is the WIDER angle, so the SMALLER cosine";
    EXPECT_NEAR(block.direction_range[0][2], -1.0F, 1e-6F) << "the axis, in view space";
}

TEST(LightBlockTest, OnlyEightLightsGetSlots)
{
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    // Ten announced, eight carried: the surplus is counted as dropped (the caller reports it once per episode).
    std::vector<LightRef> announced;
    for (std::size_t index = 0; index < 10U; ++index) {
        LightRef light      = makeLight(LightType::Directional, 0.1F * static_cast<float>(index + 1U), 0.0, 0.0, 1.0F);
        light.has_direction = true;
        light.direction     = vn::math::Vec3d(0.0, 0.0, 1.0);
        announced.push_back(light);
    }

    VineLightsBlock block;
    EXPECT_EQ(packLightBlock(announced, camera, block), kMaxLights) << "the block holds eight: the caller reports the difference once per episode";
    EXPECT_FLOAT_EQ(block.color_intensity[0][0], 0.1F);
    EXPECT_FLOAT_EQ(block.color_intensity[7][0], 0.8F) << "in announcement order, and the ninth has no slot";
    EXPECT_FLOAT_EQ(block.position_kind[7][3], static_cast<float>(LightType::Directional));
}

TEST(LightBlockTest, ACameraThatWasNeverEstablishedLeavesTheBlockEmpty)
{
    // No camera means no view space to light in: the block stays empty (black), NOT the ambient fill. That is the
    // reference behaviour and it is visible - a pass that announced no camera is unlit rather than lit by a guess.
    const CameraSnapshot none;
    ASSERT_FALSE(none.present);

    LightRef sun      = makeLight(LightType::Directional, 1.0, 1.0, 1.0, 1.0F);
    LightRef ambient  = makeLight(LightType::Ambient, 0.5, 0.5, 0.5, 1.0F);
    sun.has_direction = true;
    sun.direction     = vn::math::Vec3d(0.0, 0.0, 1.0);
    const LightRef announced[]{ ambient, sun };

    VineLightsBlock block;
    block.ambient[0] = 9.0F; // a value the packing must overwrite
    EXPECT_EQ(packLightBlock(announced, none, block), 0U);
    EXPECT_FLOAT_EQ(block.ambient[0], 0.0F) << "every field is written, the fill included";
    EXPECT_FLOAT_EQ(block.direction_range[0][2], 0.0F);
}

TEST(LightBlockTest, TheLightsReachTheFragmentStagePerDrawingCall)
{
    // THE PICTURE. One pass, four drawing calls, four bands of 16 pixels; each band's light list is chosen so that
    // only a correct packing, bound PER CALL, can explain its colour (see the file note).
    const vn::vsg::DeviceResult created = vn::vsg::createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    OffscreenTarget::Layout layout;
    layout.width  = kSize;
    layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index) {
        layout.clear_color[index] = kGreen[index];
    }
    std::unique_ptr<OffscreenTarget> target = OffscreenTarget::create(created.device, layout);
    ASSERT_NE(target, nullptr);

    // The camera every call announces: at +Z, looking at the origin. A world +Z light therefore travels TOWARDS
    // the viewer, which is the direction that lights the fragment stage's fixed normal.
    Camera camera;
    camera.lookAt(vn::math::Vec3d(0.0, 0.0, 5.0), vn::math::Vec3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));

    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage vertex;
        vertex.type   = ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>("layout(location = 0) in vec3 position;\n"
                                                                    "void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n"));
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>(kLightingFragment));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    // The block set the program's OWN declarations need: it declares the lights block, so that (and only
    // that) is what its pipelines were compiled against and what the pass binds.
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::forAbi(program_facts.abi, 0U, created.device, *storage);
    ASSERT_NE(descriptors, nullptr);
    BlockDescriptors* content_blocks[] = { descriptors.get() };

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments                 = 1U;
    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::create(program_facts.abi,
                                                                         std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                                                         std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                                                         program_facts.shaders,
                                                                         settings);
    ASSERT_NE(pipelines, nullptr);

    // Four bands: one triangle each, its base along the TOP rows of the target (NDC y = -1) and its apex at the
    // bottom (NDC y = +1) - the framebuffer's origin is the upper left, so NDC y = +1 lands on the last row.
    const auto make_band = [](float left, float right) {
        auto        indices  = vn::intrusive_ptr<const vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
        auto        geometry = vn::intrusive_ptr<Geometry>(new Geometry());
        const float centre   = (left + right) * 0.5F;
        geometry->setPositions(
            vn::intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::vector<float>{ left, -1.0F, 0.0F, right, -1.0F, 0.0F, centre, 1.0F, 0.0F })));
        setPrimitiveIndices(*geometry, 0U, indices);
        geometry->setRevision(1U);
        return geometry;
    };
    const vn::intrusive_ptr<Geometry> bands[]{ make_band(-1.0F, -0.5F), make_band(-0.5F, 0.0F), make_band(0.0F, 0.5F), make_band(0.5F, 1.0F) };

    const vn::intrusive_ptr<Material> material(new Material());
    material->setBaseColor(vn::Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    GeometryFacts                        geometry_facts[4];
    std::vector<vn::vsg::ChannelFacts>   geometry_channels[4];
    std::vector<vn::vsg::PrimitiveFacts> geometry_primitives[4];
    for (std::size_t index = 0; index < 4U; ++index) {
        ASSERT_EQ(buildGeometryFacts(*bands[index], geometry_facts[index], geometry_channels[index], geometry_primitives[index]), FactMiss::None);
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
    // cannot light (a disabled one, and a positional light nobody placed) and must be the ambient fill - with ONE
    // report between them.
    //
    // A light's DIRECTION is the direction the light TRAVELS (see Light::setDirection): the vector the shading
    // uses is its NEGATION, so a sun whose light travels AWAY from the viewer (-Z) is the one that lights a face
    // turned towards the viewer.
    vn::intrusive_ptr<Light> ambient = Light::createAmbient();
    ambient->setColor(vn::Colorf(0.1, 0.1, 0.1, 1.0));
    ambient->setIntensity(1.0F);
    vn::intrusive_ptr<Light> sun_towards = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    sun_towards->setColor(vn::Colorf(0.4, 0.0, 0.0, 1.0));
    sun_towards->setIntensity(1.0F);
    vn::intrusive_ptr<Light> sun_away = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, 1.0));
    sun_away->setColor(vn::Colorf(0.4, 0.0, 0.0, 1.0));
    sun_away->setIntensity(1.0F);
    vn::intrusive_ptr<Light> disabled_ambient = Light::createAmbient();
    disabled_ambient->setColor(vn::Colorf(0.9, 0.9, 0.9, 1.0));
    disabled_ambient->setEnabled(false);
    vn::intrusive_ptr<Light> disabled_sun = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, 1.0));
    disabled_sun->setColor(vn::Colorf(0.9, 0.9, 0.9, 1.0));
    disabled_sun->setEnabled(false);

    const Light* towards_lights[]{ ambient.get(), sun_towards.get() };
    const Light* away_lights[]{ ambient.get(), sun_away.get() };
    const Light* disabled_ambient_lights[]{ disabled_ambient.get() };
    const Light* disabled_sun_lights[]{ disabled_sun.get() };

    const vn::intrusive_ptr<RenderTarget> handle(new RenderTarget());
    TargetFacts                           target_facts;
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

    ClearValues clear;
    clear.color = true;
    for (std::size_t index = 0; index < 4U; ++index) {
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
    const std::span<const vn::graphics::backend::CompiledDraw> draws = frame.passes[0].draws;
    ASSERT_EQ(draws.size(), 4U);
    EXPECT_EQ(draws[0].lights.size(), 2U) << "the plan carries each call's own announcement";
    EXPECT_EQ(draws[3].lights.size(), 1U) << "the fourth call announced one unusable light";

    storage->beginFrame();
    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points = vn::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   draws_recorder(*pipelines, pool, entry_points);
    StateRegistry registry(pool);

    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ DrawKind::Content, program.get(), program_facts.revision, geometry_facts[0].layout, pipelines.get(), &draws_recorder }
    };
    ContentPass::Scope scope;
    scope.entries    = halves;
    scope.registry   = &registry;
    scope.storage    = storage.get();
    scope.block_sets = content_blocks;
    scope.uploads    = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  node;
    ASSERT_TRUE(content.record(frame.passes[0], facts, target->shape().compatibility(), {}, view_block, node));
    EXPECT_EQ(diagnostics.count(vn::graphics::DiagnosticCategory::ContentSkipped), 0U) << "nothing may be refused: every band is a complete content draw";
    EXPECT_EQ(diagnostics.count(vn::graphics::DiagnosticCategory::ChannelIgnored), 0U)
        << "a DISABLED light is not a drop: the two unusable bands said so themselves (see the announced lists)";

    VsgExecutor executor(diagnostics);
    executor.addTarget(handle.get(), target.get());
    auto              command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{
        PassContent{ 1U, node }
    };
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

    // The four bands, read back: ambient + the sun that reaches the surface, ambient alone for the one that does
    // not, and the block's fill for the two calls whose light could not be lit at all.
    const auto  near    = [](std::uint8_t byte, double value) { return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0; };
    const auto  band_of = [&](int x) { return target->probe().pixel(x, 32); };
    const Rgba8 lit     = band_of(8);
    EXPECT_TRUE(near(lit.r, 0.5) && near(lit.g, 0.1) && near(lit.b, 0.1))
        << "band 0 is ambient + the sun pointing at the viewer, got (" << static_cast<int>(lit.r) << ", " << static_cast<int>(lit.g) << ", "
        << static_cast<int>(lit.b) << ")";

    const Rgba8 away = band_of(24);
    EXPECT_TRUE(near(away.r, 0.1) && near(away.g, 0.1) && near(away.b, 0.1))
        << "band 1 is ambient alone: the same sun the other way lights nothing, got (" << static_cast<int>(away.r) << ", " << static_cast<int>(away.g) << ", "
        << static_cast<int>(away.b) << ")";

    const Rgba8 fill = band_of(40);
    EXPECT_TRUE(near(fill.r, 0.15) && near(fill.g, 0.15) && near(fill.b, 0.15))
        << "band 2 announced a disabled light, so it is the block's ambient fill, got (" << static_cast<int>(fill.r) << ", " << static_cast<int>(fill.g) << ", "
        << static_cast<int>(fill.b) << ")";

    const Rgba8 fill_again = band_of(56);
    EXPECT_TRUE(near(fill_again.r, 0.15) && near(fill_again.g, 0.15) && near(fill_again.b, 0.15))
        << "band 3 announced another disabled light and is the fill as well, got (" << static_cast<int>(fill_again.r) << ", " << static_cast<int>(fill_again.g)
        << ", " << static_cast<int>(fill_again.b) << ")";

    // The gaps between the bands are the pass' own clear, which is what makes the four values above a PICTURE:
    // the bands are not "the whole target painted somehow" - they sit on a background the shading never wrote.
    const Rgba8 background = target->probe().pixel(2, 32);
    EXPECT_TRUE(background.g > 150 && background.r < 40 && background.b < 40)
        << "outside the four triangles the pass keeps its clear, got (" << static_cast<int>(background.r) << ", " << static_cast<int>(background.g) << ", "
        << static_cast<int>(background.b) << ")";
}

TEST(LightBlockTest, BothLightingProgramsReadTheBlockAtTheSameBinding)
{
    // The lights are ONE block on both drawing paths (see LightBlock): the forward content program and the
    // deferred full-screen program declare it at the same set/binding, so a scene lights identically whichever
    // path draws it - and the full-screen path no longer carries a second copy of the same numbers in its push
    // range. The block's size is the ABI (the static_asserts in LightBlock.hpp are the other half).
    const auto lights_of = [](const ProgramAbi& abi) -> const AbiBinding* {
        for (const AbiBinding& binding : abi.bindings) {
            if (binding.role == AbiBlockRole::Lights) {
                return &binding;
            }
        }
        return nullptr;
    };

    ProgramFacts forward_facts;
    ASSERT_EQ(buildProgramFacts(*vn::graphics::forwardProgram(), forward_facts), FactMiss::None);
    const AbiBinding* forward_lights = lights_of(forward_facts.abi);
    ASSERT_NE(forward_lights, nullptr);
    EXPECT_EQ(forward_lights->set, 0U);
    EXPECT_EQ(forward_lights->binding, 6U) << "the binding both paths share (see vine_params.glsl)";
    EXPECT_EQ(forward_lights->block_size, sizeof(VineLightsBlock));

    ProgramFacts screen_facts;
    ASSERT_EQ(buildScreenProgramFacts(*vn::graphics::deferredLightProgram(), screen_facts), FactMiss::None);
    const AbiBinding* screen_lights = lights_of(screen_facts.abi);
    ASSERT_NE(screen_lights, nullptr) << "the deferred lighting program reads the same block";
    EXPECT_EQ(screen_lights->set, forward_lights->set);
    EXPECT_EQ(screen_lights->binding, forward_lights->binding);
    EXPECT_EQ(screen_lights->block_size, sizeof(VineLightsBlock));
    EXPECT_TRUE(screen_facts.abi.pushes.empty()) << "the lights are a block, not a push: the range is a host's";
}

TEST(LightBlockTest, TheLightsReachTheFragmentStagePerFullScreenCall)
{
    // THE PICTURE. Three full-screen calls in ONE pass, each with its own viewport (a third of the target) and its
    // own light list: a sun pointing at the viewer, the same sun reversed, and no announcement at all. The fragment
    // stage reads the LIGHT BLOCK - the same block the content path binds - so each band's colour is its own call's
    // announcement:
    //
    //   band 0: ambient + the sun towards the viewer -> 0.5 / 0.1 / 0.1
    //   band 1: the same light the other way         -> 0.1 / 0.1 / 0.1 (ambient alone)
    //   band 2: nothing announced                    -> 0.15 (the fill, so the band is visible at all)
    //
    // A block written once per PASS (all three calls sharing the first announcement), a zeroed block, or one bound
    // at the wrong binding moves at least one band, and the destination's clear (blue) is what a band that drew
    // nothing would show.
    const vn::vsg::DeviceResult created = vn::vsg::createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    constexpr float         kBlue[4]{ 0.0F, 0.0F, 0.75F, 1.0F };
    OffscreenTarget::Layout destination_layout;
    destination_layout.width  = kSize;
    destination_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index) {
        destination_layout.clear_color[index] = kBlue[index];
    }
    std::unique_ptr<OffscreenTarget> destination = OffscreenTarget::create(created.device, destination_layout);
    ASSERT_NE(destination, nullptr);

    // The program: the engine's canonical full-screen vertex stage (buildScreenProgramFacts composes it) with a
    // fragment stage that shades from the light block - at the binding both paths give it (see vine_params.glsl).
    const vn::intrusive_ptr<ShaderProgram> program(new ShaderProgram());
    {
        ShaderStage fragment;
        fragment.type   = ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>("layout(set = 0, binding = 5, std140) uniform VineLightsBlock {\n"
                                                                      "    vec4 ambient;\n"
                                                                      "    vec4 position_kind[8];\n"
                                                                      "    vec4 direction_range[8];\n"
                                                                      "    vec4 color_intensity[8];\n"
                                                                      "    vec4 cone[8];\n"
                                                                      "} lights;\n"
                                                                      "layout(location = 0) out vec4 outColor;\n"
                                                                      "void main() {\n"
                                                                      "    vec3 n = vec3(0.0, 0.0, 1.0);\n"
                                                                      "    vec3 lit = lights.ambient.rgb * lights.ambient.a;\n"
                                                                      "    for (int i = 0; i < 8; ++i) {\n"
                                                                      "        if (lights.position_kind[i].w < 0.5) continue;\n"
                                                                      "        lit += lights.color_intensity[i].rgb * lights.color_intensity[i].a *\n"
                                                                      "               max(dot(n, normalize(-lights.direction_range[i].xyz)), 0.0);\n"
                                                                      "    }\n"
                                                                      "    outColor = vec4(lit, 1.0);\n"
                                                                      "}\n"));
        program->addStage(fragment);
    }
    ProgramFacts program_facts;
    ASSERT_EQ(buildScreenProgramFacts(*program, program_facts), FactMiss::None);

    std::unique_ptr<ContentPipeline> pipelines = ContentPipeline::createScreen(program_facts.abi, program_facts.shaders);
    ASSERT_NE(pipelines, nullptr);

    // The camera every call announces: at +Z looking at the origin. A light's DIRECTION is where it travels, so the
    // one whose light travels away from the viewer (-Z) is the one that lights the face turned towards it.
    Camera camera;
    camera.lookAt(vn::math::Vec3d(0.0, 0.0, 5.0), vn::math::Vec3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));

    vn::intrusive_ptr<Light> ambient = Light::createAmbient();
    ambient->setColor(vn::Colorf(0.1, 0.1, 0.1, 1.0));
    vn::intrusive_ptr<Light> towards = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, -1.0));
    towards->setColor(vn::Colorf(0.4, 0.0, 0.0, 1.0));
    vn::intrusive_ptr<Light> away = Light::createDirectional(vn::math::Vec3d(0.0, 0.0, 1.0));
    away->setColor(vn::Colorf(0.4, 0.0, 0.0, 1.0));

    const Light* band0_lights[]{ ambient.get(), towards.get() };
    const Light* band1_lights[]{ ambient.get(), away.get() };

    const vn::intrusive_ptr<RenderTarget> handle(new RenderTarget());
    TargetFacts                           target_facts;
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
    const std::uint32_t band_width = kSize / 3U; // 21 / 21 / 22: the last band takes the remainder
    recorder.setViewport(0, 0, static_cast<int>(band_width), static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>(band0_lights, 2U));
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.setViewport(static_cast<int>(band_width), 0, static_cast<int>(band_width), static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>(band1_lights, 2U));
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.setViewport(static_cast<int>(band_width * 2U), 0, static_cast<int>(kSize - band_width * 2U), static_cast<int>(kSize));
    recorder.setLights(std::span<const Light* const>{}); // the backend default: the fill
    recorder.drawScreenProgram(handle.get(), program.get(), &camera);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 1U);
    ASSERT_EQ(frame.passes[0].draws.size(), 3U);
    EXPECT_EQ(frame.passes[0].draws[0].lights.size(), 2U) << "each call carries its own announcement";
    EXPECT_EQ(frame.passes[0].draws[2].lights.size(), 0U);

    std::unique_ptr<BlockStorage> storage = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);

    storage->beginFrame();
    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points = vn::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   draws(*pipelines, pool, entry_points);
    StateRegistry registry(pool);

    const ContentPass::Scope::Entry halves[]{
        ContentPass::Scope::Entry{ DrawKind::Screen, program_facts.program, program_facts.revision, {}, pipelines.get(), &draws }
    };
    ContentPass::Scope scope;
    scope.entries  = halves;
    scope.registry = &registry;
    scope.storage  = storage.get();
    scope.uploads  = &uploads;
    ContentPass content(scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  node;
    const ContentFacts           facts; // a screen call reads no tables: its half is looked up by program identity
    ASSERT_TRUE(content.record(frame.passes[0], facts, destination->shape().compatibility(), {}, view_block, node));
    EXPECT_EQ(diagnostics.count(vn::graphics::DiagnosticCategory::ContentSkipped), 0U);
    EXPECT_EQ(diagnostics.count(vn::graphics::DiagnosticCategory::ChannelIgnored), 0U)
        << "both calls announced lights that fit the block, so neither kind of path reports a drop";

    VsgExecutor executor(diagnostics);
    executor.addTarget(handle.get(), destination.get());
    auto              command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{
        PassContent{ 1U, node }
    };
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

    const auto near = [](std::uint8_t byte, double value) { return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0; };
    const auto band = [&](int x) { return destination->probe().pixel(x, 32); };

    const Rgba8 lit = band(8);
    EXPECT_TRUE(near(lit.r, 0.5) && near(lit.g, 0.1) && near(lit.b, 0.1))
        << "band 0 is the push's ambient + the sun towards the viewer, got (" << static_cast<int>(lit.r) << ", " << static_cast<int>(lit.g) << ", "
        << static_cast<int>(lit.b) << ")";

    const Rgba8 away_band = band(24);
    EXPECT_TRUE(near(away_band.r, 0.1) && near(away_band.g, 0.1) && near(away_band.b, 0.1))
        << "band 1 is ambient alone: the same push layout, the direction reversed, got (" << static_cast<int>(away_band.r) << ", "
        << static_cast<int>(away_band.g) << ", " << static_cast<int>(away_band.b) << ")";

    const Rgba8 fill = band(54);
    EXPECT_TRUE(near(fill.r, 0.15) && near(fill.g, 0.15) && near(fill.b, 0.15)) << "band 2 announced nothing and is the fill, got (" << static_cast<int>(fill.r)
                                                                                << ", " << static_cast<int>(fill.g) << ", " << static_cast<int>(fill.b) << ")";
}
