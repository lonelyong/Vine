#include "ResidencyTable.hpp"

#include <vine/graphics/ContentId.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <type_traits>
#include <vector>

using vn::graphics::ContentRef;
using vn::graphics::ContentRegistry;
using vn::graphics::GeometryId;
using vn::graphics::InstanceId;
using vn::graphics::MaterialId;
using vn::graphics::NodeId;
using vn::graphics::detail::ResidencyTable;

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

TEST(ResidencyTableTest, TheCollectableSetIsEnumerable)
{
    // T6: given the oldest live snapshot epoch, the collectable set is a list - no time window and no kept fallback.
    ContentRegistry registry{ 0 };
    const auto      geometry = registry.allocate<GeometryId::tag_type>();
    const auto      material = registry.allocate<MaterialId::tag_type>();
    const auto      texture  = registry.allocate<NodeId::tag_type>();

    ResidencyTable table;
    EXPECT_TRUE(table.touch(ContentRef{ geometry.handle(), 1U }, 10U));
    EXPECT_TRUE(table.touch(ContentRef{ material.handle(), 1U }, 20U));
    EXPECT_TRUE(table.touch(ContentRef{ texture.handle(), 1U }, 5U));
    EXPECT_FALSE(table.touch(ContentRef{}, 30U)) << "a null id is not a resource";
    EXPECT_EQ(table.liveCount(), 3U);

    const std::vector<ContentRef> collectable = table.collectable(20U);
    ASSERT_EQ(collectable.size(), 2U);
    EXPECT_NE(std::find(collectable.begin(), collectable.end(), ContentRef{ geometry.handle(), 1U }), collectable.end());
    EXPECT_NE(std::find(collectable.begin(), collectable.end(), ContentRef{ texture.handle(), 1U }), collectable.end());
    EXPECT_EQ(std::find(collectable.begin(), collectable.end(), ContentRef{ material.handle(), 1U }), collectable.end());

    EXPECT_EQ(table.reclaim(20U), 2U);
    EXPECT_EQ(table.liveCount(), 1U);
    EXPECT_TRUE(table.contains(ContentRef{ material.handle(), 1U }));
    EXPECT_FALSE(table.contains(ContentRef{ geometry.handle(), 1U }));
}

TEST(ResidencyTableTest, ARevisionIsPartOfTheKey)
{
    // A1: an old snapshot keeps drawing rev1 after the content publishes rev2; the table must not answer rev2 for
    // the old reference, and reclaiming rev1 must leave rev2 alone.
    ContentRegistry registry{ 0 };
    const auto      geometry = registry.allocate<GeometryId::tag_type>();
    const ContentRef first{ geometry.handle(), 1U };
    const ContentRef second{ geometry.handle(), 2U };

    ResidencyTable table;
    EXPECT_TRUE(table.touch(first, 10U));
    EXPECT_TRUE(table.touch(second, 30U));
    EXPECT_TRUE(table.contains(first));
    EXPECT_TRUE(table.contains(second));
    EXPECT_EQ(table.liveCount(), 2U);

    EXPECT_EQ(table.reclaim(25U), 1U);
    EXPECT_FALSE(table.contains(first));
    EXPECT_TRUE(table.contains(second)) << "the newer revision is still referenced by a live snapshot";
}

TEST(ResidencyTableTest, ATouchExtendsTheLifetime)
{
    ContentRegistry registry{ 0 };
    const auto      id = registry.allocate<MaterialId::tag_type>();
    const ContentRef ref{ id.handle(), 4U };

    ResidencyTable table;
    table.touch(ref, 10U);
    table.touch(ref, 30U);
    EXPECT_TRUE(table.collectable(25U).empty()) << "the newest epoch, not the first one, decides";
    EXPECT_EQ(table.reclaim(25U), 0U);
    EXPECT_TRUE(table.contains(ref));
}
