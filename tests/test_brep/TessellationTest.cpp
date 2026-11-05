#include <gtest/gtest.h>

#include <vine/brep/Tessellation.hpp>
#include <vine/models/BrepShape.hpp>

using vn::brep::hasBackend;
using vn::brep::tessellate;

TEST(Tessellation, aBuildWithoutABackendProducesNoMesh)
{
    if (hasBackend()) {
        GTEST_SKIP() << "this build has the backend";
    }

    // The module carries the contract without a backend: the capability query says so, and a call produces nothing.
    const vn::models::BrepShape shape;

    EXPECT_FALSE(tessellate(shape));
}

TEST(Tessellation, aSolidWithoutAShapeProducesNoMesh)
{
    // Holds in every build: an empty BrepShape has nothing to triangulate, backend or not.
    const vn::models::BrepShape shape;

    EXPECT_FALSE(tessellate(shape));
}
