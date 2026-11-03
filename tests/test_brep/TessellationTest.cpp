#include <gtest/gtest.h>

#include <vine/brep/Tessellation.hpp>
#include <vine/geometry/BrepShape.hpp>
#include <vine/io/IoError.hpp>

using vn::brep::tessellate;
using vn::io::IoError;

TEST(Tessellation, aSolidWithoutAShapeIsInvalidData)
{
    // A default BrepShape holds no backend shape. The contract names that InvalidData, and the answer does not
    // depend on whether the backend is built in.
    const vn::geometry::BrepShape shape;

    const auto result = tessellate(shape);

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error(), IoError::InvalidData);
}
