#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/vsg/VsgOverlay.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>

using vine::graphics::Camera;
using vine::graphics::Light;
using vine::graphics::LightPtr;
using vine::vsg::detail::fillLightPushBlock;
using vine::vsg::detail::LightPushBlock;
using vine::vsg::detail::viewRotation;

namespace
{

/** @brief Configures @p camera as a perspective camera at the origin looking down -Z. */
void makeForwardCamera(Camera& camera)
{
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 0.0), vine::math::Vec3d(0.0, 0.0, -1.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsPerspective(90.0, 2.0, 0.1, 100.0);
}

/** @brief Bakes @p lights for @p camera into a fresh block. */
LightPushBlock blockFor(const Camera* camera, const std::vector<const Light*>& lights)
{
    LightPushBlock block;
    fillLightPushBlock(camera, lights, block);
    return block;
}

/** @brief Every float of the block, for the "the block is zeroed" assertion. */
std::vector<float> allFloats(const LightPushBlock& block)
{
    std::vector<float> values(block.ambient.begin(), block.ambient.end());
    values.insert(values.end(), block.projparms.begin(), block.projparms.end());
    for (const auto& dir : block.dirs) {
        values.insert(values.end(), dir.begin(), dir.end());
    }
    for (const auto& col : block.cols) {
        values.insert(values.end(), col.begin(), col.end());
    }
    return values;
}

/** @brief Length of a three-element basis vector. */
double length(const double v[3])
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/** @brief Dot product of two three-element basis vectors. */
double dot(const double a[3], const double b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

} // namespace

// ---------------------------------------------------------------------------
// The camera basis the lights are transformed with.
// ---------------------------------------------------------------------------

TEST(OverlayLightingTest, ViewRotationBuildsARightHandedBasis)
{
    Camera camera;
    makeForwardCamera(camera);
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(&camera, r, u, f);

    // Looking down -Z with +Y up: right = +X, up = +Y, forward = -Z.
    EXPECT_NEAR(r[0], 1.0, 1e-12);
    EXPECT_NEAR(r[1], 0.0, 1e-12);
    EXPECT_NEAR(r[2], 0.0, 1e-12);
    EXPECT_NEAR(u[1], 1.0, 1e-12);
    EXPECT_NEAR(f[2], -1.0, 1e-12);
}

TEST(OverlayLightingTest, ViewRotationFollowsTheCameraOrientation)
{
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(5.0, 0.0, 0.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(&camera, r, u, f);

    // Looking down -X: forward = -X, right = the world's -Z, and "up" stays +Y.
    EXPECT_NEAR(f[0], -1.0, 1e-12);
    EXPECT_NEAR(u[1], 1.0, 1e-12);
    EXPECT_NEAR(r[2], -1.0, 1e-12);
}

TEST(OverlayLightingTest, ViewRotationIsOrthonormalForAnyCamera)
{
    const vine::math::Vec3d eyes[]    = { { 3.0, 4.0, 5.0 }, { -2.0, 0.5, 1.0 }, { 0.0, 0.0, 10.0 } };
    const vine::math::Vec3d targets[] = { { 0.0, 0.0, 0.0 }, { 1.0, -1.0, 0.0 }, { 0.0, 1.0, 0.0 } };
    for (const auto& eye : eyes) {
        for (const auto& target : targets) {
            Camera camera;
            camera.setViewMatrixAsLookAt(eye, target, vine::math::Vec3d(0.0, 1.0, 0.0));
            double r[3] = {}, u[3] = {}, f[3] = {};
            viewRotation(&camera, r, u, f);

            EXPECT_NEAR(length(r), 1.0, 1e-12);
            EXPECT_NEAR(length(u), 1.0, 1e-12);
            EXPECT_NEAR(length(f), 1.0, 1e-12);
            EXPECT_NEAR(dot(r, u), 0.0, 1e-12);
            EXPECT_NEAR(dot(r, f), 0.0, 1e-12);
            EXPECT_NEAR(dot(u, f), 0.0, 1e-12);
        }
    }
}

TEST(OverlayLightingTest, ViewRotationSurvivesDegenerateInput)
{
    // A camera whose eye IS its target has no forward direction: the fallback keeps a usable
    // basis instead of dividing by zero (the push block would otherwise hold NaN).
    Camera degenerate;
    degenerate.setViewMatrixAsLookAt(vine::math::Vec3d(1.0, 2.0, 3.0), vine::math::Vec3d(1.0, 2.0, 3.0),
                                     vine::math::Vec3d(0.0, 1.0, 0.0));
    double r[3] = {}, u[3] = {}, f[3] = {};
    viewRotation(&degenerate, r, u, f);
    EXPECT_DOUBLE_EQ(f[2], -1.0);

    // An up vector PARALLEL to the view direction leaves no right vector either: the fallback is
    // +X rather than a zero vector, so the basis stays orthonormal.
    Camera parallel;
    parallel.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                   vine::math::Vec3d(0.0, 0.0, 1.0));
    viewRotation(&parallel, r, u, f);
    EXPECT_DOUBLE_EQ(r[0], 1.0);
    EXPECT_DOUBLE_EQ(r[1], 0.0);
    EXPECT_DOUBLE_EQ(r[2], 0.0);
    EXPECT_NEAR(length(u), 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// The push block the fullscreen program reads.
// ---------------------------------------------------------------------------

TEST(OverlayLightingTest, PushBlockIsZeroedWithoutACamera)
{
    // The block is written verbatim into the push range, so a camera-less draw must zero it
    // rather than push whatever the previous frame left in it.
    LightPushBlock block;
    block.ambient[0] = 9.0f;
    fillLightPushBlock(nullptr, {}, block);
    for (const float value : allFloats(block)) {
        EXPECT_EQ(value, 0.0f);
    }
}

TEST(OverlayLightingTest, PerspectiveParametersReconstructViewPosition)
{
    Camera camera;
    makeForwardCamera(camera);
    // 90 degrees vertical fov -> cot(45) == 1; aspect 2 -> proj[0][0] == 0.5.
    const LightPushBlock block = blockFor(&camera, {});

    EXPECT_FLOAT_EQ(block.projparms[0], 0.1f);
    EXPECT_FLOAT_EQ(block.projparms[1], 100.0f);
    EXPECT_FLOAT_EQ(block.projparms[2], 0.5f);
    EXPECT_FLOAT_EQ(block.projparms[3], 1.0f);
}

TEST(OverlayLightingTest, OrthographicCameraLeavesThePerspectiveParametersEmpty)
{
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 0.0), vine::math::Vec3d(0.0, 0.0, -1.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.1, 100.0);

    const auto           light = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    const LightPushBlock block = blockFor(&camera, { light.get() });

    // Only the depth-reconstruction parameters are perspective-specific.
    EXPECT_FLOAT_EQ(block.projparms[0], 0.0f);
    EXPECT_FLOAT_EQ(block.projparms[3], 0.0f);
    EXPECT_FLOAT_EQ(block.dirs[0][2], -1.0f);
}

TEST(OverlayLightingTest, AmbientLightIsBakedAndASmallDefaultIsSeededWithoutOne)
{
    Camera camera;
    makeForwardCamera(camera);

    const auto ambient = Light::createAmbient();
    ambient->setColor(vine::Colorf(0.1f, 0.2f, 0.3f, 1.0f));
    ambient->setIntensity(0.75f);
    const LightPushBlock lit = blockFor(&camera, { ambient.get() });
    EXPECT_FLOAT_EQ(lit.ambient[0], 0.1f);
    EXPECT_FLOAT_EQ(lit.ambient[1], 0.2f);
    EXPECT_FLOAT_EQ(lit.ambient[2], 0.3f);
    EXPECT_FLOAT_EQ(lit.ambient[3], 0.75f);

    // A pass that carries no ambient light would shade everything to black (ambient 0 x albedo),
    // so a small default keeps an unlit fullscreen program visible instead.
    const LightPushBlock unlit = blockFor(&camera, {});
    EXPECT_FLOAT_EQ(unlit.ambient[0], 0.15f);
    EXPECT_FLOAT_EQ(unlit.ambient[1], 0.15f);
    EXPECT_FLOAT_EQ(unlit.ambient[2], 0.15f);
    EXPECT_FLOAT_EQ(unlit.ambient[3], 1.0f);
}

TEST(OverlayLightingTest, DirectionalLightsAreBakedInViewSpace)
{
    Camera camera;
    makeForwardCamera(camera); // looks down -Z, up +Y

    const auto forward = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    forward->setColor(vine::Colorf(1.0f, 0.5f, 0.25f, 1.0f));
    forward->setIntensity(2.0f);
    // A non-unit direction is normalized: the shader uses it as a unit vector.
    const auto right = Light::createDirectional(vine::math::Vec3d(7.0, 0.0, 0.0));

    const LightPushBlock block = blockFor(&camera, { forward.get(), right.get() });

    // The G-buffer stores view-space normals, so the direction arrives already rotated: a light
    // shining away from the camera is -Z in view space, one from the right is +X.
    EXPECT_FLOAT_EQ(block.dirs[0][0], 0.0f);
    EXPECT_FLOAT_EQ(block.dirs[0][1], 0.0f);
    EXPECT_FLOAT_EQ(block.dirs[0][2], -1.0f);
    EXPECT_FLOAT_EQ(block.dirs[0][3], 0.0f);
    EXPECT_FLOAT_EQ(block.cols[0][0], 1.0f);
    EXPECT_FLOAT_EQ(block.cols[0][1], 0.5f);
    EXPECT_FLOAT_EQ(block.cols[0][2], 0.25f);
    EXPECT_FLOAT_EQ(block.cols[0][3], 2.0f);

    EXPECT_FLOAT_EQ(block.dirs[1][0], 1.0f);
    EXPECT_FLOAT_EQ(block.dirs[1][2], 0.0f);
    EXPECT_FLOAT_EQ(block.cols[1][3], 1.0f); // default intensity
}

TEST(OverlayLightingTest, DisabledAndNullLightsAreSkippedWithoutConsumingASlot)
{
    Camera camera;
    makeForwardCamera(camera);

    const auto disabled = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, -1.0));
    disabled->setEnabled(false);
    const auto first = Light::createDirectional(vine::math::Vec3d(0.0, 1.0, 0.0));

