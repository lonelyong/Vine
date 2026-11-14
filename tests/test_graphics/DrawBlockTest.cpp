/**
 * @brief The per-draw block's PACKING RULE (the SDK's one `packDrawBlock`), device-free.
 *
 * The first case packs a NON-SYMMETRIC matrix on purpose. A transposed packing is invisible with an identity, a
 * scale or any other symmetric matrix - which is most test content - and shows up later as "the model rotates the
 * wrong way / the object is somewhere else", which reads as a modelling bug. The values are chosen so every element
 * is distinct and its position in the flat array says which convention was used.
 *
 * The value PATH (host command → plan → bytes) is a different claim and lives in `tests/test_vsg/DrawBlockTest.cpp`,
 * where the frame compiler and recorder are available.
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <vine/graphics/DrawAbi.hpp>
#include <vine/math/Matrix4x4.hpp>

using vn::graphics::packDrawBlock;
using vn::graphics::VineDrawBlock;
using vn::math::Mat4d;

namespace
{

/// @brief A matrix whose element (row, column) is `row * 10 + column`: every element distinct, not symmetric.
Mat4d nonSymmetric()
{
    Mat4d matrix;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrix(row, column) = static_cast<double>(row * 10 + column);
        }
    }
    return matrix;
}

} // namespace

TEST(DrawBlockTest, TheModelMatrixLandsColumnMajorAndTheOpacityInTheParameterSlot)
{
    VineDrawBlock block;
    packDrawBlock(nonSymmetric(), 0.375F, 1.0F, block);

    // (row, column) at `column * 4 + row`: a row-major packing would put 1 where 10 belongs (and vice versa).
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const std::size_t index = static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row);
            EXPECT_FLOAT_EQ(block.model[index], static_cast<float>(row * 10 + column)) << "row " << row << ", column " << column;
        }
    }

    // Translation lives in the fourth column, i.e. at the end of the flat array - the same fact seen from the bytes
    // rather than from the math.
    EXPECT_FLOAT_EQ(block.model[12], 3.0F) << "x translation";
    EXPECT_FLOAT_EQ(block.model[13], 13.0F) << "y translation";
    EXPECT_FLOAT_EQ(block.model[14], 23.0F) << "z translation";
    EXPECT_FLOAT_EQ(block.model[15], 33.0F);

    EXPECT_FLOAT_EQ(block.params[0], 0.375F);
    EXPECT_FLOAT_EQ(block.params[1], 1.0F); // the point size: one pixel, not zero
    EXPECT_FLOAT_EQ(block.params[2], 0.0F); // the reserved user slot packs as zero, not as a leftover
    EXPECT_FLOAT_EQ(block.params[3], 0.0F);
}

TEST(DrawBlockTest, ThePointSizeTravelsBesideTheOpacityWithoutTouchingIt)
{
    // A resolved point size is DATA (the vertex stage writes it to gl_PointSize), so it never becomes a pipeline's
    // business - and it must not disturb the opacity it shares a row with.
    VineDrawBlock block;
    packDrawBlock(Mat4d{}, 0.375F, 4.5F, block);

    EXPECT_FLOAT_EQ(block.params[1], 4.5F);
    EXPECT_FLOAT_EQ(block.params[0], 0.375F) << "the opacity is untouched";
}

TEST(DrawBlockTest, EveryFieldIsWrittenIncludingTheReservedSlot)
{
    // A block left as whatever the previous owner of these bytes wrote is a shader reading another drawable's
    // values, so the reserved lanes are written as zero on every call.
    VineDrawBlock block;
    block.params = { 9.0F, 9.0F, 9.0F, 9.0F };

    packDrawBlock(Mat4d{}, 0.5F, 2.0F, block);

    EXPECT_FLOAT_EQ(block.params[2], 0.0F);
    EXPECT_FLOAT_EQ(block.params[3], 0.0F);
    EXPECT_FLOAT_EQ(block.params[0], 0.5F);
    EXPECT_FLOAT_EQ(block.params[1], 2.0F);
}
