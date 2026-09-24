// #pragma once
// #include "core_global.hpp"

// #include <cstddef>
// #include <string>
// #include <vector>

// #include "Char.hpp"

// VN_CORE_NS_BEGIN

// /** Optimized C-style string length function
//  *  Supports char, char16_t, and char32_t with platform-specific optimizations.
//  *  @tparam T Character type (char, char16_t, or char32_t)
//  *  @param data Null-terminated string pointer
//  *  @return The length of the string (number of characters before null terminator)
//  */
// template <typename T>
// size_t cstrlen(const T* data)
// {
//     const T* p = data;
//     while (*p != T(0)) ++p;
//     return p - data;
// }

// /** Specialization for char using strlen
//  *  Uses optimized std::strlen for maximum performance.
//  */
// template <>
// VN_CORE_API size_t cstrlen<char>(const char* data);

// /** Specialization for char16_t
//  *  Platform-optimized: uses wcslen on Windows, pointer traversal on Unix/Linux.
//  */
// template <>
// VN_CORE_API size_t cstrlen<char16_t>(const char16_t* data);

// /** Specialization for char32_t
//  *  Platform-optimized: uses wcslen on Unix/Linux, pointer traversal on Windows.
//  */
// template <>
// VN_CORE_API size_t cstrlen<char32_t>(const char32_t* data);

// class VN_CORE_API String final {

//   public:
//     using size_type      = size_t;
//     using iterator       = Char*;
//     using const_iterator = const Char*;

//   public:
//     /** Default constructor, creates an empty string */
//     String();

//     /** Constructor from null-terminated C-style string */
//     String(const Char* data);

//     /** Constructor from character array with specified length */
//     String(const Char* data, size_type count);

//     /** Copy constructor */
//     String(const String& other) noexcept;

//     /** Move constructor */
//     String(String&& other) noexcept;

//     /** Destructor */
//     virtual ~String();

//   public:
//     static constexpr size_type NPOS = size_type(-1);

//   public:
//     /** Get the number of characters in the string
//      *  @return The length of the string
//      */
//     size_type size() const noexcept
//     {
//         return len_;
//     }

//     /** Get the number of characters in the string (alias for size())
//      *  @return The length of the string
//      */
//     size_type length() const noexcept
//     {
//         return len_;
//     }

//     /** Get the allocated capacity of the string buffer
//      *  @return The capacity of the allocated buffer
//      */
//     size_type capacity() const noexcept
//     {
//         return capacity_;
//     }

//     /** Check if the string is empty
//      *  @return true if the string has no characters, false otherwise
//      */
//     bool empty() const noexcept
//     {
//         return len_ == 0;
//     }

//     /** Get a mutable pointer to the string data
//      *  @return Pointer to the internal character array
//      *  @warning The returned pointer becomes invalid if the string is modified through methods like
//      *           assign(), insert(), replace(), clear(), resize(), or any operation that changes capacity.
//      *           Do not use the pointer after any such modification.
//      *  @note The returned pointer is valid as long as the string is not modified
//      */
//     Char* data() noexcept
//     {
//         return data_;
//     }

//     /** Get a const pointer to the string data
//      *  @return Const pointer to the internal character array
//      *  @warning The returned pointer becomes invalid if the string is modified through methods like
//      *           assign(), insert(), replace(), clear(), resize(), or any operation that changes capacity.
//      *           Do not use the pointer after any such modification.
//      *  @note The returned pointer is valid as long as the string is not modified
//      */
//     const Char* data() const noexcept
//     {
//         return data_;
//     }

//     /** Clear the string, making it empty
//      *  @note The capacity is not changed
//      */
//     void clear() noexcept;

//     /** Resize the string to a new size
//      *  @param new_size The new size of the string
//      *  @param ch Character to fill new positions with (default: null character)
//      *  @note If new_size is larger than current size, appends ch characters
//      *        If new_size is smaller than current size, truncates the string
//      */
//     void resize(size_type new_size, Char ch = {});

