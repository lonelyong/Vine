#include <gtest/gtest.h>
#include <vine/String.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

TEST(String, CaseConversion)
{
    const vn::String input(u8"AbC123");
    EXPECT_EQ(input.toLower().as_std_u8str(), std::u8string(u8"abc123"));
    EXPECT_EQ(input.toUpper().as_std_u8str(), std::u8string(u8"ABC123"));
    EXPECT_EQ(input.as_std_u8str(), std::u8string(u8"AbC123"));
}

TEST(String, TrimFunctions)
{
    vn::String a(u8"  \tHello\n");
    EXPECT_EQ(a.trimmedStart().as_std_u8str(), std::u8string(u8"Hello\n"));
    EXPECT_EQ(a.trimmedEnd().as_std_u8str(), std::u8string(u8"  \tHello"));
    EXPECT_EQ(a.trimmed().as_std_u8str(), std::u8string(u8"Hello"));

    a.trim();
    EXPECT_EQ(a.as_std_u8str(), std::u8string(u8"Hello"));
}

TEST(String, EqualsStartsWithEndsWith)
{
    const vn::String value(u8"AbCdEf");

    EXPECT_TRUE(value.isEqual(vn::String(u8"AbCdEf")));
    EXPECT_FALSE(value.isEqual(vn::String(u8"abcdef")));
    EXPECT_TRUE(value.isEqual(vn::String(u8"abcdef"), true));

    EXPECT_TRUE(value.startsWith(u8'A'));
    EXPECT_TRUE(value.startsWith(vn::String(u8"AbC")));
    EXPECT_TRUE(value.startsWith(vn::String(u8"abc"), true));
    EXPECT_FALSE(value.startsWith(vn::String(u8"xyz")));

    EXPECT_TRUE(value.endsWith(u8'f'));
    EXPECT_TRUE(value.endsWith(vn::String(u8"dEf")));
    EXPECT_TRUE(value.endsWith(vn::String(u8"DEF"), true));
    EXPECT_FALSE(value.endsWith(vn::String(u8"xyz")));
}

TEST(String, Hex)
{
    // Byte sequence -> lowercase hex string.
    const std::array<std::uint8_t, 3> bytes{ 0xBA, 0x78, 0x16 };
    EXPECT_EQ(vn::String::hex(bytes).as_std_u8str(), std::u8string(u8"ba7816"));

    // std::vector and edge values are accepted too.
    const std::vector<unsigned char> vec{ 0x00, 0x0F, 0xFF };
    EXPECT_EQ(vn::String::hex(vec).as_std_u8str(), std::u8string(u8"000fff"));

    // Empty input yields an empty string.
    EXPECT_EQ(vn::String::hex(std::span<const std::uint8_t>()).as_std_u8str(), std::u8string());
}

TEST(String, SplitBySingleDelimiter)
{
    const vn::String csv(u8"a,,b,");

    const auto keepEmpty = csv.split(u8',', true);
    ASSERT_EQ(keepEmpty.size(), 4u);
    EXPECT_EQ(keepEmpty[0].as_std_u8str(), std::u8string(u8"a"));
    EXPECT_EQ(keepEmpty[1].as_std_u8str(), std::u8string(u8""));
    EXPECT_EQ(keepEmpty[2].as_std_u8str(), std::u8string(u8"b"));
    EXPECT_EQ(keepEmpty[3].as_std_u8str(), std::u8string(u8""));

    const auto skipEmpty = csv.split(u8',', false);
    ASSERT_EQ(skipEmpty.size(), 2u);
    EXPECT_EQ(skipEmpty[0].as_std_u8str(), std::u8string(u8"a"));
    EXPECT_EQ(skipEmpty[1].as_std_u8str(), std::u8string(u8"b"));
}

TEST(String, SplitByDelimiterSet)
{
    const vn::String text(u8"a;b|c;;d|");
    const auto         parts = text.split({ u8';', u8'|' }, false);

    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0].as_std_u8str(), std::u8string(u8"a"));
    EXPECT_EQ(parts[1].as_std_u8str(), std::u8string(u8"b"));
    EXPECT_EQ(parts[2].as_std_u8str(), std::u8string(u8"c"));
    EXPECT_EQ(parts[3].as_std_u8str(), std::u8string(u8"d"));
}

