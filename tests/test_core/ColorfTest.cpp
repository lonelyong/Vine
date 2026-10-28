#include <vine/Color.hpp>
#include <vine/Colorf.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace vn;

TEST(ColorfTest, DefaultIsOpaqueWhite)
{
    Colorf c;
    EXPECT_FLOAT_EQ(c.r, 1.0f);
    EXPECT_FLOAT_EQ(c.g, 1.0f);
    EXPECT_FLOAT_EQ(c.b, 1.0f);
    EXPECT_FLOAT_EQ(c.a, 1.0f);
}

TEST(ColorfTest, ToColorClampsAndEncodes)
{
    const Color c = Colorf{ 1.0f, 0.5f, -0.1f, 2.0f }.toColor();
    EXPECT_EQ(c.r, 255) << "above white clamps";
    EXPECT_EQ(c.g, 188) << "linear 0.5 is the byte 188, not 128: the sRGB curve moved it";
    EXPECT_EQ(c.b, 0) << "below black clamps";
    EXPECT_EQ(c.a, 255);
}

TEST(ColorfTest, InvertedComplementsTheLightAndLeavesAlphaAlone)
{
    const Colorf inverted = Colorf{ 0.2f, 0.4f, 0.6f, 0.5f }.inverted();
    EXPECT_FLOAT_EQ(inverted.r, 0.8f);
    EXPECT_FLOAT_EQ(inverted.g, 0.6f);
    EXPECT_FLOAT_EQ(inverted.b, 0.4f);
    EXPECT_FLOAT_EQ(inverted.a, 0.5f) << "alpha is coverage, not light: it is not complemented";
}

TEST(ColorfTest, InvertMutatesInPlaceAndTheCopyAgrees)
{
    Colorf c{ 0.2f, 0.4f, 0.6f, 0.5f };
    c.invert();
    EXPECT_FLOAT_EQ(c.r, 0.8f);
    EXPECT_FLOAT_EQ(c.b, 0.4f);
    EXPECT_FLOAT_EQ(c.a, 0.5f);

    // The two spellings are one operation: the copy is defined through the in-place one, so they cannot drift.
    EXPECT_EQ((Colorf{ 0.2f, 0.4f, 0.6f, 0.5f }.inverted()), c);
}

TEST(ColorfTest, TheLinearComplementIsNotTheEncodedComplement)
{
    // 0.2 linear complements to 0.8 linear. The DISPLAY complement of the same colour is the encoded round trip
    // (#333333 -> #cccccc, which is 0.6039 light), and the sRGB curve is the difference - so an implementation that
    // produced the display answer here cannot pass.
    const Colorf linear  = Colorf{ 0.2f, 0.2f, 0.2f, 1.0f }.inverted();
    const Colorf display = Color(51, 51, 51, 255).inverted().toLinear();
    EXPECT_FLOAT_EQ(linear.r, 0.8f);
    EXPECT_NEAR(static_cast<double>(display.r), 0.6039, 5e-3) << "the display complement of the same colour";
    EXPECT_GT(std::fabs(linear.r - display.r), 0.15f) << "the two complements are visibly different";
}

TEST(ColorfTest, LerpIsTheComponentWiseMix)
{
    const Colorf black{ 0.0f, 0.0f, 0.0f, 1.0f };
    const Colorf white{ 1.0f, 1.0f, 1.0f, 0.0f };

    const Colorf quarter = Colorf::lerp(black, white, 0.25f);
    EXPECT_FLOAT_EQ(quarter.r, 0.25f);
    EXPECT_FLOAT_EQ(quarter.g, 0.25f);
    EXPECT_FLOAT_EQ(quarter.b, 0.25f);
    EXPECT_FLOAT_EQ(quarter.a, 0.75f) << "alpha fades with the colour: a cross-fade is this call alone";

    EXPECT_EQ(Colorf::lerp(black, white, 0.0f), black) << "t = 0 is the first colour";
    EXPECT_EQ(Colorf::lerp(black, white, 1.0f), white) << "t = 1 is the second";
    EXPECT_FLOAT_EQ(Colorf::lerp(black, white, 2.0f).r, 2.0f) << "t outside 0..1 extrapolates, it does not clamp";
}

