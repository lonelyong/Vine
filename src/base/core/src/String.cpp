#include <vine/String.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <ctype.h>
#include <cwchar>
#include <limits>
#include <locale>
#include <vector>

#ifdef _WIN32
#    include <windows.h>
#endif

#include <vine/Exception.hpp>

VN_CORE_NS_BEGIN

namespace
{

// The Unicode replacement character U+FFFD is used to replace invalid or unrepresentable code points during encoding/decoding.
// The character is �.
constexpr char32_t k_replacement_char = 0xFFFD;

char32_t decode_utf8_one(const std::u8string& src, size_t& index)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(src.data());
    const auto  size  = src.size();

    if (index >= size) {
        return k_replacement_char;
    }

    const unsigned char b0 = bytes[index++];

    // ASCII fast path: single byte (0xxxxxxx)
    if (b0 < 0x80 /*0b10000000*/) {
        return static_cast<char32_t>(b0);
    }

    // Multi-byte sequences must start with a byte indicating the length (110xxxxx for 2 bytes, 1110xxxx for 3 bytes, 11110xxx for 4 bytes)
    auto read_next = [&](unsigned char& out) -> bool {
        if (index >= size) {
            return false;
        }
        const unsigned char candidate = bytes[index];

        if ((candidate & 0xC0 /*0b11000000*/) != 0x80 /*0b10000000*/) {
            return false;
        }
        out = candidate;
        ++index;
        return true;
    };

    unsigned char b1 = 0;
    unsigned char b2 = 0;
    unsigned char b3 = 0;

    // 2-byte sequence (110xxxxx 10xxxxxx)
    if ((b0 & 0xE0 /*0b11100000*/) == 0xC0 /*0b11000000*/) {
        if (!read_next(b1)) {
            return k_replacement_char;
        }
        // Decode code point from 2 bytes
        // The first byte contributes 5 bits, the second byte contributes 6 bits, for a total of 11 bits (enough for code points up to U+7FF)
        const char32_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        return cp >= 0x80 ? cp : k_replacement_char;
    }

    // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
    if ((b0 & 0xF0 /*0b11110000*/) == 0xE0 /*0b11100000*/) {
        if (!read_next(b1) || !read_next(b2)) {
            return k_replacement_char;
        }
        // Decode code point from 3 bytes
        // The first byte contributes 4 bits, the next two bytes contribute 6 bits each, for a total of 16 bits (enough for code points up to U+FFFF)
        const char32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return k_replacement_char;
        }
        return cp;
    }

    // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
    if ((b0 & 0xF8 /*0b11111000*/) == 0xF0 /*0b11110000*/) {
        if (!read_next(b1) || !read_next(b2) || !read_next(b3)) {
            return k_replacement_char;
        }
        // Decode code point from 4 bytes
        // The first byte contributes 3 bits, the next three bytes contribute 6 bits each, for a total of 21 bits (enough for all Unicode code points)
        const char32_t cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        // Reject overlong encodings and code points outside the valid Unicode range
        // Valid code points are from U+0000 to U+10FFFF, and must not be encoded in more bytes than necessary
        if (cp < 0x10000 || cp > 0x10FFFF) {
            return k_replacement_char;
        }
        return cp;
    }

    return k_replacement_char;
}

void append_utf8(std::u8string& out, char32_t cp)
{
    // Encode a Unicode code point (char32_t) into UTF-8 and append it to the output string
    // UTF-8 encoding rules:
    // - For code points U+0000 to U+007F: 1 byte (0xxxxxxx)
    // - For code points U+0080 to U+07FF: 2 bytes (110xxxxx 10xxxxxx)
    // - For code points U+0800 to U+FFFF: 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
    // - For code points U+10000 to U+10FFFF: 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)

    // ascii fast path, 1 byte (0xxxxxxx)
    if (cp <= 0x7F /*0b01111111*/) {
        out.push_back(static_cast<char8_t>(cp));
        return;
    }

    // 2 bytes (110xxxxx 10xxxxxx)
    // For code points U+0080 to U+07FF
    if (cp <= 0x7FF) {
        out.push_back(static_cast<char8_t>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        return;
    }

    // 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
    // For code points U+0800 to U+FFFF
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = k_replacement_char;
        }
        out.push_back(static_cast<char8_t>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        return;
    }

    // invalid code points above U+10FFFF are replaced with the replacement character
    if (cp > 0x10FFFF) {
        cp = k_replacement_char;
    }

    // 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
    // For code points U+10000 to U+10FFFF
    out.push_back(static_cast<char8_t>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char8_t>(0x80 | (cp & 0x3F)));
}

} // namespace

