#include <vine/Color.hpp>

#include <vine/Colorf.hpp>

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

TEST(ColorTest, InvertMutatesInPlaceAndTheCopyAgrees)
{
    Color c(0, 10, 20, 128);
    c.invert();
    EXPECT_EQ(c, Color(255, 245, 235, 128)) << "the channels complement, the alpha does not move";

    // The two spellings are one operation: the copy is defined through the in-place one, so they cannot drift.
    const Color again(0, 10, 20, 128);
    EXPECT_EQ(again.inverted(), c);
}

TEST(ColorTest, Presets)
{
    EXPECT_EQ(Color::kWhite, Color(255, 255, 255, 255));
    EXPECT_EQ(Color::kBlack, Color(0, 0, 0, 255));
    EXPECT_EQ(Color::kRed, Color(255, 0, 0, 255));
    EXPECT_EQ(Color::kGreen, Color(0, 255, 0, 255));
    EXPECT_EQ(Color::kBlue, Color(0, 0, 255, 255));
    EXPECT_EQ(Color::kTransparent, Color(0, 0, 0, 0));
}

TEST(ColorTest, HexTextIsOneTextPerValueAndTheEncodedHalf)
{
    EXPECT_EQ(Color(255, 0, 0, 255).toString(), String(u8"#ff0000"))
        << "an opaque colour does not write a redundant trailing alpha pair";
    EXPECT_EQ(Color(255, 0, 0, 254).toString(), String(u8"#ff0000fe"));
    EXPECT_EQ(Color(0x12, 0x34, 0x56, 0x78).toString(), String(u8"#12345678"));

    // The text is the ENCODED half: linear 0.5 red is the byte 188 (0xbc). A linear-light text would write 0.5, and
    // then the same string would mean two colours depending on who parses it.
    EXPECT_EQ(Colorf(0.5f, 0.0f, 0.0f, 1.0f).toColor().toString(), String(u8"#bc0000"));
}

TEST(ColorTest, HexTextParsesBothAlphaFormsAndForgetsTheCase)
{
    const auto with_alpha = Color::parse(String(u8"#FF0000FE"));
    ASSERT_TRUE(with_alpha.has_value());
    EXPECT_EQ(*with_alpha, Color(255, 0, 0, 254));

    const auto without_alpha = Color::parse(String(u8"#00Ff00"));
    ASSERT_TRUE(without_alpha.has_value());
    EXPECT_EQ(*without_alpha, Color(0, 255, 0, 255)) << "an absent alpha pair means opaque";
}

TEST(ColorTest, HexTextAcceptsTheShortFormsAndWritesThemBackInFull)
{
    const auto triple = Color::parse(String(u8"#CCC"));
    ASSERT_TRUE(triple.has_value());
    EXPECT_EQ(*triple, Color(204, 204, 204, 255)) << "each digit doubles to a pair";
    EXPECT_EQ(Color::parse(String(u8"#CCCCCC")), triple) << "the long form is the same colour";
    EXPECT_EQ(Color::parse(String(u8"#CCCCCCFF")), triple) << "... with an explicit opaque alpha pair";

    const auto mixed = Color::parse(String(u8"#c0F"));
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(*mixed, Color(204, 0, 255, 255));

    const auto quadruple = Color::parse(String(u8"#C0F8"));
    ASSERT_TRUE(quadruple.has_value());
    EXPECT_EQ(*quadruple, Color(204, 0, 255, 136)) << "the fourth digit is the alpha";

    // One canonical text: a short form is an accepted INPUT, never an output.
    EXPECT_EQ(triple->toString(), String(u8"#cccccc"));
    EXPECT_EQ(quadruple->toString(), String(u8"#cc00ff88"));
}

TEST(ColorTest, HexTextRoundTripsEveryValueItWrites)
{
    for (int alpha : { 0, 1, 128, 254, 255 })
    {
        const Color colour(17, 200, 3, static_cast<std::uint8_t>(alpha));
        const auto  parsed = Color::parse(colour.toString());
        ASSERT_TRUE(parsed.has_value()) << colour.toString().as_std_str();
        EXPECT_EQ(*parsed, colour);
    }
}

TEST(ColorTest, EverythingElseIsARefusedParseNotAGuess)
{
    for (const char8_t* text : { u8"",
                                 u8"#",
                                 u8"#cc",
                                 u8"#ccccc",
                                 u8"#ff0000f",
                                 u8"#ff00000",
                                 u8"#ff0000ff0",
                                 u8"ff0000",
                                 u8"0xff0000",
                                 u8"#gg0000",
                                 u8"#+f0000",
                                 u8"# ff0000",
                                 u8" #ff0000",
                                 u8"ff0000ff",
                                 u8"rgba(255,0,0,1)" })
    {
        EXPECT_FALSE(Color::parse(String(text)).has_value()) << String(text).as_std_str();
    }
}

TEST(ColorTest, TheOrderedFormsAreNotThePositionalOne)
{
    // The same integer is two different colours: that is why these entry points are named after their order.
    const Color argb = Color::fromArgb(0x80FF0000u);
    EXPECT_EQ(argb, Color(255, 0, 0, 128));

    const Color rgba = Color(0x80FF0000u);
    EXPECT_EQ(rgba, Color(128, 255, 0, 0));
    EXPECT_NE(argb, rgba);
}

TEST(ColorTest, BgrIsColorRefOrderAndIgnoresItsHighByte)
{
    EXPECT_EQ(Color::fromBgr(0x000000FFu), Color(255, 0, 0, 255)) << "red sits in the low byte";
    EXPECT_EQ(Color::fromBgr(0x00FF0000u), Color(0, 0, 255, 255)) << "blue sits in bits 16..23";
    EXPECT_EQ(Color::fromBgr(0x00A1B2C3u), Color(0xC3, 0xB2, 0xA1, 255)) << "COLORREF carries no coverage";
    EXPECT_EQ(Color::fromBgr(0xFF0000FFu), Color::fromBgr(0x000000FFu))
        << "the high byte is not part of the value";
}

TEST(ColorTest, TheOrderedFormsAreUsableInConstantExpressions)
{
    static_assert(Color::fromArgb(0xFF112233u) == Color(0x11, 0x22, 0x33, 0xFF));
    static_assert(Color::fromBgr(0x00332211u) == Color(0x11, 0x22, 0x33, 0xFF));
}
