/**
 * @brief The view block: the per-view ABI values a pass' draws read (see `.ai/design/vsg-reimplementation.md`
 * §11.16o, milestone M3d-3c).
 *
 * Device-free by construction: the builder turns a camera snapshot plus the frame's time and extent into the
 * ABI's block, and every claim it makes is arithmetic.
 *
 * Why the cases look the way they do. Four of the block's contents are conventions rather than values a
 * caller happens to have, and each fails silently:
 *
 *   * the matrices are column-major - a transposed write is invisible on any symmetric matrix (the identity,
 *     a scale), so the case uses an ASYMMETRIC view and projection;
 *   * the clip-space matrices carry the DEVICE's convention (reverse-Z, y-down) rather than the SDK's, a
 *     fold that a wrong sign in turns into geometry the depth test rejects (no error, just a picture without
 *     its content) or an upside-down scene - so one case pins the fold arithmetically, on the SDK matrices
 *     themselves and on the block;
 *   * `view_proj` is `proj * view` and `inv_view` is the view's inverse - a wrong composition places the
 *     scene somewhere plausible rather than refusing;
 *   * `frame` and `cam_pos` carry reserved slots, which are packed as zero because "there is a place for it"
 *     is how per-view values quietly stop reaching shaders.
 *
 * The extent is the TARGET's (the picture the shading reconstructs screen-space data from), which is the
 * reason it is an argument here rather than something read off a pass' viewport.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/vsg/api/ViewBlock.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>

using vine::graphics::VineViewBlock;
using vine::math::Mat4d;
using vine::math::Vec3d;
using vine::vsg::buildViewBlock;
using vine::vsg::core::CameraSnapshot;

namespace
{

/// @brief The fold the block applies to the SDK's clip-space matrices (spelled out here on purpose).
Mat4d deviceClip()
{
    Mat4d fold;
    fold.element(1, 1) = -1.0;   // y-down NDC
    fold.element(2, 2) = -0.5;   // reverse-Z: the SDK's near (-1) becomes 1, its far (+1) becomes 0
    fold.element(2, 3) = 0.5;
    return fold;
}

/// @brief An asymmetric, invertible matrix (a rotation about z, a scale and a translation).
Mat4d asymmetricMatrix()
{
    Mat4d matrix;
    matrix.element(0, 0) = 0.0;
    matrix.element(0, 1) = -2.0;
    matrix.element(0, 2) = 0.0;
    matrix.element(0, 3) = 0.5;
    matrix.element(1, 0) = 1.5;
    matrix.element(1, 1) = 0.0;
    matrix.element(1, 2) = 0.0;
    matrix.element(1, 3) = -0.25;
    matrix.element(2, 0) = 0.0;
    matrix.element(2, 1) = 0.0;
    matrix.element(2, 2) = 3.0;
    matrix.element(2, 3) = 1.0;
    matrix.element(3, 0) = 0.0;
    matrix.element(3, 1) = 0.0;
    matrix.element(3, 2) = 0.0;
    matrix.element(3, 3) = 1.0;
    return matrix;
}

/// @brief A camera snapshot with everything a pass can announce.
CameraSnapshot camera()
{
    CameraSnapshot snapshot;
    snapshot.present    = true;
    snapshot.view       = asymmetricMatrix();
    snapshot.projection = asymmetricMatrix();
    snapshot.eye        = Vec3d(0.25, -0.5, 1.75);
    return snapshot;
}

/// @brief Compares a flat ABI matrix against a math matrix, element by element and by the ABI's order.
void expectColumnMajor(const std::array<float, 16>& flat, const Mat4d& matrix, const char* what)
{
    for (std::size_t column = 0; column < 4U; ++column)
    {
        for (std::size_t row = 0; row < 4U; ++row)
        {
            EXPECT_FLOAT_EQ(flat[column * 4U + row], static_cast<float>(matrix(row, column)))
                << what << ": element (" << row << ", " << column << ") is not at column * 4 + row";
        }
    }
}

/// @brief One point multiplied by a math matrix: the four clip-space components.
std::array<double, 4> clip(const Mat4d& matrix, double x, double y, double z)
{
    std::array<double, 4> out{};
    for (std::size_t row = 0; row < 4U; ++row)
    {
        out[row] = matrix(row, 0) * x + matrix(row, 1) * y + matrix(row, 2) * z + matrix(row, 3);
    }
    return out;
}

/// @brief The same, multiplied by an ABI matrix (column-major: element (row, column) at column * 4 + row).
std::array<double, 4> clipOf(const std::array<float, 16>& matrix, double x, double y, double z)
{
    std::array<double, 4> out{};
    for (std::size_t row = 0; row < 4U; ++row)
    {
        out[row] = static_cast<double>(matrix[row]) * x + static_cast<double>(matrix[4U + row]) * y +
                   static_cast<double>(matrix[8U + row]) * z + static_cast<double>(matrix[12U + row]);
    }
    return out;
}

}  // namespace

TEST(ViewBlockTest, TheMatricesAreColumnMajorAndComposedTheWayTheAbiSays)
{
    const CameraSnapshot snapshot = camera();
    const VineViewBlock  block    = buildViewBlock(snapshot, 2.5F, 640U, 360U);

    EXPECT_EQ(sizeof(VineViewBlock), 288U) << "the block the block storage expects (see BlockStorage::Layout)";
    expectColumnMajor(block.view, snapshot.view, "the view");
    expectColumnMajor(block.inv_view, snapshot.view.inverted(), "the inverse view");
    expectColumnMajor(block.proj, deviceClip() * snapshot.projection, "the projection");
    // The composition a vertex stage multiplies positions by: world -> clip is projection * view, with the
    // device's fold on the clip-space side.
    expectColumnMajor(block.view_proj, deviceClip() * snapshot.projection * snapshot.view, "view_proj");
    expectColumnMajor(block.view_proj, deviceClip() * (snapshot.projection * snapshot.view), "view_proj");

    // The inverse is an inverse, not a copy: view * view⁻¹ is the identity.
    EXPECT_TRUE((snapshot.view.inverted() * snapshot.view).isIdentity(1e-9))
        << "the snapshot's view has to be invertible for this case to mean anything";
}

TEST(ViewBlockTest, TheClipMatricesAreFoldedIntoTheDevicesReverseZAndYDownConvention)
{
    // A camera the SDK really produces, so the case starts from the convention the block has to convert:
    // looking down -Z from 2 units out, with an orthographic window of [-1, 1] and 0.5/4.0 planes.
    vine::graphics::Camera sdk_camera;
    sdk_camera.setViewMatrixAsLookAt(Vec3d(0.0, 0.0, 2.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));
    sdk_camera.setProjectionMatrixAsOrtho(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);

    CameraSnapshot snapshot;
    snapshot.present    = true;
    snapshot.view       = sdk_camera.viewMatrix();
    snapshot.projection = sdk_camera.projectionMatrix();
    snapshot.eye        = sdk_camera.eye();

    // What the SDK's own matrix says: a VIEW-space point on the near plane (0.5 in front of the eye, i.e.
    // view z = -0.5) lands at clip z = -1, and one on the far plane (view z = -4) at clip z = +1. This is the
    // INPUT fact - the assertions below would hide it if the fold and the SDK matrix had the same bug.
    const std::array<double, 4> sdk_near = clip(snapshot.projection, 0.0, 0.0, -0.5);
    const std::array<double, 4> sdk_far  = clip(snapshot.projection, 0.0, 0.0, -4.0);
    EXPECT_NEAR(sdk_near[2], -1.0, 1e-9) << "the SDK maps its near plane to clip z = -1";
    EXPECT_NEAR(sdk_far[2], 1.0, 1e-9) << "and its far plane to clip z = +1";

    const VineViewBlock block = buildViewBlock(snapshot, 0.0F, 128U, 96U);

    // What the block says (the device's convention): near maps to depth 1, far to 0 - so the window's
    // reverse-Z clear (0.0) plus its GREATER compare accept the near geometry and reject what is behind it.
    const std::array<double, 4> near_point = clipOf(block.view_proj, 0.0, 0.0, 1.5);
    const std::array<double, 4> far_point  = clipOf(block.view_proj, 0.0, 0.0, -2.0);
    EXPECT_NEAR(near_point[2] / near_point[3], 1.0, 1e-6) << "the device's near plane is depth 1";
    EXPECT_NEAR(far_point[2] / far_point[3], 0.0, 1e-6) << "and its far plane is depth 0";

    // The same fold on `proj` alone, asked in view space (its own space): 0.5 in front of the eye is the
    // near plane, 4.0 in front the far one.
    const std::array<double, 4> view_near = clipOf(block.proj, 0.0, 0.0, -0.5);
    const std::array<double, 4> view_far  = clipOf(block.proj, 0.0, 0.0, -4.0);
    EXPECT_NEAR(view_near[2] / view_near[3], 1.0, 1e-6);
    EXPECT_NEAR(view_far[2] / view_far[3], 0.0, 1e-6);

    // y: the device's NDC is y-down, so world up must come out NEGATIVE here (world up renders up); the SDK's
    // own matrix says the opposite, which is what the fold exists for.
    EXPECT_GT(clip(snapshot.projection, 0.0, 0.5, 1.5)[1], 0.0) << "the SDK's clip is y-up";
    EXPECT_LT(clipOf(block.view_proj, 0.0, 0.5, 1.5)[1], 0.0) << "the block's clip is y-down";
    EXPECT_GT(clipOf(block.view, 0.0, 0.5, 1.5)[1], 0.0)
        << "and the view matrix is NOT folded: view space is its own space";

    // x and w are the same in both conventions, so the fold cannot move the picture sideways.
    EXPECT_NEAR(clipOf(block.view_proj, 0.5, 0.0, 1.5)[0], 0.5, 1e-6);
    EXPECT_NEAR(clipOf(block.view_proj, 0.5, 0.0, 1.5)[3], 1.0, 1e-6);
}

TEST(ViewBlockTest, TheFrameCarriesTheTimeAndThePicturesExtentAndTheReservedSlotsAreZero)
{
    const CameraSnapshot snapshot = camera();
    const VineViewBlock  block    = buildViewBlock(snapshot, 12.25F, 1920U, 1080U);

    EXPECT_FLOAT_EQ(block.frame[0], 12.25F) << "frame.x is the frame's time in seconds";
    EXPECT_FLOAT_EQ(block.frame[1], 1920.0F) << "frame.y is the picture's width, in device pixels";
    EXPECT_FLOAT_EQ(block.frame[2], 1080.0F) << "frame.z is the picture's height";
    EXPECT_FLOAT_EQ(block.frame[3], 0.0F) << "frame.w is reserved: a value nobody defined must not appear";

    EXPECT_FLOAT_EQ(block.cam_pos[0], 0.25F) << "cam_pos.xyz is the camera's world position";
    EXPECT_FLOAT_EQ(block.cam_pos[1], -0.5F);
    EXPECT_FLOAT_EQ(block.cam_pos[2], 1.75F);
    EXPECT_FLOAT_EQ(block.cam_pos[3], 0.0F) << "cam_pos.w is reserved";
}

TEST(ViewBlockTest, NoCameraMeansNoViewButTheFramesFactsStay)
{
    // A pass with no camera has nothing to view; the honest answer is a block with no view in it - and the
    // frame's time and extent are still the frame's (they do not depend on a camera).
    const CameraSnapshot empty;
    const VineViewBlock  block = buildViewBlock(empty, 3.5F, 64U, 32U);

    for (const float value : block.view)
    {
        EXPECT_FLOAT_EQ(value, 0.0F);
    }
    for (const float value : block.view_proj)
    {
        EXPECT_FLOAT_EQ(value, 0.0F);
    }
    EXPECT_FLOAT_EQ(block.frame[0], 3.5F);
    EXPECT_FLOAT_EQ(block.frame[1], 64.0F);
    EXPECT_FLOAT_EQ(block.frame[2], 32.0F);
    EXPECT_FLOAT_EQ(block.cam_pos[3], 0.0F);
}
