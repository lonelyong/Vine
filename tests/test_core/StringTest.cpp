#include <gtest/gtest.h>
#include <vine/String.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST(String, CaseConversion)
{
    const vine::String input(u8"AbC123");
    EXPECT_EQ(input.toLower().stdu8str(), std::u8string(u8"abc123"));
    EXPECT_EQ(input.toUpper().stdu8str(), std::u8string(u8"ABC123"));
    EXPECT_EQ(input.stdu8str(), std::u8string(u8"AbC123"));
}

TEST(String, TrimFunctions)
{
    vine::String a(u8"  \tHello\n");
    EXPECT_EQ(a.trimmedStart().stdu8str(), std::u8string(u8"Hello\n"));
    EXPECT_EQ(a.trimmedEnd().stdu8str(), std::u8string(u8"  \tHello"));
    EXPECT_EQ(a.trimmed().stdu8str(), std::u8string(u8"Hello"));

    a.trim();
    EXPECT_EQ(a.stdu8str(), std::u8string(u8"Hello"));
}

TEST(String, EqualsStartsWithEndsWith)
{
    const vine::String value(u8"AbCdEf");

    EXPECT_TRUE(value.isEqual(vine::String(u8"AbCdEf")));
    EXPECT_FALSE(value.isEqual(vine::String(u8"abcdef")));
    EXPECT_TRUE(value.isEqual(vine::String(u8"abcdef"), true));

    EXPECT_TRUE(value.startsWith(u8'A'));
    EXPECT_TRUE(value.startsWith(vine::String(u8"AbC")));
    EXPECT_TRUE(value.startsWith(vine::String(u8"abc"), true));
    EXPECT_FALSE(value.startsWith(vine::String(u8"xyz")));

    EXPECT_TRUE(value.endsWith(u8'f'));
    EXPECT_TRUE(value.endsWith(vine::String(u8"dEf")));
    EXPECT_TRUE(value.endsWith(vine::String(u8"DEF"), true));
    EXPECT_FALSE(value.endsWith(vine::String(u8"xyz")));
}

TEST(String, Hex)
{
    // Byte sequence -> lowercase hex string.
    const std::array<std::uint8_t, 3> bytes{ 0xBA, 0x78, 0x16 };
    EXPECT_EQ(vine::String::hex(bytes).stdu8str(), std::u8string(u8"ba7816"));

    // std::vector and edge values are accepted too.
    const std::vector<unsigned char> vec{ 0x00, 0x0F, 0xFF };
    EXPECT_EQ(vine::String::hex(vec).stdu8str(), std::u8string(u8"000fff"));

    // Empty input yields an empty string.
    EXPECT_EQ(vine::String::hex(std::span<const std::uint8_t>()).stdu8str(), std::u8string());
}

TEST(String, SplitBySingleDelimiter)
{
    const vine::String csv(u8"a,,b,");

    const auto keepEmpty = csv.split(u8',', true);
    ASSERT_EQ(keepEmpty.size(), 4u);
    EXPECT_EQ(keepEmpty[0].stdu8str(), std::u8string(u8"a"));
    EXPECT_EQ(keepEmpty[1].stdu8str(), std::u8string(u8""));
    EXPECT_EQ(keepEmpty[2].stdu8str(), std::u8string(u8"b"));
    EXPECT_EQ(keepEmpty[3].stdu8str(), std::u8string(u8""));

    const auto skipEmpty = csv.split(u8',', false);
    ASSERT_EQ(skipEmpty.size(), 2u);
    EXPECT_EQ(skipEmpty[0].stdu8str(), std::u8string(u8"a"));
    EXPECT_EQ(skipEmpty[1].stdu8str(), std::u8string(u8"b"));
}

TEST(String, SplitByDelimiterSet)
{
    const vine::String text(u8"a;b|c;;d|");
    const auto         parts = text.split({ u8';', u8'|' }, false);

    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0].stdu8str(), std::u8string(u8"a"));
    EXPECT_EQ(parts[1].stdu8str(), std::u8string(u8"b"));
    EXPECT_EQ(parts[2].stdu8str(), std::u8string(u8"c"));
    EXPECT_EQ(parts[3].stdu8str(), std::u8string(u8"d"));
}

