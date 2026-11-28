#include <vine/graphics/ContentId.hpp>

#include <gtest/gtest.h>

#include <type_traits>

using vn::graphics::ContentRegistry;
using vn::graphics::GeometryId;
using vn::graphics::InstanceId;
using vn::graphics::MaterialId;
using vn::graphics::NodeId;

static_assert(!std::is_same_v<GeometryId, MaterialId>, "different content kinds are different id types");
static_assert(!std::is_convertible_v<GeometryId, MaterialId>, "a GeometryId must not be usable as a MaterialId");

TEST(ContentRegistryTest, AStaleHandleAnswersGone)
{
    // T5: release moves the generation, so the old handle cannot resolve to the slot's next occupant.
    ContentRegistry registry{ 7 };
    const auto      first  = registry.allocate<NodeId::tag_type>();
    const auto      second = registry.allocate<GeometryId::tag_type>();
    ASSERT_FALSE(first.isNull());
    ASSERT_FALSE(second.isNull());
    EXPECT_TRUE(registry.alive(first.handle()));
    EXPECT_TRUE(registry.alive(second.handle()));

    registry.release(first.handle());
    EXPECT_FALSE(registry.alive(first.handle()));

    const auto reused = registry.allocate<NodeId::tag_type>();
    EXPECT_EQ(reused.handle().index(), first.handle().index()) << "the free slot is reused";
    EXPECT_NE(reused.handle().generation(), first.handle().generation()) << "but not with the stale generation";
    EXPECT_FALSE(registry.alive(first.handle()));
    EXPECT_TRUE(registry.alive(reused.handle()));
    EXPECT_TRUE(registry.alive(second.handle()));
    EXPECT_EQ(registry.peakObjects(), 2U);
}

TEST(ContentRegistryTest, AHandleFromAnotherDomainNeverAnswers)
{
    ContentRegistry first_domain{ 0 };
    ContentRegistry second_domain{ 1 };
    const auto      id = first_domain.allocate<MaterialId::tag_type>();

    EXPECT_TRUE(first_domain.alive(id.handle()));
    EXPECT_FALSE(second_domain.alive(id.handle()));

    // Releasing through the wrong registry is a no-op, not a cross-domain free.
    second_domain.release(id.handle());
    EXPECT_TRUE(first_domain.alive(id.handle()));
}

TEST(ContentRegistryTest, ARepeatedReleaseDoesNotMoveTheGenerationTwice)
{
    ContentRegistry registry{ 0 };
    const auto      id = registry.allocate<InstanceId::tag_type>();
    registry.release(id.handle());
    registry.release(id.handle());
    EXPECT_FALSE(registry.alive(id.handle()));

    const auto next = registry.allocate<InstanceId::tag_type>();
    EXPECT_EQ(next.handle().index(), id.handle().index());
    EXPECT_EQ(next.handle().generation(), id.handle().generation() + 1U);
}