TEST(String, SplitByStringDelimiter)
{
    const vn::String text(u8"a<->b<-><->c");
    const auto         keepEmpty = text.split(vn::String(u8"<->"), true);

    ASSERT_EQ(keepEmpty.size(), 4u);
    EXPECT_EQ(keepEmpty[0].as_std_u8str(), std::u8string(u8"a"));
    EXPECT_EQ(keepEmpty[1].as_std_u8str(), std::u8string(u8"b"));
    EXPECT_EQ(keepEmpty[2].as_std_u8str(), std::u8string(u8""));
    EXPECT_EQ(keepEmpty[3].as_std_u8str(), std::u8string(u8"c"));

    const auto identity = text.split(vn::String(u8""), false);
    ASSERT_EQ(identity.size(), 1u);
    EXPECT_EQ(identity[0].as_std_u8str(), text.as_std_u8str());
}

TEST(String, Utf16RoundTrip)
{
    const char16_t* utf16Data = u"A中😀";
    const vn::String s      = vn::String::fromUtf16(utf16Data);
    EXPECT_EQ(s.as_std_u8str(), std::u8string(u8"A中😀"));

    const auto utf16 = s.toUtf16();
    const std::u16string expected = u"A中😀";
    EXPECT_EQ(utf16, expected);
}

TEST(String, Utf32RoundTrip)
{
    const std::u32string source = U"A中😀";
    const vn::String   s      = vn::String::fromUtf32(source.c_str(), source.size());
    EXPECT_EQ(s.as_std_u8str(), std::u8string(u8"A中😀"));

    const auto utf32 = s.toUtf32();
    EXPECT_EQ(utf32, source);
}

TEST(String, NumericConversions)
{
    bool ok = false;
    EXPECT_EQ(vn::String(u8"-2A").toInt(&ok, 16), -42);
    EXPECT_TRUE(ok);

    EXPECT_EQ(vn::String(u8"abc").toInt(&ok), 0);
    EXPECT_FALSE(ok);

    // 超出 int 范围是失败，而不是回绕成另一个值。
    EXPECT_EQ(vn::String(u8"99999999999").toInt(&ok), 0);
    EXPECT_FALSE(ok);
    EXPECT_EQ(vn::String(u8"2147483647").toInt(&ok), 2147483647);
    EXPECT_TRUE(ok);

    const double v = vn::String(u8"3.125").toDouble(&ok);
    EXPECT_TRUE(ok);
    EXPECT_DOUBLE_EQ(v, 3.125);

    EXPECT_DOUBLE_EQ(vn::String(u8"not-a-number").toDouble(&ok), 0.0);
    EXPECT_FALSE(ok);
}

TEST(String, Local8BitAsciiRoundTrip)
{
    const char* plain = "Hello-123";
    const auto  s = vn::String::fromLocal8Bit(plain);
    EXPECT_EQ(s.as_std_u8str(), std::u8string(u8"Hello-123"));
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

    vn::String s(raw);
    const auto   utf32 = s.toUtf32();

    ASSERT_EQ(utf32.size(), 4u);
    EXPECT_EQ(utf32[0], 0xFFFDu);
    EXPECT_EQ(utf32[1], static_cast<char32_t>('('));
    EXPECT_EQ(utf32[2], 0xFFFDu);
    EXPECT_EQ(utf32[3], static_cast<char32_t>('A'));
}

TEST(String, OrderingIsLexicographicByCodePoint)
{
    const vn::String a(u8"abc");
    const vn::String b(u8"abd");
    const vn::String same(u8"abc");

    EXPECT_TRUE(a < b);
    EXPECT_TRUE(a <= b);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(b >= a);
    EXPECT_TRUE((a <=> b) < 0);
    EXPECT_TRUE((b <=> a) > 0);
    EXPECT_TRUE((a <=> same) == 0);
    EXPECT_TRUE(a <= same);
    EXPECT_TRUE(a >= same);

    // Byte order over UTF-8 is code point order: 'z' (U+007A) sorts before e-acute (U+00E9).
    EXPECT_TRUE(vn::String(u8"z") < vn::String(u8"\u00e9"));

    // A prefix sorts before its extension.
    EXPECT_TRUE(vn::String(u8"ab") < vn::String(u8"abc"));
}

