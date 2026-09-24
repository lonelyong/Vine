/**
 * @brief The pass registry: the SDK's pass objects to the plan's pass numbers (see
 * `.ai/design/vsg-reimplementation.md` §11.16be, `api/PassRegistry.hpp`).
 *
 * Device-free by construction - the registry never dereferences an identity - so what is checked here is
 * the CONTRACT the whole frame drive keys on: the same object answers the same number for as long as it
 * is announced, two objects never share a number, and a released number is never handed out again. The
 * facade's own use of it (the warm-up that announces before any frame, and the frame that draws after it)
 * is what VsgBackendTest's device case drives on top of this.
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <vine/vsg/api/PassRegistry.hpp>

using vn::vsg::PassRegistry;

TEST(PassRegistryTest, AnAnnouncedPassKeepsItsNumberUntilItIsReleased)
{
    PassRegistry registry;

    int one = 0;
    int two = 0;

    const auto first = registry.adopt(&one);
    EXPECT_NE(first, 0U) << "0 is the plan's spelling of 'no pass'";
    EXPECT_EQ(registry.adopt(&one), first) << "the same object is the same pass, frame after frame";
    EXPECT_TRUE(registry.contains(&one));

    const auto second = registry.adopt(&two);
    EXPECT_NE(second, first) << "two passes never share a number";
    EXPECT_EQ(registry.live(), 2U);

    EXPECT_TRUE(registry.release(&one));
    EXPECT_FALSE(registry.release(&one)) << "the announcement is answered once";
    EXPECT_FALSE(registry.contains(&one));
    EXPECT_EQ(registry.live(), 1U);

    // A NEW object at the SAME address is a new pass: the released number may not come back - what it
    // keyed is still remembered elsewhere (see the header), so handing it to another pass would make that
    // memory describe the wrong one.
    const auto third = registry.adopt(&one);
    EXPECT_NE(third, first) << "a released number is never re-issued";
    EXPECT_NE(third, second);
    EXPECT_EQ(registry.live(), 2U);
}

TEST(PassRegistryTest, ANullIdentityIsNoPassAndChangesNothing)
{
    PassRegistry registry;

    EXPECT_EQ(registry.adopt(nullptr), 0U);
    EXPECT_EQ(registry.live(), 0U) << "a null advertisement registers nothing";
    EXPECT_FALSE(registry.release(nullptr));
    EXPECT_FALSE(registry.contains(nullptr));
}