    // The light list is borrowed from the pass, so a null entry must be tolerated; a disabled
    // light must not spend one of the three slots.
    const LightPushBlock block = blockFor(&camera, { nullptr, disabled.get(), first.get() });
    EXPECT_FLOAT_EQ(block.dirs[0][1], 1.0f);
    EXPECT_FLOAT_EQ(block.dirs[1][1], 0.0f);
}

TEST(OverlayLightingTest, OnlyThreeDirectionalLightsAreBaked)
{
    Camera camera;
    makeForwardCamera(camera);

    std::vector<LightPtr> owned;
    for (int i = 0; i < 4; ++i) {
        owned.push_back(Light::createDirectional(vine::math::Vec3d(0.0, 1.0, 0.0)));
    }
    const std::vector<const Light*> lights{ owned[0].get(), owned[1].get(), owned[2].get(), owned[3].get() };
    const LightPushBlock            block = blockFor(&camera, lights);

    // Three slots, filled in order; the fourth directional light has nowhere to go (a documented
    // S4 limitation) and is ignored rather than overwriting a baked one.
    for (int i = 0; i < 3; ++i) {
        EXPECT_FLOAT_EQ(block.dirs[i][1], 1.0f);
        EXPECT_FLOAT_EQ(block.dirs[i][3], 0.0f);
    }
}