TEST(String, SplitByStringDelimiter)
{
    const vine::String text(u8"a<->b<-><->c");
    const auto         keepEmpty = text.split(vine::String(u8"<->"), true);

    ASSERT_EQ(keepEmpty.size(), 4u);
    EXPECT_EQ(keepEmpty[0].stdu8str(), std::u8string(u8"a"));
    EXPECT_EQ(keepEmpty[1].stdu8str(), std::u8string(u8"b"));
    EXPECT_EQ(keepEmpty[2].stdu8str(), std::u8string(u8""));
    EXPECT_EQ(keepEmpty[3].stdu8str(), std::u8string(u8"c"));

    const auto identity = text.split(vine::String(u8""), false);
    ASSERT_EQ(identity.size(), 1u);
    EXPECT_EQ(identity[0].stdu8str(), text.stdu8str());
}

TEST(String, Utf16RoundTrip)
{
    const char16_t* utf16Data = u"A中😀";
    const vine::String s      = vine::String::fromUtf16(utf16Data);
    EXPECT_EQ(s.stdu8str(), std::u8string(u8"A中😀"));

    const auto utf16 = s.toUtf16();
    const std::u16string expected = u"A中😀";
    EXPECT_EQ(utf16, expected);
}

TEST(String, Utf32RoundTrip)
{
    const std::u32string source = U"A中😀";
    const vine::String   s      = vine::String::fromUtf32(source.c_str(), source.size());
    EXPECT_EQ(s.stdu8str(), std::u8string(u8"A中😀"));

    const auto utf32 = s.toUtf32();
    EXPECT_EQ(utf32, source);
}

TEST(String, NumericConversions)
{
    bool ok = false;
    EXPECT_EQ(vine::String(u8"-2A").toInt(&ok, 16), -42);
    EXPECT_TRUE(ok);

    EXPECT_EQ(vine::String(u8"abc").toInt(&ok), 0);
    EXPECT_FALSE(ok);

    // 超出 int 范围是失败，而不是回绕成另一个值。
    EXPECT_EQ(vine::String(u8"99999999999").toInt(&ok), 0);
    EXPECT_FALSE(ok);
    EXPECT_EQ(vine::String(u8"2147483647").toInt(&ok), 2147483647);
    EXPECT_TRUE(ok);

    const double v = vine::String(u8"3.125").toDouble(&ok);
    EXPECT_TRUE(ok);
    EXPECT_DOUBLE_EQ(v, 3.125);

    EXPECT_DOUBLE_EQ(vine::String(u8"not-a-number").toDouble(&ok), 0.0);
    EXPECT_FALSE(ok);
}

TEST(String, Local8BitAsciiRoundTrip)
{
    const char* plain = "Hello-123";
    const auto  s = vine::String::fromLocal8Bit(plain);
    EXPECT_EQ(s.stdu8str(), std::u8string(u8"Hello-123"));
    EXPECT_EQ(s.toLocal8Bit(), std::string("Hello-123"));
}

TEST(String, InvalidUtf8DoesNotSkipFollowingByte)
{
    // Invalid 3-byte lead (0xE2) followed by non-continuation byte '(' and then 'A'.
    // Expected decoding: U+FFFD, '(', U+FFFD, 'A'.
    std::u8string raw;
    raw.push_back(static_cast<char8_t>(0xE2));
    raw.push_back(static_cast<char8_t>(0x28));
    raw.push_back(static_cast<char8_t>(0xA1));
    raw.push_back(static_cast<char8_t>('A'));

    vine::String s(raw);
    const auto   utf32 = s.toUtf32();

    ASSERT_EQ(utf32.size(), 4u);
    EXPECT_EQ(utf32[0], 0xFFFDu);
    EXPECT_EQ(utf32[1], static_cast<char32_t>('('));
    EXPECT_EQ(utf32[2], 0xFFFDu);
    EXPECT_EQ(utf32[3], static_cast<char32_t>('A'));
}