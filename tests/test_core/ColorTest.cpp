#include <vine/Color.hpp>

#include <gtest/gtest.h>

using namespace vn;

TEST(ColorTest, DefaultIsOpaqueWhite)
{
    Color c;
    EXPECT_EQ(c.r, 255);
    EXPECT_EQ(c.g, 255);
    EXPECT_EQ(c.b, 255);
    EXPECT_EQ(c.a, 255);
}

TEST(ColorTest, PackUnpack)
{
    const Color c(0x12, 0x34, 0x56, 0x78);
    EXPECT_EQ(c.toRgba32(), 0x12345678u);
    EXPECT_EQ(Color(0x12345678u), c);
}

TEST(ColorTest, Inverted)
{
    const Color c(0, 10, 20, 255);
    const Color inv = c.inverted();
    EXPECT_EQ(inv.r, 255);
    EXPECT_EQ(inv.g, 245);
    EXPECT_EQ(inv.b, 235);
    EXPECT_EQ(inv.a, 255);
}

TEST(ColorTest, Presets)
{
    EXPECT_EQ(Color::White, Color(255, 255, 255, 255));
    EXPECT_EQ(Color::Black, Color(0, 0, 0, 255));
    EXPECT_EQ(Color::Red, Color(255, 0, 0, 255));
    EXPECT_EQ(Color::Green, Color(0, 255, 0, 255));
    EXPECT_EQ(Color::Blue, Color(0, 0, 255, 255));
    EXPECT_EQ(Color::Transparent, Color(0, 0, 0, 0));
}
