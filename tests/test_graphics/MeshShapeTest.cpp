#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <vine/Object.hpp>
#include <vine/models/Primitive.hpp>
#include <vine/models/MixedMesh.hpp>
#include <vine/models/QuadMesh.hpp>
#include <vine/models/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

using vn::models::MixedMesh;
using vn::models::QuadMesh;
using vn::models::TriangleMesh;
using vn::math::Vec2f;
using vn::math::Vec3f;
using vn::math::Vec4f;

TEST(MeshShapeTest, AnIndexedMeshIsValidOnlyInWholeTriangles)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    EXPECT_FALSE(mesh->isValid()) << "a mesh with no vertices draws nothing and is not valid";
    EXPECT_EQ(mesh->triangleCount(), 0u);

    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    EXPECT_FALSE(mesh->hasIndices());
    EXPECT_TRUE(mesh->isValid()) << "three sequential vertices already form a triangle";
    EXPECT_EQ(mesh->triangleCount(), 1u);

    mesh->setIndices({ 0u, 1u });
    EXPECT_FALSE(mesh->isValid()) << "two indices do not name a triangle";

    mesh->setIndices({ 0u, 1u, 2u });
    EXPECT_TRUE(mesh->hasIndices());
    EXPECT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 1u);
    EXPECT_EQ(mesh->vertexCount(), 3u);

    mesh->setPositions({});
    mesh->setIndices({});
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);
    EXPECT_EQ(mesh->vertexCount(), 0u);
}

TEST(MeshShapeTest, ANonIndexedMeshIsValidOnlyInWholeTriangles)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);

    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    EXPECT_FALSE(mesh->hasIndices());
    EXPECT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 1u);
    EXPECT_EQ(mesh->vertexCount(), 3u);

    // The soup stores three consecutive vertices per triangle, so a vertex array that is not a multiple of three
    // cannot be drawn as triangles - it is not silently rounded down.
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0) });
    EXPECT_FALSE(mesh->isValid());

    mesh->setPositions({});
    EXPECT_FALSE(mesh->isValid());
    EXPECT_EQ(mesh->triangleCount(), 0u);
}

TEST(MeshShapeTest, QuadMeshUsesFourCornersAndTriangulates)
{
    auto mesh = vn::make_intrusive<QuadMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0) });
    mesh->setIndices({ 0u, 1u, 2u, 3u });

    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->quadCount(), 1u);
    const auto triangle_mesh = mesh->toTriangleMesh();
    ASSERT_TRUE(triangle_mesh->isValid());
    EXPECT_EQ(triangle_mesh->triangleCount(), 2u);
    EXPECT_EQ(vn::models::UInt32Array(triangle_mesh->indices().begin(), triangle_mesh->indices().end()),
              (vn::models::UInt32Array{ 0u, 1u, 2u, 0u, 2u, 3u }));
    ASSERT_EQ(triangle_mesh->vertexCount(), mesh->vertexCount());
    EXPECT_EQ(triangle_mesh->positions()[2], mesh->positions()[2]);
}

TEST(MeshShapeTest, QuadMeshConvertsSequentialCorners)
{
    auto mesh = vn::make_intrusive<QuadMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0) });

    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->vertexCount(), 4u);
    EXPECT_EQ(mesh->quadCount(), 1u);
    EXPECT_FALSE(mesh->hasIndices()) << "corners without an index array are sequential";
    EXPECT_TRUE(mesh->computeAabb().isValid());

    const auto triangle_mesh = mesh->toTriangleMesh();
    ASSERT_TRUE(triangle_mesh->isValid());
    EXPECT_EQ(vn::models::UInt32Array(triangle_mesh->indices().begin(), triangle_mesh->indices().end()),
              (vn::models::UInt32Array{ 0u, 1u, 2u, 0u, 2u, 3u }));

    mesh->setPositions({});
    EXPECT_EQ(mesh->vertexCount(), 0u);
    EXPECT_FALSE(mesh->aabb().isValid()) << "the cached box goes with the vertices it described";
    EXPECT_FALSE(mesh->isValid());
}

TEST(MeshShapeTest, QuadMeshRequiresWholeFacesOfFourCorners)
{
    auto mesh = vn::make_intrusive<QuadMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0), Vec3f(2, 0, 0) });
    EXPECT_FALSE(mesh->isValid()) << "five corners are not whole quads";
    EXPECT_THROW((void)mesh->toTriangleMesh(), std::invalid_argument);

    mesh->setIndices({ 0u, 1u, 2u, 9u });
    EXPECT_FALSE(mesh->isValid()) << "an index past the last vertex is not a face";
}

