#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::geometry::TriangleMesh;
using vn::math::Vec2f;
using vn::math::Vec3f;
using vn::math::Vec4f;

TEST(MeshShapeTest, AnIndexedMeshIsValidOnlyInWholeTriangles)
{
    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    EXPECT_FALSE(mesh->isValid()) << "a mesh with no vertices draws nothing and is not valid";
    EXPECT_EQ(mesh->triangleCount(), 0u);

    mesh->addVertex(Vec3f(0, 0, 0));
    mesh->addVertex(Vec3f(1, 0, 0));
    mesh->addVertex(Vec3f(0, 1, 0));
    EXPECT_FALSE(mesh->isValid()) << "vertices without a triangle draw nothing either";

    mesh->setIndices({ 0u, 1u });
    EXPECT_FALSE(mesh->isValid()) << "two indices do not name a triangle";

    mesh->setIndices({ 0u, 1u, 2u });
    EXPECT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 1u);
    EXPECT_EQ(mesh->vertexCount(), 3u);

    // appending is how a builder grows one: one triangle at a time
    mesh->addTriangle(2u, 1u, 0u);
    EXPECT_EQ(mesh->triangleCount(), 2u);

    mesh->clear();
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);
    EXPECT_EQ(mesh->vertexCount(), 0u);
}

TEST(MeshShapeTest, ANonIndexedMeshIsValidOnlyInWholeTriangles)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);

    mesh->addTriangle(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0));
    EXPECT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 1u);
    EXPECT_EQ(mesh->vertexCount(), 3u);

    // The soup stores three consecutive vertices per triangle, so a vertex array that is not a multiple of three
    // cannot be drawn as triangles - it is not silently rounded down.
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0) });
    EXPECT_FALSE(mesh->isValid());

    mesh->clear();
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);
}

TEST(MeshShapeTest, AnAttributeHangsOffTheSharedStorageNotTheCaller)
{
    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mesh->addVertex(Vec3f(0, 0, 0));
    mesh->addVertex(Vec3f(1, 0, 0));
    mesh->addVertex(Vec3f(0, 1, 0));
    mesh->addTriangle(0u, 1u, 2u);

    // A setter takes the array by value and moves it into a buffer, so the caller's copy going out of scope leaves the
    // mesh reading its own storage.
    vn::geometry::Vec4fArray colors{ Vec4f(1, 0, 0, 1), Vec4f(0, 1, 0, 1), Vec4f(0, 0, 1, 1) };
    mesh->setColors(colors);
    colors.clear();
    EXPECT_EQ(mesh->colors().size(), 3u);
    EXPECT_FLOAT_EQ(mesh->colors()[2].z, 1.0f);

    // Every attribute is optional and independent: stating one states nothing about the others.
    EXPECT_TRUE(mesh->normals().empty());
    EXPECT_TRUE(mesh->texcoords().empty());

    // An empty array REMOVES an attribute: "no colours" and "an empty colour array" are one state.
    mesh->setColors({});
    EXPECT_TRUE(mesh->colors().empty());
}

TEST(MeshShapeTest, ClearingAMeshEmptiesEveryChannelAndTheBox)
{
    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    mesh->addVertex(Vec3f(0, 0, 0));
    mesh->addVertex(Vec3f(2, 0, 0));
    mesh->addVertex(Vec3f(0, 2, 0));
    mesh->addTriangle(0u, 1u, 2u);
    mesh->setNormals({ Vec3f(0, 0, 1), Vec3f(0, 0, 1), Vec3f(0, 0, 1) });
    mesh->setTexcoords({ Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1) });
    mesh->setColors({ Vec4f(1, 1, 1, 1), Vec4f(1, 1, 1, 1), Vec4f(1, 1, 1, 1) });
    ASSERT_TRUE(mesh->computeAabb().isValid()) << "three vertices have a box";

    mesh->clear();

    EXPECT_TRUE(mesh->positions().empty());
    EXPECT_TRUE(mesh->normals().empty());
    EXPECT_TRUE(mesh->texcoords().empty());
    EXPECT_TRUE(mesh->colors().empty());
    EXPECT_EQ(mesh->triangleCount(), 0u) << "the index array goes with the vertices it named";
    EXPECT_FALSE(mesh->aabb().isValid()) << "the cached box described vertices that are gone";
    EXPECT_FALSE(mesh->isValid()) << "a mesh without vertices is not drawable, whatever its index array says";
}
