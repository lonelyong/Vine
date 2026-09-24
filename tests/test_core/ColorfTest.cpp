#include <vine/Color.hpp>
#include <vine/Colorf.hpp>

#include <gtest/gtest.h>

using namespace vn;

TEST(ColorfTest, DefaultIsOpaqueWhite)
{
    Colorf c;
    EXPECT_FLOAT_EQ(c.r, 1.0f);
    EXPECT_FLOAT_EQ(c.g, 1.0f);
    EXPECT_FLOAT_EQ(c.b, 1.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST(ColorfTest, ToColorRoundsAndClamps)
{
    const Color c = Colorf{ 1.0f, 0.5f, -0.1f, 2.0f }.toColor();
    EXPECT_EQ(c.r, 255);
    EXPECT_EQ(c.g, 128);
    EXPECT_EQ(c.b, 0);
    EXPECT_EQ(c.a, 255);
}

TEST(ColorfTest, RoundTripThroughColor)
{
    const Color  u8(12, 34, 56, 78);
    const Colorf f = Colorf::fromColor(u8);
    EXPECT_NEAR(f.r, 12.0f / 255.0f, 1e-4f);
    EXPECT_NEAR(f.g, 34.0f / 255.0f, 1e-4f);
    EXPECT_NEAR(f.b, 56.0f / 255.0f, 1e-4f);
    EXPECT_NEAR(f.a, 78.0f / 255.0f, 1e-4f);

    EXPECT_EQ(f.toColor(), u8);
}

TEST(ColorfTest, ColorToColorf)
{
    const Colorf f = Color(255, 0, 0, 128).toColorf();
    EXPECT_NEAR(f.r, 1.0f, 1e-4f);
    EXPECT_NEAR(f.g, 0.0f, 1e-4f);
    EXPECT_NEAR(f.b, 0.0f, 1e-4f);
    EXPECT_NEAR(f.a, 128.0f / 255.0f, 1e-4f);
}
