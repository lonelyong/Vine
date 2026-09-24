/**
 * @brief The camera matrices a declared push block carries, filled by name (see `.ai/design/vsg-reimplementation.md`
 * §11.16ag, `api/ContentPush.hpp`).
 *
 * Device-free by construction: the input is the range the ENGINE's own program declares (scanned out of its
 * text, api/ProgramAbi) plus a camera snapshot, and the judgement is arithmetic:
 *
 *   * `pc.projection` is the SAME value `VineViewBlock.proj` carries - the projection folded into the device's
 *     clip convention (`foldToDeviceClip`, the one spelling of the fold, api/ViewBlock) - so a program that
 *     reads the push and one that reads the block cannot disagree about where the picture is;
 *   * `pc.modelView` is `view * model`, in that order: the drawable's model matrix is the RIGHT-hand factor,
 *     and a packer that composed them the other way would put every drawable at a transform nobody authored;
 *   * a member name nobody recognizes, or a known name of the wrong size, is refused WITH ITS NAME - the
 *     alternative is a shader shading with zeros, which no diagnostic would explain.
 *
 * The pixel half of the claim (a push-reading program drawn through the content pass, with a real camera)
 * lives in ContentPassTest; this file pins the arithmetic the pixels would otherwise have to infer.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/math/Matrix4x4.hpp>

#include <vine/vsg/api/ContentPush.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/api/ViewBlock.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>

using vn::graphics::ShaderProgram;
using vn::math::Mat4d;
using vn::vsg::AbiPushMember;
using vn::vsg::AbiPushRange;
using vn::vsg::buildProgramFacts;
using vn::vsg::canFillPushMember;
using vn::vsg::contentPushMemberName;
using vn::vsg::contentPushMemberOf;
using vn::vsg::ContentPushMember;
using vn::vsg::core::CameraSnapshot;
using vn::vsg::FactMiss;
using vn::vsg::foldToDeviceClip;
using vn::vsg::packContentPush;
using vn::vsg::ProgramAbi;
using vn::vsg::ProgramFacts;

namespace
{

/// @brief A translation (the math module has no factories: a matrix is written element by element).
Mat4d translation(double x, double y, double z)
{
    Mat4d matrix;
    matrix(0, 3) = x;
    matrix(1, 3) = y;
    matrix(2, 3) = z;
    return matrix;
}

/// @brief A uniform scale.
Mat4d scale(double factor)
{
    Mat4d matrix;
    matrix(0, 0) = factor;
    matrix(1, 1) = factor;
    matrix(2, 2) = factor;
    return matrix;
}

/// @brief A rotation about z.
Mat4d rotationZ(double radians)
{
    Mat4d matrix;
    const double cosine = std::cos(radians);
    const double sine   = std::sin(radians);
    matrix(0, 0)       = cosine;
    matrix(0, 1)       = -sine;
    matrix(1, 0)       = sine;
    matrix(1, 1)       = cosine;
    return matrix;
}

/// @brief A matrix with the SHAPE of a projection (a y and a z row that are not the identity's), so the clip
///        fold changes it. It does not claim to be a frustum - nothing here rasterises anything.
Mat4d projectionLike()
{
    Mat4d matrix;
    matrix(0, 0) = 1.3;
    matrix(1, 1) = 1.1;
    matrix(2, 2) = -1.02;
    matrix(2, 3) = -0.2;
    matrix(3, 2) = -1.0;
    return matrix;
}

/// @brief A camera whose view, projection and model are all non-trivial: a packer that dropped a sign,
///        transposed a matrix or composed the two in the other order cannot pass on a case like this.
CameraSnapshot makeCamera()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = translation(-1.5, 2.5, -4.0) * rotationZ(0.7);
    camera.projection = projectionLike();
    return camera;
}

/// @brief A model matrix that is its own thing: a scale, a rotation and a translation.
Mat4d makeModel()
{
    return translation(0.75, -0.25, 0.5) * rotationZ(-0.4) * scale(2.0);
}

/// @brief Reads one element of a column-major mat4 written as floats: element (row, column) sits at
///        `column * 4 + row`.
double elementOf(const std::vector<std::byte>& bytes, std::size_t matrix_offset, int row, int column)
{
    float value = 0.0F;
    std::memcpy(&value, bytes.data() + matrix_offset + (static_cast<std::size_t>(column) * 4U +
                                                        static_cast<std::size_t>(row)) * sizeof(float),
                sizeof(float));
    return static_cast<double>(value);
}

/// @brief Asserts the 64 bytes at @p matrix_offset are @p expected, column-major.
void expectMatrix(const std::vector<std::byte>& bytes, std::size_t matrix_offset, const Mat4d& expected)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(elementOf(bytes, matrix_offset, row, column), expected(row, column), 1e-6)
                << "element (" << row << ", " << column << ")";
        }
    }
}

/// @brief The push range the ENGINE's forward program declares (scanned out of its own text).
AbiPushRange enginePush()
{
    ProgramFacts facts;
    EXPECT_EQ(buildProgramFacts(*vn::graphics::forwardProgram(), facts), FactMiss::None);
    ProgramAbi abi;
    EXPECT_EQ(vn::vsg::scanProgramAbi(facts.shaders.vertex, facts.shaders.fragment, {}, abi), FactMiss::None);
    EXPECT_EQ(abi.pushes.size(), 1U);
    return abi.pushes.empty() ? AbiPushRange{} : abi.pushes.front();
}

}  // namespace

TEST(ContentPushTest, TheDeclaredNamesAreTheOnesTheProgramsWrite)
{
    EXPECT_EQ(contentPushMemberOf("projection"), ContentPushMember::Projection);
    EXPECT_EQ(contentPushMemberOf("modelView"), ContentPushMember::ModelView);
    EXPECT_EQ(contentPushMemberOf("tint"), ContentPushMember::Unknown);
    EXPECT_EQ(contentPushMemberOf(""), ContentPushMember::Unknown);

    EXPECT_STREQ(contentPushMemberName(ContentPushMember::Projection), "projection");
    EXPECT_STREQ(contentPushMemberName(ContentPushMember::ModelView), "modelView");

    // A size is part of the answer: `vec4 projection` is a declaration whose bytes are not a matrix.
    EXPECT_TRUE(canFillPushMember(AbiPushMember{ "projection", 0U, 64U }));
    EXPECT_TRUE(canFillPushMember(AbiPushMember{ "modelView", 64U, 64U }));
    EXPECT_FALSE(canFillPushMember(AbiPushMember{ "projection", 0U, 16U }));
    EXPECT_FALSE(canFillPushMember(AbiPushMember{ "tint", 0U, 16U }));
}

TEST(ContentPushTest, ThePushCarriesTheSameMatricesTheViewBlockDoes)
{
    const AbiPushRange range = enginePush();
    ASSERT_EQ(range.members.size(), 2U);

    const CameraSnapshot camera = makeCamera();
    const Mat4d         model  = makeModel();
    std::vector<std::byte> bytes;
    std::string_view       unhandled;
    ASSERT_TRUE(packContentPush(range, camera, model, bytes, unhandled));
    ASSERT_EQ(bytes.size(), 128U);
    EXPECT_TRUE(unhandled.empty());

    // `pc.projection == VineViewBlock.proj`: the fold is the view block's, spelled once (api/ViewBlock), so a
    // program that reads the push and one that reads the block agree about the clip convention.
    const vn::graphics::VineViewBlock block = vn::vsg::buildViewBlock(camera, 0.0F, 0U, 0U);
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(elementOf(bytes, 0U, row, column),
                        block.proj[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)], 1e-6)
                << "the push' projection must be the block's, element (" << row << ", " << column << ")";
        }
    }
    expectMatrix(bytes, 0U, foldToDeviceClip(camera.projection));

    // `pc.modelView == view * model`, in THAT order: the model matrix is the right-hand factor.
    expectMatrix(bytes, 64U, camera.view * model);
    // ... and the other order is a different matrix, which is why the order is worth pinning: a packer that
    // composed them backwards would pass a comparison against its own arithmetic but not this one.
    bool differs = false;
    const Mat4d backwards = model * camera.view;
    for (int column = 0; column < 4 && !differs; ++column)
    {
        for (int row = 0; row < 4 && !differs; ++row)
        {
            differs = std::abs(elementOf(bytes, 64U, row, column) - backwards(row, column)) > 1e-3;
        }
    }
    EXPECT_TRUE(differs) << "the fixture must be one where the two orders differ, or the case proves nothing";
}

TEST(ContentPushTest, TheModelMatrixIsTheDrawablesAndLandsWhereTheDrawableGoes)
{
    const AbiPushRange range = enginePush();
    ASSERT_EQ(range.members.size(), 2U);

    // The push' `modelView` applied to the object's origin is the drawable's place in VIEW space: this is the
    // arithmetic a pixel case would otherwise have to infer, spelled as a number.
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = translation(10.0, 0.0, 0.0);
    camera.projection = projectionLike();
    const Mat4d model = scale(2.0);

    std::vector<std::byte> bytes;
    std::string_view       unhandled;
    ASSERT_TRUE(packContentPush(range, camera, model, bytes, unhandled));

    // view * model maps the origin to (10, 0, 0); model * view maps it to (20, 0, 0) - the two orders differ
    // by the translation being scaled, which is exactly the bug a swapped composition is.
    EXPECT_NEAR(elementOf(bytes, 64U, 0, 3), 10.0, 1e-6);
    EXPECT_NEAR(elementOf(bytes, 64U, 1, 3), 0.0, 1e-6);
    EXPECT_NEAR(elementOf(bytes, 64U, 2, 3), 0.0, 1e-6);
    EXPECT_NEAR(elementOf(bytes, 64U, 0, 0), 2.0, 1e-6) << "the model's scale is not scaled by the view";
}

TEST(ContentPushTest, AMemberNobodyCanFillIsRefusedWithItsName)
{
    const CameraSnapshot camera = makeCamera();
    const Mat4d         model  = makeModel();
    std::vector<std::byte> bytes;
    std::string_view       unhandled;

    // A name this backend does not know: the program would shade with zeros, so it is refused - by name.
    AbiPushRange unknown;
    unknown.size    = 64U;
    unknown.members = { AbiPushMember{ "tint", 0U, 64U } };
    EXPECT_FALSE(packContentPush(unknown, camera, model, bytes, unhandled));
    EXPECT_EQ(unhandled, "tint");

    // A known name of the wrong size: `vec4 projection` is not a projection matrix.
    AbiPushRange wrong_size;
    wrong_size.size    = 32U;
    wrong_size.members = { AbiPushMember{ "projection", 0U, 16U }, AbiPushMember{ "modelView", 16U, 16U } };
    EXPECT_FALSE(packContentPush(wrong_size, camera, model, bytes, unhandled));
    EXPECT_EQ(unhandled, "projection");

    // A range nobody could size is not a range to write.
    AbiPushRange empty;
    EXPECT_FALSE(packContentPush(empty, camera, model, bytes, unhandled));
    EXPECT_TRUE(unhandled.empty());
}

TEST(ContentPushTest, ASnapshotWithNoCameraWritesZerosAndSaysSo)
{
    const AbiPushRange range = enginePush();
    ASSERT_EQ(range.members.size(), 2U);

    // "There is no view" is a value, not an error (the same fact buildViewBlock reads): the range is written,
    // defined and zero - so a pass that draws without a camera shades at the origin instead of shading with
    // whatever the last drawable pushed.
    CameraSnapshot camera;
    camera.present = false;
    std::vector<std::byte> bytes;
    std::string_view       unhandled;
    ASSERT_TRUE(packContentPush(range, camera, makeModel(), bytes, unhandled));
    ASSERT_EQ(bytes.size(), 128U);
    for (const std::byte entry : bytes)
    {
        EXPECT_EQ(entry, std::byte{ 0 });
    }
}
