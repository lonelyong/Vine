#pragma once

#include "core_global.hpp"

#include <concepts>

VN_CORE_NS_BEGIN

template <typename T>
concept CharType = std::same_as<T, char> || std::same_as<T, signed char> || std::same_as<T, unsigned char> || std::same_as<T, wchar_t> ||
                   std::same_as<T, char8_t> || std::same_as<T, char16_t> || std::same_as<T, char32_t>;

/** Check if a character is whitespace (ASCII)
 *  ASCII whitespace characters:
 *  - \\t - horizontal tab     (0x09, 9)
 *  - \\n - line feed          (0x0A, 10)
 *  - \\v - vertical tab       (0x0B, 11)
 *  - \\f - form feed          (0x0C, 12)
 *  - \\r - carriage return    (0x0D, 13)
 *  - ' ' - space             (0x20, 32)
 *  @param c The character to check
 *  @return true if c is an ASCII whitespace character, false otherwise
 */

template <CharType Char>
constexpr bool isspace(Char c)
{
    return c == static_cast<Char>(' ') || (c >= static_cast<Char>('\t') && c <= static_cast<Char>('\r'));
}

/** Compare two characters in a case-insensitive manner
 *  @param l The left character to compare
 *  @param r The right character to compare
 *  @return true if l and r are equal (ignoring case for ASCII letters), false otherwise
 *  @note Only ASCII uppercase letters (A-Z) are converted to lowercase (add 'a' - 'A' to code point)
 *        Non-ASCII and already lowercase characters are compared as-is
 *  @example iequals('A', 'a') returns true
 *           iequals('A', 'B') returns false
 *           iequals('á', 'Á') returns false (non-ASCII, not converted)
 */
template <CharType Char>
constexpr bool iequals(Char l, Char r)
{
    constexpr Char ascii_case_delta = static_cast<Char>('a' - 'A');
    // Case-insensitive character comparison
    // Only ASCII uppercase letters (A-Z) are converted to lowercase
    if (l >= static_cast<Char>('A') && l <= static_cast<Char>('Z'))
        l += ascii_case_delta;
    if (r >= static_cast<Char>('A') && r <= static_cast<Char>('Z'))
        r += ascii_case_delta;
    return l == r;
}

/** Convert an ASCII uppercase letter to lowercase
 *  @param c The character to convert
 *  @return If c is an ASCII uppercase letter (A-Z), returns the corresponding lowercase letter (a-z).
 *          Otherwise, returns c unchanged.
 *  @note Only ASCII uppercase letters (A-Z) are converted. Other characters including non-ASCII
 *        characters and already lowercase letters are returned as-is.
 *  @example tolower('A') returns 'a'
 *           tolower('a') returns 'a'
 *           tolower('1') returns '1'
 */
template <CharType Char>
constexpr Char tolower(Char c)
{
    constexpr Char ascii_case_delta = static_cast<Char>('a' - 'A');
    if (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z'))
        return c + ascii_case_delta;
    return c;
}

/** Convert an ASCII lowercase letter to uppercase
 *  @param c The character to convert
 *  @return If c is an ASCII lowercase letter (a-z), returns the corresponding uppercase letter (A-Z).
 *          Otherwise, returns c unchanged.
 *  @note Only ASCII lowercase letters (a-z) are converted. Other characters including non-ASCII
 *        characters and already uppercase letters are returned as-is.
 *  @example toupper('a') returns 'A'
 *           toupper('A') returns 'A'
 *           toupper('1') returns '1'
 */
template <CharType Char>
constexpr Char toupper(Char c)
{
    constexpr Char ascii_case_delta = static_cast<Char>('a' - 'A');
    if (c >= static_cast<Char>('a') && c <= static_cast<Char>('z'))
        return c - ascii_case_delta;
    return c;
}

/** Check if a character is an ASCII letter
 *  @param c The character to check
 *  @return true if c is an ASCII letter (A-Z or a-z), false otherwise
 *  @note Only ASCII letters are recognized. Accented characters and other non-ASCII
 *        characters are considered non-alphabetic.
 *  @example isalpha('A') returns true
 *           isalpha('a') returns true
 *           isalpha('1') returns false
 *           isalpha('á') returns false
 */
template <CharType Char>
constexpr bool isalpha(Char c)
{
    return (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z')) || (c >= static_cast<Char>('a') && c <= static_cast<Char>('z'));
}

/** Check if a character is an ASCII digit
 *  @param c The character to check
 *  @return true if c is an ASCII digit (0-9), false otherwise
 *  @example isdigit('5') returns true
 *           isdigit('a') returns false
 */
template <CharType Char>
constexpr bool isdigit(Char c)
{
    return c >= static_cast<Char>('0') && c <= static_cast<Char>('9');
}

/** Check if a character is an ASCII letter or digit
 *  @param c The character to check
 *  @return true if c is an ASCII letter (A-Z, a-z) or digit (0-9), false otherwise
 *  @example isalnum('A') returns true
 *           isalnum('5') returns true
 *           isalnum('!') returns false
 */
template <CharType Char>
constexpr bool isalnum(Char c)
{
    return isalpha(c) || isdigit(c);
}

/** Check if a character is an ASCII uppercase letter
 *  @param c The character to check
 *  @return true if c is an ASCII uppercase letter (A-Z), false otherwise
 *  @example isupper('A') returns true
 *           isupper('a') returns false
 */
template <CharType Char>
constexpr bool isupper(Char c)
{
    return c >= static_cast<Char>('A') && c <= static_cast<Char>('Z');
}

/** Check if a character is an ASCII lowercase letter
 *  @param c The character to check
 *  @return true if c is an ASCII lowercase letter (a-z), false otherwise
 *  @example islower('a') returns true
 *           islower('A') returns false
 */
template <CharType Char>
constexpr bool islower(Char c)
{
    return c >= static_cast<Char>('a') && c <= static_cast<Char>('z');
}

VN_CORE_NS_END
