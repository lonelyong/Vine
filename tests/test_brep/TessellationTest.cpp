#include <gtest/gtest.h>

#include <vine/brep/Tessellation.hpp>
#include <vine/geometry/BrepShape.hpp>

using vn::brep::tessellate;
using vn::brep::tessellationSupported;

TEST(Tessellation, aBuildWithoutABackendProducesNoMesh)
{
    // The module carries the contract without a backend: the capability query says so, and a call produces nothing.
    EXPECT_FALSE(tessellationSupported());

    const vn::geometry::BrepShape shape;
    const auto                   mesh = tessellate(shape);

    EXPECT_FALSE(mesh);
}
