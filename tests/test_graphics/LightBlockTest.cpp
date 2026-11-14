/**
 * @brief The LIGHT BLOCK: the SDK's fifth L1 block and the ONE packing rule both backends fill it with.
 *
 * Why these are device-free unit cases. The block is a function of the announced lights and the camera, so its
 * arithmetic - the world -> view rotation, the slot order, the ambient fill, the count, the unused-slot tag - is
 * pinned here without a device. The picture that proves the block reaches a shader (per drawing call, on both the
 * content and the full-screen path) needs a device and therefore lives in the vsg suite
 * (`tests/test_vsg/LightBlockTest.cpp`), which is also where a host can compare it with the reference backend.
 *
 * The ownership rule these cases belong to: the struct and the packing are the engine's, not a backend's
 * (see .ai/design/graphics-shader-params.md §2.1/§3.3) - a backend only decides WHERE the bytes go.
 */

#include <gtest/gtest.h>

#include <vine/Colorf.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/LightAbi.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

using namespace vn::graphics;

namespace
{

/// @brief One announced light as the frame remembers it (the plan's own type).
LightValue makeLight(LightType type, double r, double g, double b, float intensity, bool enabled = true)
{
    LightValue light;
    light.enabled   = enabled;
    light.type      = type;
    light.color     = vn::Colorf(r, g, b, 1.0);
    light.intensity = intensity;
    return light;
}

/// @brief A camera at +Z looking at the origin, up +Y: its view matrix is the world's own axes.
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

} // namespace

TEST(LightBlockTest, TheBlockIsTheShapeTheShaderDeclares)
{
    // The numbers the GLSL declaration in shaders/vine_params.glsl states: one ambient vec4 plus four arrays of
    // kMaxLights vec4 rows. The static_asserts in ShaderAbi.hpp are the compile-time half; this case keeps the
    // expected numbers visible where a failure names them.
    EXPECT_EQ(sizeof(VineLightsBlock), 16U + 4U * kMaxLights * 16U);
    EXPECT_EQ(alignof(VineLightsBlock), 16U);
    EXPECT_EQ(offsetof(VineLightsBlock, position_kind), 16U);
    EXPECT_EQ(offsetof(VineLightsBlock, direction_range), 16U + kMaxLights * 16U);
    EXPECT_EQ(offsetof(VineLightsBlock, color_intensity), 16U + 2U * kMaxLights * 16U);
    EXPECT_EQ(offsetof(VineLightsBlock, cone), 16U + 3U * kMaxLights * 16U);
    EXPECT_EQ(kLightKindEmpty, -1.0F) << "an unused slot's kind tag is part of the ABI (the GLSL compares against -1)";
}