TEST(String, HashMatchesEquality)
{
    const vn::String a(u8"hello");
    const vn::String b(u8"hello");
    const vn::String c(u8"jello");

    const std::hash<vn::String> hasher;
    EXPECT_EQ(hasher(a), hasher(b));
    EXPECT_NE(hasher(a), hasher(c)); // sanity: the hash is neither constant nor blind to the first bytes

    std::unordered_set<vn::String> set;
    set.insert(a);
    set.insert(b);
    EXPECT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.contains(b));
    EXPECT_FALSE(set.contains(c));
}

namespace
{
/** Compile-time probe: can a T be turned into a mutable std::u8string by an explicit cast?
 *  Written as a concept so the requirement is checked at instantiation, where an invalid cast
 *  is a substitution failure instead of a hard error.
 */
template <typename T>
concept CastableToMutableU8String = requires(T& value) { static_cast<std::u8string&>(value); };
} // namespace

TEST(String, StorageAccessIsReadOnly)
{
    vn::String text(u8"abc");

    static_assert(std::is_same_v<decltype(text.as_std_u8str()), const std::u8string&>);
    static_assert(std::is_same_v<decltype(text.std_u8str_view()), std::u8string_view>);
    static_assert(std::is_same_v<decltype(text.std_str_view()), std::string_view>);

    // Reading the storage as std::u8string happens implicitly; no mutable cross-type access exists,
    // neither implicitly nor through an explicit cast.
    static_assert(std::is_convertible_v<const vn::String&, const std::u8string&>);
    static_assert(!std::is_convertible_v<vn::String&, std::u8string&>);
    static_assert(!CastableToMutableU8String<vn::String>);
    static_assert(CastableToMutableU8String<std::u8string>); // the probe does report a positive

    // Element-wise mutation stays available, but it cannot change the container's structure.
    static_assert(std::is_same_v<decltype(text.data()), char8_t*>);
    static_assert(std::is_constructible_v<vn::String, std::u8string&&>);
}

TEST(String, ViewIsZeroCopyAndNulTerminated)
{
    const vn::String text(u8"caf\u00e9");

    const std::string_view view = text.std_str_view();

    // Same bytes, no copy: the view points straight into the string's own buffer.
    EXPECT_EQ(reinterpret_cast<const void*>(view.data()), reinterpret_cast<const void*>(text.data()));
    EXPECT_EQ(view.size(), text.size());
    EXPECT_EQ(view, std::string_view("caf\xc3\xa9", 5));

    // NUL-terminated, because it points into std::u8string storage: safe for printf("%s").
    EXPECT_EQ(view.data()[view.size()], '\0');
}

TEST(String, U8ViewIsTheStorageItself)
{
    const vn::String text(u8"h\u00e9llo");

    const std::u8string_view u8view = text.std_u8str_view();

    EXPECT_EQ(u8view.size(), text.size());
    EXPECT_TRUE(u8view == std::u8string_view(u8"h\u00e9llo"));
    EXPECT_EQ(u8view.data(), text.as_std_u8str().data());
}

TEST(String, AsStdStringAliasesTheStorage)
{
    vn::String text(u8"abc");

    // The accessor reinterprets the storage, so both spellings address the same object.
    EXPECT_EQ(&text.as_std_str(), reinterpret_cast<const std::string*>(&text.as_std_u8str()));

    text += vn::String(u8"def");
    EXPECT_EQ(text.as_std_str(), "abcdef");

    text = vn::String(u8"xyz");
    EXPECT_EQ(text.as_std_str().size(), 3u);
}