//     /** Reserve capacity for the string
//      *  @param new_cap The new capacity to reserve
//      *  @note If new_cap is less than current capacity, this call has no effect
//      */
//     void reserve(size_type new_cap);

//     /** Shrink the capacity to match the current size
//      *  @note Releases unused memory allocated by the string
//      */
//     void shrinkToFit();

//     /** Swap contents with another string
//      *  @param s The string to swap with
//      *  @note This operation is guaranteed to not throw
//      */
//     void swap(String& s) noexcept;

//     /** Assign a character repeated count times
//      *  @param ch The character to assign
//      *  @param count The number of times to repeat the character
//      *  @return Reference to this string
//      *  @note Replaces the current string content
//      */
//     String& assign(Char ch, size_type count);

//     /** Assign from another string
//      *  @param str The source string to assign
//      *  @return Reference to this string
//      *  @note Performs a deep copy of the source string
//      */
//     String& assign(const String& str);

//     /** Assign a substring from another string
//      *  @param str The source string
//      *  @param pos The starting position in the source string
//      *  @param count The number of characters to copy
//      *  @return Reference to this string
//      *  @note Copies characters [pos, pos + count) from str
//      */
//     String& assign(const String& str, size_type pos, size_type count);

//     /** Assign using move semantics from another string
//      *  @param str The source string (will be moved from)
//      *  @return Reference to this string
//      *  @note Transfers ownership of the buffer from str to this string
//      */
//     String& assign(String&& str);

//     /** Assign from an iterator range
//      *  @param begin Iterator to the beginning of the range
//      *  @param end Iterator to the end of the range (exclusive)
//      *  @return Reference to this string
//      *  @note Copies all characters in the range [begin, end)
//      */
//     String& assign(const_iterator begin, const_iterator end);

//     /** Assign from a C-style character array
//      *  @param data Pointer to the character array
//      *  @param count The number of characters to copy (default: NPOS = until null terminator)
//      *  @return Reference to this string
//      *  @note If count is NPOS, all characters up to the null terminator are copied
//      */
//     String& assign(const Char* data, size_type count = NPOS);

//     /** Insert a single character at the specified position
//      *  @param pos The position to insert at
//      *  @param ch The character to insert
//      *  @return Reference to this string
//      *  @note Shifts all characters from pos onwards to the right
//      */
//     String& insert(size_type pos, Char ch);

//     /** Insert a C-style string at the specified position
//      *  @param pos The position to insert at
//      *  @param data Pointer to the character array
//      *  @param count The number of characters to insert (default: NPOS = until null terminator)
//      *  @return Reference to this string
//      *  @note If count is NPOS, all characters up to the null terminator are inserted
//      */
//     String& insert(size_type pos, const Char* data, size_type count = NPOS);

//     /** Insert a string at the specified position
//      *  @param pos The position to insert at
//      *  @param str The source string to insert
//      *  @return Reference to this string
//      *  @note The entire str is inserted at position pos
//      */
//     String& insert(size_type pos, const String& str);

//     /** Insert a substring at the specified position
//      *  @param pos The position to insert at
//      *  @param str The source string
//      *  @param str_pos The starting position in the source string
//      *  @param count The number of characters to insert (default: NPOS)
//      *  @return Reference to this string
//      *  @note Inserts characters [str_pos, str_pos + count) from str at position pos
//      */
//     String& insert(size_type pos, const String& str, size_type str_pos, size_type count = NPOS);

//     /** Replace a range with a single character
//      *  @param pos The starting position of the range to replace
//      *  @param count The number of characters to replace
//      *  @param ch The character to use as replacement
//      *  @return Reference to this string
//      *  @note Replaces characters [pos, pos + count) with count copies of ch
//      */
//     String& replace(size_type pos, size_type count, Char ch);