TEST(ColorfTest, LerpBlendsLightNotSamples)
{
    // Halfway between black and white is 0.5 LIGHT, which a display shows as the byte 188. A blend of the encoded
    // samples would answer 128, so this pins the space the interpolation happens in.
    const Colorf half = Colorf::lerp(Colorf{ 0.0f, 0.0f, 0.0f }, Colorf{ 1.0f, 1.0f, 1.0f }, 0.5f);
    EXPECT_FLOAT_EQ(half.r, 0.5f);
    EXPECT_EQ(half.toColor().r, 188) << "0.5 light is 188 on screen; 128 would mean the samples were blended";
}

TEST(ColorfTest, ThePresetsAreTheSameNumbersInBothSpellings)
{
    // White, black, transparent and the primaries are the only colours whose numbers do not move between the two
    // spellings (0 and 1 are the sRGB curve's fixed points), which is why the vocabulary is shared instead of being
    // two sets that could drift.
    EXPECT_EQ(Colorf::kWhite, Colorf(1.0f, 1.0f, 1.0f, 1.0f));
    EXPECT_EQ(Colorf::kBlack, Colorf(0.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_EQ(Colorf::kTransparent, Colorf(0.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_EQ(Colorf::kRed, Colorf(1.0f, 0.0f, 0.0f, 1.0f));

    EXPECT_EQ(Colorf::kWhite.toColor(), Color::kWhite);
    EXPECT_EQ(Colorf::kBlack.toColor(), Color::kBlack);
    EXPECT_EQ(Colorf::kTransparent.toColor(), Color::kTransparent);
    EXPECT_EQ(Colorf::kRed.toColor(), Color::kRed);
    EXPECT_EQ(Colorf::kGreen.toColor(), Color::kGreen);
    EXPECT_EQ(Colorf::kBlue.toColor(), Color::kBlue);

    EXPECT_EQ(Color(255, 255, 255, 255).toLinear(), Colorf::kWhite) << "the ends decode to the presets, not near them";
    EXPECT_EQ(Color(0, 0, 0, 255).toLinear(), Colorf::kBlack);
}

TEST(ColorfTest, TheCurveIsSrgbNotAPowerOfTwoPointTwo)
{
    // A 2.2 power would answer 0.2195 (and 186 in the other direction); the sRGB curve is the standard one, and the
    // difference is two 8-bit steps - visible, so the wrong curve cannot pass as "close enough".
    EXPECT_NEAR(Color(128, 128, 128).toLinear().r, 0.21586f, 5e-4f) << "sRGB 128 decodes to 0.2159, not 0.2195";
    EXPECT_NEAR(Color(51, 51, 51).toLinear().r, 0.033105f, 5e-4f) << "the window's #333333 grey";
    EXPECT_EQ(Colorf(0.5f, 0.5f, 0.5f).toColor().r, 188) << "linear 0.5 encodes to 188";
}

TEST(ColorfTest, TheTwoSpacesAreExactInversesOverEveryByte)
{
    // Every 8-bit value must survive a decode-then-encode round trip: that is what makes the pair "the" bridge
    // between the two spellings rather than two independent approximations.
    for (int value = 0; value <= 255; ++value) {
        const auto  byte = static_cast<std::uint8_t>(value);
        const Color color(byte, byte, byte, byte);
        EXPECT_EQ(color.toLinear().toColor(), color) << "byte " << value << " did not survive the round trip";
    }
}

TEST(ColorfTest, AlphaCrossesTheBoundaryWithoutBeingEncoded)
{
    // Alpha is coverage, not light: it only loses its 8-bit scale, so a half-transparent colour stays
    // half-transparent in both directions while the channels around it are transformed.
    const Colorf decoded = Color(255, 255, 255, 128).toLinear();
    EXPECT_FLOAT_EQ(decoded.r, 1.0f);
    EXPECT_NEAR(decoded.a, 128.0f / 255.0f, 1e-6f) << "white's alpha is the fraction the byte names";
    EXPECT_EQ(Colorf(0.0f, 0.0f, 0.0f, 128.0f / 255.0f).toColor().a, 128);

    EXPECT_NEAR(Color(0, 0, 0, 64).toLinear().a, 64.0f / 255.0f, 1e-6f) << "a dark colour's alpha is untouched by the transfer function";
}
