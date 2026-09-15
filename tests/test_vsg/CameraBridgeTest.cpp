/**
 * @brief The camera bridge: what a Vine camera becomes on the vsg side.
 *
 * The engine culls with `Camera::projectionMatrix()` (Scene::collectRenderCommands) and the GPU renders
 * with the vsg projection this bridge builds, so the two have to describe ONE frustum. They do that by
 * agreeing on the parameters — which is why the bridge must take the whole orthographic window: built
 * from a height alone it produced a centred frustum, and a camera with an off-centre window (a tiled
 * view, one eye of a stereo pair) then culled against one frustum and rendered another.
 *
 * Nothing here needs a device: the bridge only builds vsg objects.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Camera.hpp>

#include <vine/vsg/CameraBridge.hpp>

#include <vsg/app/Camera.h>
#include <vsg/app/ProjectionMatrix.h>
#include <vsg/app/ViewMatrix.h>

using vine::graphics::Camera;
using vine::vsg::CameraBridge;

namespace
{
/// The bridge is a session member (VsgRendererPersistent::cameraBridge), not a namespace of free
/// functions, so the tests hold one.
CameraBridge& bridge()
{
    static CameraBridge instance;
    return instance;
}
} // namespace

TEST(CameraBridgeTest, TheOrthographicWindowReachesTheProjection)
{
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 10.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    // Off-centre on purpose: a symmetric window cannot tell "all four bounds" from "the height, centred".
    camera.setProjectionMatrixAsOrtho(-2.0, 6.0, -1.0, 3.0, 0.5, 100.0);

    const auto vsg_camera = bridge().create(&camera);
    ASSERT_NE(vsg_camera, nullptr);
    const auto ortho = vsg_camera->projectionMatrix.cast<::vsg::Orthographic>();
    ASSERT_NE(ortho, nullptr) << "an orthographic Vine camera must become a vsg::Orthographic";

    EXPECT_DOUBLE_EQ(ortho->left, -2.0);
    EXPECT_DOUBLE_EQ(ortho->right, 6.0);
    EXPECT_DOUBLE_EQ(ortho->bottom, -1.0);
    EXPECT_DOUBLE_EQ(ortho->top, 3.0);
    EXPECT_DOUBLE_EQ(ortho->nearDistance, 0.5);
    EXPECT_DOUBLE_EQ(ortho->farDistance, 100.0);
}

TEST(CameraBridgeTest, ThePerspectiveParametersReachTheProjection)
{
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(1.0, 2.0, 3.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsPerspective(50.0, 16.0 / 9.0, 0.25, 500.0);

    const auto vsg_camera = bridge().create(&camera);
    ASSERT_NE(vsg_camera, nullptr);
    const auto persp = vsg_camera->projectionMatrix.cast<::vsg::Perspective>();
    ASSERT_NE(persp, nullptr) << "a perspective Vine camera must become a vsg::Perspective";

    EXPECT_DOUBLE_EQ(persp->fieldOfViewY, 50.0) << "the VSG projection takes degrees, as the camera does";
    EXPECT_DOUBLE_EQ(persp->aspectRatio, 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(persp->nearDistance, 0.25);
    EXPECT_DOUBLE_EQ(persp->farDistance, 500.0);

    const auto look_at = vsg_camera->viewMatrix.cast<::vsg::LookAt>();
    ASSERT_NE(look_at, nullptr);
    EXPECT_EQ(look_at->eye, ::vsg::dvec3(1.0, 2.0, 3.0));
    EXPECT_EQ(look_at->center, ::vsg::dvec3(0.0, 0.0, 0.0));
    EXPECT_EQ(look_at->up, ::vsg::dvec3(0.0, 1.0, 0.0));
}

TEST(CameraBridgeTest, ApplyingAgainFollowsTheCameraAndItsProjectionKind)
{
    // A slot re-applies its camera every frame (VsgContentSlot::renderContentSlot), so the bridge has to
    // be able to switch the projection KIND in place: the camera can be turned from perspective to
    // orthographic between two frames, and keeping the vsg::Perspective would render the old kind with
    // the new parameters quietly ignored.
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 10.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsPerspective(45.0, 1.5, 0.1, 100.0);

    const auto vsg_camera = bridge().create(&camera);
    ASSERT_NE(vsg_camera, nullptr);
    ASSERT_NE(vsg_camera->projectionMatrix.cast<::vsg::Perspective>(), nullptr);

    camera.setProjectionMatrixAsOrtho(-1.0, 1.0, -0.5, 0.5, 0.1, 100.0);
    bridge().apply(&camera, vsg_camera);
    const auto ortho = vsg_camera->projectionMatrix.cast<::vsg::Orthographic>();
    ASSERT_NE(ortho, nullptr) << "the projection object must follow the camera's kind";
    EXPECT_DOUBLE_EQ(ortho->bottom, -0.5);

    // And back, with the new parameters (not the ones the vsg::Perspective happened to keep).
    camera.setProjectionMatrixAsPerspective(30.0, 2.0, 0.2, 200.0);
    bridge().apply(&camera, vsg_camera);
    const auto persp = vsg_camera->projectionMatrix.cast<::vsg::Perspective>();
    ASSERT_NE(persp, nullptr);
    EXPECT_DOUBLE_EQ(persp->fieldOfViewY, 30.0);
    EXPECT_DOUBLE_EQ(persp->aspectRatio, 2.0);
    EXPECT_DOUBLE_EQ(persp->nearDistance, 0.2);
    EXPECT_DOUBLE_EQ(persp->farDistance, 200.0);
}

TEST(CameraBridgeTest, ANullCameraOrCameraLeavesTheTargetAlone)
{
    // Both arguments are borrowed and either may be absent: creating from a null camera must still
    // return a usable vsg::Camera (the slot builds its view around it and applies the real camera
    // later), and applying a null camera must not touch what is there.
    const auto from_null = bridge().create(nullptr);
    ASSERT_NE(from_null, nullptr);

    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));
    camera.setProjectionMatrixAsPerspective(45.0, 1.0, 0.1, 100.0);
    bridge().apply(&camera, from_null);
    const auto persp = from_null->projectionMatrix.cast<::vsg::Perspective>();
    ASSERT_NE(persp, nullptr);
    EXPECT_DOUBLE_EQ(persp->fieldOfViewY, 45.0);

    bridge().apply(nullptr, from_null);
    EXPECT_DOUBLE_EQ(persp->fieldOfViewY, 45.0) << "a null camera must leave the projection as it is";
    bridge().apply(&camera, nullptr); // no target: a no-op, not a crash
}