//     /** Replace a range with a C-style string
//      *  @param pos The starting position of the range to replace
//      *  @param count The number of characters to replace
//      *  @param data Pointer to the character array
//      *  @param data_count The number of characters to use (default: NPOS = until null terminator)
//      *  @return Reference to this string
//      *  @note Replaces characters [pos, pos + count) with data[0..data_count)
//      */
//     String& replace(size_type pos, size_type count, const Char* data, size_type data_count = NPOS);

//     /** Replace a range with another string
//      *  @param pos The starting position of the range to replace
//      *  @param count The number of characters to replace
//      *  @param str The source string
//      *  @return Reference to this string
//      *  @note Replaces characters [pos, pos + count) with the entire str
//      */
//     String& replace(size_type pos, size_type count, const String& str);

//     /** Replace a range with a substring
//      *  @param pos The starting position of the range to replace
//      *  @param count The number of characters to replace
//      *  @param str The source string
//      *  @param str_pos The starting position in the source string
//      *  @param str_count The number of characters to use (default: NPOS)
//      *  @return Reference to this string
//      *  @note Replaces characters [pos, pos + count) with str[str_pos, str_pos + str_count)
//      */
//     String& replace(size_type pos, size_type count, const String& str, size_type str_pos, size_type str_count = NPOS);

//     /** Get mutable reference to character at specified index
//      *  @param idx The index of the character
//      *  @return Reference to the character
//      *  @warning The returned reference becomes invalid if the string is modified through methods like
//      *           assign(), insert(), replace(), clear(), resize(), or any operation that changes capacity.
//      *           Do not use the reference after any such modification.
//      *  @note Does not perform bounds checking
//      */
//     Char& at(size_type idx);

//     /** Get const reference to character at specified index
//      *  @param idx The index of the character
//      *  @return Const reference to the character
//      *  @warning The returned reference becomes invalid if the string is modified through methods like
//      *           assign(), insert(), replace(), clear(), resize(), or any operation that changes capacity.
//      *           Do not use the reference after any such modification.
//      *  @note Does not perform bounds checking
//      */
//     const Char& at(size_type idx) const;

//     /** Extract a substring
//      *  @param start The starting position
//      *  @param count The number of characters to extract (default: NPOS = to the end)
//      *  @return A new string containing the substring
//      *  @note Returns characters [start, start + count)
//      *        If start >= size(), returns an empty string
//      */
//     String substr(size_type start, size_type count = NPOS) const;

//     /** Find the first occurrence of a character
//      *  @param c The character to search for
//      *  @param pos The starting position (default: 0)
//      *  @return The index of the first occurrence, or NPOS if not found
//      *  @note Searches in the range [pos, size())
//      */
//     size_type find(Char c, size_type pos = 0) const;

//     /** Find the first occurrence of a substring
//      *  @param str The substring to search for
//      *  @param pos The starting position (default: 0)
//      *  @return The index of the first occurrence, or NPOS if not found
//      *  @note Searches in the range [pos, size())
//      *        If str is empty, returns pos
//      */
//     size_type find(const String& str, size_type pos = 0) const;

//     /** Find the first occurrence of a C-style string
//      *  @param data The C-style string to search for
//      *  @param pos The starting position (default: 0)
//      *  @return The index of the first occurrence, or NPOS if not found
//      *  @note Searches in the range [pos, size())
//      */
//     size_type find(const Char* data, size_type pos = 0) const;

//     /** Find the last occurrence of a character
//      *  @param c The character to search for
//      *  @param pos The starting position for backward search (default: NPOS = from the end)
//      *  @return The index of the last occurrence, or NPOS if not found
//      *  @note Searches backward in the range [0, pos]
//      */
//     size_type rfind(Char c, size_type pos = NPOS) const;

//     /** Find the last occurrence of a substring
//      *  @param str The substring to search for
//      *  @param pos The starting position for backward search (default: NPOS = from the end)
//      *  @return The index of the last occurrence, or NPOS if not found
//      *  @note Searches backward in the range [0, pos]
//      *        If str is empty, returns min(pos, size())
//      */
//     size_type rfind(const String& str, size_type pos = NPOS) const;

