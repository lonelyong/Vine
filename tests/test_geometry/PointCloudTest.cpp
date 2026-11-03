#include <gtest/gtest.h>

#include <vine/geometry/PointCloud.hpp>

TEST(PointCloudTest, anEmptyCloudIsNotUsable)
{
    const vn::geometry::PointCloud cloud;

    EXPECT_FALSE(cloud.isValid());
    EXPECT_EQ(cloud.pointCount(), 0u);
    EXPECT_TRUE(cloud.positions().empty());
    EXPECT_FALSE(cloud.hasVolume());
    EXPECT_TRUE(cloud.aabb().isEmpty());
    EXPECT_EQ(cloud.shapeType(), vn::geometry::ShapeType::PointCloud);
    EXPECT_EQ(cloud.shapeKind(), vn::geometry::ShapeKind::PointCloud);
}

TEST(PointCloudTest, positionsAreSharedAsThreeScalarsPerPoint)
{
    vn::geometry::PointCloud cloud;
    cloud.setPositions({ { 1.0f, 2.0f, 3.0f }, { -1.0f, 0.0f, 0.5f } });

    ASSERT_TRUE(cloud.isValid());
    ASSERT_EQ(cloud.pointCount(), 2u);
    ASSERT_EQ(cloud.positions().size(), 2u);
    EXPECT_FLOAT_EQ(cloud.positions()[1].z, 0.5f);

    const auto storage = cloud.positionsBuffer();
    ASSERT_TRUE(storage);
    EXPECT_EQ(storage->size(), 6u);
    EXPECT_EQ(cloud.positions().data(), reinterpret_cast<const vn::geometry::PointCloud::Vec3*>(storage->view().data()));
}

TEST(PointCloudTest, normalsAndColorsAreOptionalAndIndependent)
{
    vn::geometry::PointCloud cloud;
    cloud.setPositions({ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } });

    EXPECT_TRUE(cloud.normals().empty());
    EXPECT_TRUE(cloud.colors().empty());

    cloud.setNormals({ { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } });
    cloud.setColors({ { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.5f } });

    ASSERT_EQ(cloud.normals().size(), 2u);
    ASSERT_EQ(cloud.colors().size(), 2u);
    EXPECT_FLOAT_EQ(cloud.normals()[1].x, 1.0f);
    EXPECT_FLOAT_EQ(cloud.colors()[1].w, 0.5f);

    // A channel that does not hold one entry per point is kept, and the consumers are the ones that drop it.
    cloud.setColors({ { 1.0f, 1.0f, 1.0f, 1.0f } });
    EXPECT_EQ(cloud.colors().size(), 1u);
    EXPECT_NE(cloud.colors().size(), cloud.pointCount());

    cloud.setNormals({});
    EXPECT_TRUE(cloud.normals().empty());
    EXPECT_EQ(cloud.pointCount(), 2u) << "changing a channel never moves the points";
}

TEST(PointCloudTest, theCachedBoxIsClearedWhenThePositionsChange)
{
    vn::geometry::PointCloud cloud;
    cloud.setPositions({ { -1.0f, -2.0f, -3.0f }, { 1.0f, 2.0f, 3.0f } });

    const auto computed = cloud.computeAabb();
    EXPECT_FALSE(computed.isEmpty());
    EXPECT_FLOAT_EQ(computed.min().x, -1.0f);
    EXPECT_FLOAT_EQ(computed.max().z, 3.0f);
    EXPECT_FALSE(cloud.aabb().isEmpty()) << "computeAabb() fills the cache";

    cloud.setPositions({ { 5.0f, 5.0f, 5.0f } });

    // The cached box would no longer describe the stored points, so it is cleared rather than kept.
    EXPECT_TRUE(cloud.aabb().isEmpty());
    EXPECT_FLOAT_EQ(cloud.computeAabb().min().y, 5.0f);
}

TEST(PointCloudTest, anEmptyCloudComputesTheZeroBox)
{
    vn::geometry::PointCloud cloud;

    const auto box = cloud.computeAabb();

    EXPECT_TRUE(box.isZero());
}