TEST(LightBlockTest, TheBlockCarriesTheLightsInViewSpace)
{
    // The camera sits at +Z looking at the origin, so view space and world space share their axes - which is
    // exactly why the interesting case below uses a ROTATED camera: with this one a packing that forgot the
    // rotation would look correct.
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    const LightValue lights[]{
        makeLight(LightType::Ambient, 0.2, 0.3, 0.4, 0.5F),
        makeLight(LightType::Directional, 0.8, 0.4, 0.2, 2.0F),
        makeLight(LightType::Directional, 0.1, 0.5, 0.9, 1.0F),
    };
    LightValue sun       = lights[1];
    LightValue side      = lights[2];
    sun.has_direction    = true;
    sun.direction        = vn::math::Vec3d(0.0, 0.0, 1.0); // towards the viewer.
    side.has_direction   = true;
    side.direction       = vn::math::Vec3d(2.0, 0.0, 0.0); // NOT unit length: the packing normalizes it.

    const LightValue  announced[]{ lights[0], sun, side };
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

    LightValue along; // world +X
    along.enabled       = true;
    along.type          = LightType::Directional;
    along.color         = vn::Colorf(1.0, 1.0, 1.0, 1.0);
    along.intensity     = 1.0F;
    along.has_direction = true;
    along.direction     = vn::math::Vec3d(1.0, 0.0, 0.0);

    LightValue against  = along;
    against.direction   = vn::math::Vec3d(-1.0, 0.0, 0.0);

    LightValue up  = along;
    up.direction   = vn::math::Vec3d(0.0, 1.0, 0.0);

    const LightValue announced[]{ along, against, up };
    VineLightsBlock  block;
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

    LightValue disabled_ambient = makeLight(LightType::Ambient, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightValue disabled_sun     = makeLight(LightType::Directional, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    LightValue unplaced_point   = makeLight(LightType::Point, 1.0, 0.0, 0.0, 1.0F); // no position: lights nowhere
    LightValue sun              = makeLight(LightType::Directional, 0.25, 0.5, 0.75, 1.0F);
    sun.has_direction           = true;
    sun.direction               = vn::math::Vec3d(0.0, 0.0, 1.0);

    const LightValue  announced[]{ disabled_ambient, disabled_sun, unplaced_point, sun };
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
    const std::size_t represented = packLightBlock(std::span<const LightValue>{}, camera, block);

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

    LightValue lamp   = makeLight(LightType::Point, 0.9, 0.6, 0.3, 1.5F);
    lamp.has_position = true;
    lamp.position     = vn::math::Vec3d(0.0, 0.0, 4.0);
    lamp.range        = 12.0F;

    const LightValue announced[]{ lamp };
    VineLightsBlock  block;
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

    LightValue spot           = makeLight(LightType::Spot, 1.0, 1.0, 1.0, 1.0F);
    spot.has_position         = true;
    spot.position             = vn::math::Vec3d(0.0, 0.0, 2.0);
    spot.has_direction        = true;
    spot.direction            = vn::math::Vec3d(0.0, 0.0, -1.0);
    spot.range                = 8.0F;
    spot.inner_cone_degrees   = 15.0;
    spot.outer_cone_degrees   = 30.0;

    const LightValue announced[]{ spot };
    VineLightsBlock  block;
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
    std::vector<LightValue> announced;
    for (std::size_t index = 0; index < 10U; ++index) {
        LightValue light    = makeLight(LightType::Directional, 0.1F * static_cast<float>(index + 1U), 0.0, 0.0, 1.0F);
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

    LightValue sun      = makeLight(LightType::Directional, 1.0, 1.0, 1.0, 1.0F);
    LightValue ambient  = makeLight(LightType::Ambient, 0.5, 0.5, 0.5, 1.0F);
    sun.has_direction   = true;
    sun.direction       = vn::math::Vec3d(0.0, 0.0, 1.0);
    const LightValue announced[]{ ambient, sun };

    VineLightsBlock block;
    block.ambient[0] = 9.0F; // a value the packing must overwrite
    EXPECT_EQ(packLightBlock(announced, none, block), 0U);
    EXPECT_FLOAT_EQ(block.ambient[0], 0.0F) << "every field is written, the fill included";
    EXPECT_FLOAT_EQ(block.direction_range[0][2], 0.0F);
}

TEST(LightBlockTest, TheSlotARuleShadowNamesIsTheSlotThePackingGave)
{
    // packedSlotOf is the packing's own walk (see LightAbi.hpp): the two must agree, or a shadow map scales a
    // different light's term than the one it belongs to.
    const CameraSnapshot camera = cameraAtPlusZ();
    ASSERT_TRUE(camera.present);

    int        off_id       = 0;
    int        ambient_id   = 0;
    int        unplaced_id  = 0;
    int        sun_id       = 0;
    LightValue off          = makeLight(LightType::Directional, 1.0, 0.0, 0.0, 1.0F, /*enabled*/ false);
    off.identity            = &off_id;
    LightValue ambient      = makeLight(LightType::Ambient, 0.1, 0.1, 0.1, 1.0F);
    ambient.identity        = &ambient_id;
    LightValue unplaced     = makeLight(LightType::Point, 0.0, 1.0, 0.0, 1.0F); // no position
    unplaced.identity       = &unplaced_id;
    LightValue sun          = makeLight(LightType::Directional, 0.0, 0.0, 1.0, 1.0F);
    sun.identity            = &sun_id;
    sun.has_direction       = true;
    sun.direction           = vn::math::Vec3d(0.0, 0.0, 1.0);

    const LightValue announced[]{ off, ambient, unplaced, sun };
    VineLightsBlock  block;
    ASSERT_EQ(packLightBlock(announced, camera, block), 2U) << "the ambient plus the sun";

    EXPECT_EQ(packedSlotOf(announced, &sun_id), 0U) << "a disabled light and an unplaced one take no slot, so the sun is first";
    EXPECT_EQ(packedSlotOf(announced, &off_id), kMaxLights) << "a disabled light was never packed";
    EXPECT_EQ(packedSlotOf(announced, &ambient_id), kMaxLights) << "an ambient light fills the ambient field, not a slot";
    EXPECT_EQ(packedSlotOf(announced, &unplaced_id), kMaxLights) << "a positional light nobody placed takes no slot";
    EXPECT_EQ(packedSlotOf(announced, nullptr), kMaxLights) << "a null identity names no light";
    EXPECT_FLOAT_EQ(block.position_kind[0][3], static_cast<float>(LightType::Directional));
}