//     /** Find the last occurrence of a C-style string
//      *  @param data The C-style string to search for
//      *  @param pos The starting position for backward search (default: NPOS = from the end)
//      *  @return The index of the last occurrence, or NPOS if not found
//      *  @note Searches backward in the range [0, pos]
//      */
//     size_type rfind(const Char* data, size_type pos = NPOS) const;

//     /** Convert string to lowercase
//      *  @param ascii_only If true (default), only ASCII characters A-Z are converted
//      *  @return A new string with lowercase characters
//      *  @note This string is not modified
//      */
//     String toLower(bool ascii_only = true) const;

//     /** Convert string to uppercase
//      *  @param ascii_only If true (default), only ASCII characters a-z are converted
//      *  @return A new string with uppercase characters
//      *  @note This string is not modified
//      */
//     String toUpper(bool ascii_only = true) const;

//     /** Remove whitespace from the beginning of the string (modifies in-place)
//      *  @return Reference to this string
//      *  @note Removes all leading whitespace characters (space, tab, newline, carriage return, form feed, vertical tab)
//      */
//     String& trimStart();

//     /** Get a copy with whitespace removed from the beginning
//      *  @return A new string with leading whitespace removed
//      *  @note This string is not modified
//      */
//     String trimmedStart() const;

//     /** Get a copy with whitespace removed from the end
//      *  @return A new string with trailing whitespace removed
//      *  @note This string is not modified
//      */
//     String trimmedEnd() const;

//     /** Remove whitespace from the end of the string (modifies in-place)
//      *  @return Reference to this string
//      *  @note Removes all trailing whitespace characters (space, tab, newline, carriage return, form feed, vertical tab)
//      */
//     String& trimEnd();

//     /** Remove whitespace from both ends of the string (modifies in-place)
//      *  @return Reference to this string
//      *  @note Removes all leading and trailing whitespace characters
//      */
//     String& trim();

//     /** Get a copy with whitespace removed from both ends
//      *  @return A new string with all leading and trailing whitespace removed
//      *  @note This string is not modified
//      */
//     String trimmed() const;

//     /** Check if the string equals another string
//      *  @param other The string to compare with
//      *  @param ignore_case If true, performs case-insensitive comparison (default: false)
//      *  @return true if the strings are equal, false otherwise
//      */
//     bool equals(const String& other, bool ignore_case = false) const;

//     /** Check if the string starts with a single character
//      *  @param c The character to search for
//      *  @param ignore_case If true, performs case-insensitive comparison (default: false)
//      *  @return true if the string starts with the character, false otherwise
//      */
//     bool startsWith(Char c, bool ignore_case = false) const;

//     /** Check if the string starts with another string
//      *  @param str The prefix to check for
//      *  @param ignore_case If true, performs case-insensitive comparison (default: false)
//      *  @return true if the string starts with str, false otherwise
//      */
//     bool startsWith(const String& str, bool ignore_case = false) const;

//     /** Check if the string ends with a single character
//      *  @param c The character to search for
//      *  @param ignore_case If true, performs case-insensitive comparison (default: false)
//      *  @return true if the string ends with the character, false otherwise
//      */
//     bool endsWith(Char c, bool ignore_case = false) const;

//     /** Check if the string ends with another string
//      *  @param str The suffix to check for
//      *  @param ignore_case If true, performs case-insensitive comparison (default: false)
//      *  @return true if the string ends with str, false otherwise
//      */
//     bool endsWith(const String& str, bool ignore_case = false) const;

//     /** Convert string to integer
//      *  @return The integer value
//      *  @param ok Optional pointer to bool, set to true if conversion succeeded, false otherwise
//      *  @note If the string cannot be converted to a valid integer, returns 0 and sets ok to false.
//      *        The conversion stops at the first non-digit character (after optional leading whitespace).
//      *  @example String("123").toInt() returns 123
//      *           String("45ab").toInt() returns 45 (stops at 'a')
//      *           String("abc").toInt(&ok) returns 0, ok = false
//      */
//     int toInt(bool* ok = nullptr) const;