// Specialization for char to use strlen
template <>
size_t cstrlen<char>(const char* data)
{
    return std::strlen(data);
}

// Specialization for char16_t
// On Windows wchar_t is 16-bit, on Linux/Unix it's typically 32-bit
template <>
size_t cstrlen<char16_t>(const char16_t* data)
{
#if defined(_WIN32) || defined(_WIN64)
    // Windows: wchar_t is 16-bit, safe to cast
    return std::wcslen(reinterpret_cast<const wchar_t*>(data));
#else
    // Linux/Unix: wchar_t is 32-bit, use generic version
    const char16_t* p = data;
    while (*p != 0) ++p;
    return p - data;
#endif
}

// Specialization for char32_t
// On Linux/Unix wchar_t is 32-bit, matching char32_t
template <>
size_t cstrlen<char32_t>(const char32_t* data)
{
#if !defined(_WIN32) && !defined(_WIN64)
    // Linux/Unix: wchar_t is 32-bit, safe to cast
    return std::wcslen(reinterpret_cast<const wchar_t*>(data));
#else
    // Windows: wchar_t is 16-bit, use generic version
    const char32_t* p = data;
    while (*p != 0) ++p;
    return p - data;
#endif
}

String String::toLower() const
{
    String result(*this);
    std::transform(result.stdstr_.begin(), result.stdstr_.end(), result.stdstr_.begin(), ::tolower);
    return result;
}

String String::toUpper() const
{
    String result(*this);
    std::transform(result.stdstr_.begin(), result.stdstr_.end(), result.stdstr_.begin(), ::toupper);
    return result;
}

String& String::trimStart()
{
    auto it = std::find_if_not(stdstr_.begin(), stdstr_.end(), ::isspace);
    stdstr_.erase(stdstr_.begin(), it);
    return *this;
}

String& String::trimEnd()
{
    auto it = std::find_if_not(stdstr_.rbegin(), stdstr_.rend(), ::isspace);
    stdstr_.erase(it.base(), stdstr_.end());
    return *this;
}

std::vector<String> String::split(value_type delimiter, bool keep_empty) const
{
    std::vector<String> result;
    size_type           start = 0;

    while (start <= stdstr_.size()) {
        const size_type pos = stdstr_.find(delimiter, start);
        const size_type end = (pos == impl_type::npos) ? stdstr_.size() : pos;
        const size_type len = end - start;

        if (keep_empty || len > 0) {
            result.emplace_back(stdstr_.substr(start, len));
        }

        if (pos == impl_type::npos) {
            break;
        }
        start = pos + 1;
    }

    return result;
}

std::vector<String> String::split(const std::initializer_list<value_type>& delimiters, bool keep_empty) const
{
    std::vector<String> result;
    size_type           start = 0;

    auto is_delimiter = [&](value_type ch) -> bool { return std::find(delimiters.begin(), delimiters.end(), ch) != delimiters.end(); };

    for (size_type i = 0; i <= stdstr_.size(); ++i) {
        const bool at_end    = (i == stdstr_.size());
        const bool split_now = at_end || is_delimiter(stdstr_[i]);

        if (!split_now) {
            continue;
        }

        const size_type len = i - start;
        if (keep_empty || len > 0) {
            result.emplace_back(stdstr_.substr(start, len));
        }
        start = i + 1;
    }

    return result;
}

std::vector<String> String::split(const String& delimiter, bool keep_empty) const
{
    if (delimiter.empty()) {
        return { *this };
    }

    std::vector<String> result;
    size_type           start = 0;

    while (start <= stdstr_.size()) {
        const size_type pos = stdstr_.find(delimiter.stdstr_, start);
        const size_type end = (pos == impl_type::npos) ? stdstr_.size() : pos;
        const size_type len = end - start;

        if (keep_empty || len > 0) {
            result.emplace_back(stdstr_.substr(start, len));
        }

        if (pos == impl_type::npos) {
            break;
        }

        start = pos + delimiter.size();
    }

    return result;
}

std::string String::toLocal8Bit() const
{
#if defined(_WIN32) || defined(_WIN64)
    const auto utf16 = toUtf16();
    if (utf16.empty()) {
        return {};
    }

    const int required =
        WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(required), '\0');
    const int   converted =
        WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(utf16.data()), static_cast<int>(utf16.size()), out.data(), required, nullptr, nullptr);
    if (converted != required) {
        return {};
    }
    return out;