TEST(MeshShapeTest, MixedMeshValidatesOffsetsAndConvertsToTriangleMesh)
{
    auto mesh = vn::make_intrusive<MixedMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0), Vec3f(-1, 0, 0) });
    mesh->setFaceOffsets({ 0u, 4u, 5u });
    EXPECT_FALSE(mesh->isValid()) << "the second face has fewer than three corners";

    mesh->setFaceOffsets({ 0u, 4u, 3u, 5u });
    EXPECT_FALSE(mesh->isValid()) << "offsets must be nondecreasing and end at the corner count";
    EXPECT_THROW((void)mesh->toTriangleMesh(), std::invalid_argument);

    mesh->setFaceOffsets({ 0u, 5u });
    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->faceCount(), 1u);
    const auto triangle_mesh = mesh->toTriangleMesh();
    ASSERT_TRUE(triangle_mesh->isValid());
    EXPECT_EQ(triangle_mesh->triangleCount(), 3u);
    EXPECT_EQ(vn::models::UInt32Array(triangle_mesh->indices().begin(), triangle_mesh->indices().end()),
              (vn::models::UInt32Array{ 0u, 1u, 2u, 0u, 2u, 3u, 0u, 3u, 4u }));

    vn::intrusive_ptr<vn::Buffer<std::uint32_t>> shared_offsets(
        new vn::Buffer<std::uint32_t>(vn::models::UInt32Array{ 0u, 5u }));
    mesh->setFaceOffsets(shared_offsets);
    EXPECT_EQ(mesh->faceOffsetsBuffer().get(), shared_offsets.get());
    EXPECT_EQ(vn::models::UInt32Array(mesh->faceOffsets().begin(), mesh->faceOffsets().end()),
              (vn::models::UInt32Array{ 0u, 5u }));
}

TEST(MeshShapeTest, MixedMeshWithSeveralFacesConvertsEveryFace)
{
    auto mesh = vn::make_intrusive<MixedMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0), Vec3f(-1, 0, 0) });
    mesh->setIndices({ 0u, 1u, 2u, 0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u, 4u });
    mesh->setFaceOffsets({ 0u, 3u, 7u, 12u });

    ASSERT_TRUE(mesh->isValid());
    EXPECT_EQ(mesh->faceCount(), 3u);
    const auto triangle_mesh = mesh->toTriangleMesh();
    ASSERT_TRUE(triangle_mesh->isValid());
    EXPECT_EQ(triangle_mesh->triangleCount(), 1u + 2u + 3u) << "a triangle, a quad and a pentagon fan out";
}

TEST(MeshShapeTest, AnAttributeHangsOffTheSharedStorageNotTheCaller)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    mesh->setIndices({ 0u, 1u, 2u });

    // A setter takes the array by value and moves it into a buffer, so the caller's copy going out of scope leaves the
    // mesh reading its own storage.
    vn::models::Vec4fArray colors{ Vec4f(1, 0, 0, 1), Vec4f(0, 1, 0, 1), Vec4f(0, 0, 1, 1) };
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

TEST(MeshShapeTest, EmptyArraysClearEveryChannelAndTheBox)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(2, 0, 0), Vec3f(0, 2, 0) });
    mesh->setIndices({ 0u, 1u, 2u });
    mesh->setNormals({ Vec3f(0, 0, 1), Vec3f(0, 0, 1), Vec3f(0, 0, 1) });
    mesh->setTexcoords({ Vec2f(0, 0), Vec2f(1, 0), Vec2f(0, 1) });
    mesh->setColors({ Vec4f(1, 1, 1, 1), Vec4f(1, 1, 1, 1), Vec4f(1, 1, 1, 1) });
    ASSERT_TRUE(mesh->computeAabb().isValid()) << "three vertices have a box";

    mesh->setPositions({});
    mesh->setIndices({});
    mesh->setNormals({});
    mesh->setTexcoords({});
    mesh->setColors({});

    EXPECT_TRUE(mesh->positions().empty());
    EXPECT_TRUE(mesh->normals().empty());
    EXPECT_TRUE(mesh->texcoords().empty());
    EXPECT_TRUE(mesh->colors().empty());
    EXPECT_EQ(mesh->triangleCount(), 0u) << "the index array goes with the vertices it named";
    EXPECT_FALSE(mesh->aabb().isValid()) << "the cached box described vertices that are gone";
    EXPECT_FALSE(mesh->isValid()) << "a mesh without vertices is not drawable, whatever its index array says";
}

TEST(MeshShapeTest, TheCachedBoxFollowsTheVertices)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    ASSERT_TRUE(mesh->computeAabb().isValid());
    EXPECT_FLOAT_EQ(mesh->aabb().max().x, 1.0f);

    // Replacing the positions moves the box, so the cache must not survive it.
    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(4, 0, 0), Vec3f(0, 1, 0) });
    EXPECT_FALSE(mesh->aabb().isValid()) << "the cached box described the vertices as they were";
    EXPECT_FLOAT_EQ(mesh->computeAabb().max().x, 4.0f);
}

TEST(MeshShapeTest, TheRevisionIsReportedByTheHolderNotByASetter)
{
    auto mesh = vn::make_intrusive<TriangleMesh>();
    EXPECT_EQ(mesh->revision(), 0u);

    mesh->setPositions({ Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0) });
    mesh->setIndices({ 0u, 1u, 2u });
    EXPECT_EQ(mesh->revision(), 0u) << "geometry never advances the revision itself";
    EXPECT_EQ(mesh->positionsBuffer()->revision(), 0u) << "a replaced buffer is a new buffer, not an edited one";

    mesh->bumpRevision();
    EXPECT_EQ(mesh->revision(), 1u) << "the holder reports the change once it is complete";

    mesh->setRevision(7u);
    EXPECT_EQ(mesh->revision(), 7u);
}