//     /** Convert string to double
//      *  @return The double value
//      *  @param ok Optional pointer to bool, set to true if conversion succeeded, false otherwise
//      *  @note If the string cannot be converted to a valid double, returns 0.0 and sets ok to false.
//      *        Supports formats: "123", "123.45", "1.23e-4", "-123.45", etc.
//      *  @example String("123.45").toDouble() returns 123.45
//      *           String("1.23e-4").toDouble() returns 0.000123
//      *           String("abc").toDouble(&ok) returns 0.0, ok = false
//      */
//     double toDouble(bool* ok = nullptr) const;

//     /** Convert string to UTF-8 encoded bytes
//      *  @return std::u8string containing UTF-8 encoded data
//      *  @note The returned string contains the UTF-8 byte sequence representation of this string
//      */
//     std::u8string toUtf8() const;

//     /** Convert string to UTF-16 encoded code units
//      *  @return std::u16string containing UTF-16 encoded data
//      *  @note The returned string contains properly encoded UTF-16 surrogates for characters beyond the BMP
//      */
//     std::u16string toUtf16() const;

//     /** Convert string to local 8-bit encoded bytes
//      *  @return std::string containing local 8-bit encoded data
//      *  @note On Windows, uses Win32 API to convert to the system's ANSI code page (CP_ACP)
//      *        On Linux/other systems, not implemented yet and will throw an exception
//      *  @throws Exception if the encoding is not supported on the platform
//      */
//     std::string toLocal8Bit() const;

//     /** Format a string using printf-style formatting
//      *  @param fmt Format string (printf-style)
//      *  @param ... Arguments for formatting
//      *  @return Formatted string
//      *  @note Uses vsnprintf internally for safe formatting
//      */
//     static String format(const char* fmt, ...);

//     /** Split string by delimiter
//      *  @param delimiter The character to split on
//      *  @param keepEmpty Whether to keep empty substrings (default: true)
//      *  @return Vector of substrings
//      *  @note If keepEmpty is false, consecutive delimiters will not produce empty strings
//      */
//     std::vector<String> split(Char delimiter, bool keepEmpty = true) const;

//     /** Split string by delimiter string
//      *  @param delimiter The string to split on
//      *  @param keepEmpty Whether to keep empty substrings (default: true)
//      *  @return Vector of substrings
//      *  @note If keepEmpty is false, consecutive delimiters will not produce empty strings
//      */
//     std::vector<String> split(const String& delimiter, bool keepEmpty = true) const;

//     /** Get iterator to the beginning of the string
//      *  @return Iterator pointing to the first character
//      *  @warning Any modification to the string through methods like assign(), insert(), replace(),
//      *           clear(), resize(), or any operation that changes capacity invalidates all iterators.
//      *           Do not use iterators after any such modification.
//      */
//     iterator begin() const
//     {
//         return data_;
//     }

//     /** Get iterator to the end of the string
//      *  @return Iterator pointing one past the last character
//      *  @warning Any modification to the string through methods like assign(), insert(), replace(),
//      *           clear(), resize(), or any operation that changes capacity invalidates all iterators.
//      *           Do not use iterators after any such modification.
//      */
//     iterator end() const
//     {
//         return data_ + len_;
//     }

//     /** Get const iterator to the beginning of the string
//      *  @return Const iterator pointing to the first character
//      *  @warning Any modification to the string through methods like assign(), insert(), replace(),
//      *           clear(), resize(), or any operation that changes capacity invalidates all iterators.
//      *           Do not use iterators after any such modification.
//      */
//     const_iterator cbegin() const
//     {
//         return data_;
//     }