#else
    return std::string(reinterpret_cast<const char*>(stdstr_.data()), stdstr_.size());
#endif
}

std::u16string String::toUtf16() const
{
    if (stdstr_.empty())
        return {};

    std::u16string out;
    out.reserve(stdstr_.size());

    size_t idx = 0;
    while (idx < stdstr_.size()) {
        // First decode one code point from UTF-8, then encode it into UTF-16
        char32_t cp = decode_utf8_one(stdstr_, idx);
        // 1-unit encoding for code points U+0000 to U+FFFF.
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(cp));
            continue;
        }

        // 2-unit surrogate pair encoding for code points U+10000 to U+10FFFF.
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
        out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }

    return out;
}

std::u32string String::toUtf32() const
{
    if (stdstr_.empty()) {
        return {};
    }

    std::u32string out;
    out.reserve(stdstr_.size());

    size_t idx = 0;
    while (idx < stdstr_.size()) {
        out.push_back(decode_utf8_one(stdstr_, idx));
    }

    return out;
}

String String::fromLocal8Bit(const char* data, size_type count)
{
    if (!data) {
        return {};
    }

    if (count == std::string::npos) {
        count = std::strlen(data);
    }

    if (count == 0) {
        return {};
    }

#if defined(_WIN32) || defined(_WIN64)
    const int utf16_size = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(count), nullptr, 0);
    if (utf16_size <= 0) {
        return {};
    }

    std::u16string utf16(static_cast<size_t>(utf16_size), u'\0');
    const int      converted = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(count), reinterpret_cast<wchar_t*>(utf16.data()), utf16_size);
    if (converted != utf16_size) {
        return {};
    }
    return fromUtf16(utf16.data(), utf16.size());
#else
    return String(impl_type(reinterpret_cast<const char8_t*>(data), count));
#endif
}

String String::fromUtf16(const char16_t* data, size_type count)
{
    if (!data || count == 0)
        return {};

    if (count == std::string::npos) {
        count = cstrlen(data);
    }

    std::u8string out;
    out.reserve(count);

    size_type i = 0;
    while (i < count) {
        char32_t cp = data[i++];

        // High: D800–DBFF 110110xxxxxxxxxx
        // Low : DC00–DFFF 110111xxxxxxxxxx

        // 2-unit surrogate pair (for code points above U+FFFF).
        // high surrogate (0xD800 to 0xDBFF) followed by low surrogate (0xDC00 to 0xDFFF).
        if (cp >= 0xD800 /*1101100000000000*/ && cp <= 0xDBFF /*1101101111111111*/) {
            if (i < count) {
                const char32_t low = data[i];
                if (low >= 0xDC00 /*1101110000000000*/ && low <= 0xDFFF /*1101111111111111*/) {
                    ++i;
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                }
                // invalid low surrogate, unpaired high surrogate (0xD800 to 0xDBFF must be followed by a low surrogate in the range 0xDC00 to 0xDFFF).
                else {
                    cp = k_replacement_char;
                }
            }
            // missing low surrogate (0xD800 to 0xDBFF must be followed by a low surrogate in the range 0xDC00 to 0xDFFF).
            // Unpaired high surrogates are invalid and replaced with the replacement character.
            else {
                cp = k_replacement_char;
            }
        }
        // missing high surrogate (0xDC00 to 0xDFFF must precede a low surrogate).
        // Unpaired low surrogates (U+DC00 to U+DFFF) are also invalid and replaced with the replacement character.
        else if (cp >= 0xDC00 /*1101110000000000*/ && cp <= 0xDFFF /*1101111111111111*/) {
            cp = k_replacement_char;
        }

        append_utf8(out, cp);
    }

    return String(std::move(out));
}

String String::fromUtf32(const char32_t* data, size_type count)
{
    if (!data || count == 0) {
        return {};
    }

    if (count == std::string::npos) {
        count = cstrlen(data);
    }

    std::u8string out;
    out.reserve(count);

    for (size_type i = 0; i < count; ++i) {
        append_utf8(out, data[i]);
    }

    return String(std::move(out));
}

String String::hex(std::span<const std::uint8_t> bytes)
{
    static constexpr char8_t digits[] = u8"0123456789abcdef";
    std::u8string            out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return String(std::move(out));
}

VN_CORE_NS_END