TEST(String, StdStringAndU8StringLayOutIdentically)
{
    // Layout fingerprint behind as_std_str(): the two instantiations must lay their members out
    // identically for equal content when they live at the same address. This catches an
    // implementation change that keeps sizeof() equal but moves members around - exactly what the
    // static_asserts in String.hpp cannot see.
    //
    // Only the small-string shapes are compared. With a heap buffer the object holds a pointer to
    // a separately allocated block, so two independent allocations can never be at the same
    // address (ASan's quarantine guarantees they are not), and comparing those bytes would test
    // the allocator instead of the layout. The heap path is covered functionally below.
    //
    // The storage is zeroed before each placement-new and BOTH sides are built through the same
    // constructor (content pointer in, so no copy constructor): a constructor only writes the bytes
    // it owns, and comparing what two different construction paths happen to leave behind would test
    // the paths rather than the layout. Without the zeroing the comparison also read the
    // indeterminate tail of the small-string buffer, which made the outcome depend on the stack
    // contents left by earlier tests.
    //
    // The first word is skipped: with debug iterators (the MSVC Debug default) every container
    // starts with a _Container_proxy pointer, and two objects built at the same address are given
    // different proxies, so that word legitimately differs. It is also the ONE word the aliasing in
    // as_std_str() cannot care about - the content and the bookkeeping members all live after it,
    // which is exactly what the comparison below pins.
    constexpr std::size_t kIgnoredPrefix = sizeof(void*);
    const auto same_bytes = [](const char* narrow, const char8_t* wide) {
        static_assert(sizeof(std::string) == sizeof(std::u8string));
        static_assert(kIgnoredPrefix < sizeof(std::string));
        alignas(std::string) unsigned char storage[sizeof(std::string)]{};
        unsigned char                     snapshot[sizeof(std::string)];

        auto* a = ::new (static_cast<void*>(storage)) std::string(narrow);
        std::memcpy(snapshot, storage, sizeof(std::string));
        a->~basic_string();

        auto* b  = ::new (static_cast<void*>(storage)) std::u8string(wide);
        const bool identical = std::memcmp(snapshot + kIgnoredPrefix, storage + kIgnoredPrefix,
                                           sizeof(std::string) - kIgnoredPrefix) == 0;
        b->~basic_string();
        return identical;
    };

    EXPECT_TRUE(same_bytes("", u8"")) << "empty-string layout differs between std::string and std::u8string";
    EXPECT_TRUE(same_bytes("abcdefghijklmno", u8"abcdefghijklmno"))
        << "small-string layout differs between std::string and std::u8string";

    // Heap path: same bytes in, same bytes out through the alias.
    const vn::String long_text(std::u8string(64, u8'x'));
    EXPECT_EQ(long_text.as_std_str(), std::string(64, 'x'));
    EXPECT_EQ(long_text.as_std_str().size(), 64u);
}

TEST(String, AtThrowsOnOutOfRange)
{
    const vn::String text(u8"abc");

    EXPECT_EQ(text.at(1), u8'b');
    EXPECT_THROW(text.at(3), std::out_of_range);
}

TEST(String, IgnoreCaseFoldsAsciiOnly)
{
    // The folded comparison is a byte-wise ASCII fold: non-ASCII bytes are compared as-is, and
    // feeding them to std::tolower would be undefined (they are negative chars).
    EXPECT_TRUE(vn::String(u8"Header").isEqual(vn::String(u8"header"), true));
    EXPECT_TRUE(vn::String(u8"caf\u00e9").isEqual(vn::String(u8"caf\u00e9"), true));
    EXPECT_FALSE(vn::String(u8"caf\u00e9").isEqual(vn::String(u8"CAF\u00c9"), true));
    EXPECT_TRUE(vn::String(u8"Content-Type").startsWith(vn::String(u8"content"), true));
    EXPECT_TRUE(vn::String(u8"file.XML").endsWith(vn::String(u8".xml"), true));
}
TEST(String, FromUtf8CopiesTheBytesVerbatim)
{
    // The narrow-to-String boundary the project crosses when a std::string-shaped API hands text over: the bytes are
    // copied as they are (they are UTF-8 already), so the round trip through as_std_str() is the identity.
    const std::string narrow = "中文 and ascii";
    const vn::String  text   = vn::String::fromUtf8(narrow);
    EXPECT_EQ(text.as_std_str(), narrow);
    EXPECT_EQ(text.size(), narrow.size());

    // Not a C string: an embedded NUL must survive (the old hand-rolled helpers spelled c_str() and truncated here).
    const std::string with_nul("a\0b", 3);
    EXPECT_EQ(vn::String::fromUtf8(with_nul).size(), 3u);

    // Nothing to transcode, nothing to validate: bytes that are not valid UTF-8 pass through unchanged.
    const std::string invalid("\xC3\x28", 2);
    EXPECT_EQ(vn::String::fromUtf8(invalid).as_std_str(), invalid);

    EXPECT_TRUE(vn::String::fromUtf8(std::string_view{}).empty());
    EXPECT_TRUE(vn::String::fromUtf8("").empty());
}