//     /** Get const iterator to the end of the string
//      *  @return Const iterator pointing one past the last character
//      *  @warning Any modification to the string through methods like assign(), insert(), replace(),
//      *           clear(), resize(), or any operation that changes capacity invalidates all iterators.
//      *           Do not use iterators after any such modification.
//      */
//     const_iterator cend() const
//     {
//         return data_ + len_;
//     }

//   public:
//     /** Create a string from UTF-8 encoded data
//      *  @param data Pointer to UTF-8 encoded character array
//      *  @param count The number of bytes to read (default: NPOS = until null terminator)
//      *  @return A new String with the decoded UTF-8 data
//      *  @note Invalid UTF-8 sequences may be skipped or handled according to implementation
//      *        If count is NPOS, all bytes up to the null terminator are processed
//      */
//     static String fromUtf8(const char* data, size_type count = NPOS);

//     /** Create a string from UTF-8 encoded data
//      *  @param data Pointer to UTF-8 encoded character array
//      *  @param count The number of bytes to read (default: NPOS = until null terminator)
//      *  @return A new String with the decoded UTF-8 data
//      *  @note Invalid UTF-8 sequences may be skipped or handled according to implementation
//      *        If count is NPOS, all bytes up to the null terminator are processed
//      */
//     static String fromUtf8(const char8_t* data, size_type count = NPOS);

//     /** Create a string from UTF-16 encoded data
//      *  @param data Pointer to UTF-16 encoded character array
//      *  @param count The number of code units to read (default: NPOS = until null terminator)
//      *  @return A new String with the decoded UTF-16 data
//      *  @note Properly handles UTF-16 surrogate pairs for characters beyond the BMP
//      *        If count is NPOS, processing continues until a null terminator is found
//      */
//     static String fromUtf16(const char16_t* data, size_type count = NPOS);

//     /** Create a string from local 8-bit encoded data
//      *  @param data Pointer to local 8-bit encoded character array
//      *  @return A new String with the decoded data
//      *  @note The encoding depends on the platform's locale settings
//      *  @throws e.g., invalid encoding or unsupported locale
//      */
//     static String fromLocal8Bit(const char* data, size_type count = NPOS);

//   public:
//     /** Assignment operator
//      *  @param right The string to assign from
//      *  @return Reference to this string
//      *  @note Performs a deep copy of the source string
//      */
//     String& operator=(const String& right);

//     /** Get mutable reference to character at specified index
//      *  @param idx The index of the character
//      *  @return Reference to the character
//      *  @note Does not perform bounds checking
//      */
//     Char& operator[](size_type idx)
//     {
//         return *(data_ + idx);
//     }

//     /** Get const reference to character at specified index
//      *  @param idx The index of the character
//      *  @return Const reference to the character
//      *  @note Does not perform bounds checking
//      */
//     const Char& operator[](size_type idx) const
//     {
//         return *(data_ + idx);
//     }

//     /** Equality comparison operator
//      *  @param right The string to compare with
//      *  @return true if the strings are equal, false otherwise
//      */
//     bool operator==(const String& right) const;

//     /** Inequality comparison operator
//      *  @param right The string to compare with
//      *  @return true if the strings are not equal, false otherwise
//      */
//     bool operator!=(const String& right) const;

//     /** Less than comparison operator
//      *  @param right The string to compare with
//      *  @return true if this string is lexicographically less than right
//      */
//     bool operator<(const String& right) const;

//     /** Greater than comparison operator
//      *  @param right The string to compare with
//      *  @return true if this string is lexicographically greater than right
//      */
//     bool operator>(const String& right) const;

//     /** String concatenation operator
//      *  @param right The string to concatenate
//      *  @return A new string containing this string followed by right
//      *  @note This string is not modified
//      */
//     String operator+(const String& right) const;

//     /** Append operator
//      *  @param right The string to append
//      *  @return Reference to this string
//      *  @note Appends right to the end of this string
//      */
//     String& operator+=(const String& right);

//   private:
//     Char*     data_{};
//     size_type len_{};
//     size_type capacity_{};
// };

// VN_CORE_NS_END
