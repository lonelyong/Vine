// #include <vine/String.hpp>

// #include <algorithm>
// #include <climits>
// #include <cmath>
// #include <cstdarg>
// #include <cstring>
// #include <ctype.h>
// #include <locale>
// #include <vector>

// #ifdef _WIN32
// #    include <windows.h>
// #endif

// #include <vine/Exception.hpp>

// VN_CORE_NS_BEGIN

// namespace
// {

// Char g_empty_buffer[] = { 0 };

// inline void delete_data(Char* data)
// {
//     if (data && data != g_empty_buffer) {
//         delete[] data;
//     }
// }

// } // namespace

// // Specialization for char to use strlen
// template <>
// size_t cstrlen<char>(const char* data)
// {
//     return std::strlen(data);
// }

// // Specialization for char16_t
// // On Windows wchar_t is 16-bit, on Linux/Unix it's typically 32-bit
// template <>
// size_t cstrlen<char16_t>(const char16_t* data)
// {
// #if defined(_WIN32) || defined(_WIN64)
//     // Windows: wchar_t is 16-bit, safe to cast
//     return std::wcslen(reinterpret_cast<const wchar_t*>(data));
// #else
//     // Linux/Unix: wchar_t is 32-bit, use generic version
//     const char16_t* p = data;
//     while (*p != 0) ++p;
//     return p - data;
// #endif
// }

// // Specialization for char32_t
// // On Linux/Unix wchar_t is 32-bit, matching char32_t
// template <>
// size_t cstrlen<char32_t>(const char32_t* data)
// {
// #if !defined(_WIN32) && !defined(_WIN64)
//     // Linux/Unix: wchar_t is 32-bit, safe to cast
//     return std::wcslen(reinterpret_cast<const wchar_t*>(data));
// #else
//     // Windows: wchar_t is 16-bit, use generic version
//     const char32_t* p = data;
//     while (*p != 0) ++p;
//     return p - data;
// #endif
// }

// String::String()
//   : data_(g_empty_buffer)
// {}

// String::String(const Char* data)
// {
//     assign(data);
// }

// String::String(const Char* data, size_type count)
// {
//     assign(data, count);
// }

// String::String(const String& other) noexcept
// {
//     if (other.empty()) {
//         clear();
//     }
//     else {
//         assign(other.data_, other.len_);
//     }
// }

// String::String(String&& other) noexcept
// {
//     delete_data(data_);

//     data_     = other.data_;
//     len_      = other.len_;
//     capacity_ = other.capacity_;

//     other.data_     = new Char[]{ 0 };
//     other.len_      = 0;
//     other.capacity_ = 0;
// }

// String::~String()
// {
//     delete_data(data_);
// }

// void String::resize(size_type new_size, Char ch)
// {
//     if (new_size == len_)
//         return;

//     if (new_size < len_) {
//         len_        = new_size;
//         data_[len_] = 0;
//         return;
//     }

//     reserve(new_size);

//     std::fill(data_ + len_, data_ + new_size, ch);

//     len_        = new_size;
//     data_[len_] = 0;
// }

// void String::reserve(size_type new_cap)
// {
//     if (new_cap <= capacity_)
//         return;

//     auto buf = new Char[new_cap + 1];

//     if (len_ > 0)
//         std::memcpy(buf, data_, len_ * sizeof(Char));

//     buf[len_] = 0;

//     delete_data(data_);

//     data_     = buf;
//     capacity_ = new_cap;
// }

// void String::swap(String& s) noexcept
// {
//     std::swap(data_, s.data_);
//     std::swap(len_, s.len_);
//     std::swap(capacity_, s.capacity_);
// }

// void String::shrinkToFit()
// {
//     if (capacity_ > len_ && len_ > 0) {
//         auto buf = new Char[len_ + 1];
//         memcpy(buf, data_, len_ * sizeof(Char));
//         buf[len_] = 0;
//         delete_data(data_);
//         data_     = buf;
//         capacity_ = len_;
//     }
// }

// void String::clear() noexcept
// {
//     data_[0] = 0;
//     len_     = 0;
// }

// String& String::assign(const Char* data, size_type len)
// {
//     if (!data || len == 0) {
//         clear();
//         return *this;
//     }

//     if (len == NPOS) {
//         len = cstrlen(data);
//     }

//     // 自重合，且长度小于当前长度
//     if (data_ == data) {
//         if (len_ == len) {
//             return *this;
//         }

//         if (len < len_) {
//             data_[len] = 0;
//             len_       = len;
//             return *this;
//         }
//     }

//     // 缓冲区小于长度，需要重新分配内存，自重合时没有问题
//     if (capacity_ < len) {
//         auto buf = new Char[len + 1];
//         buf[len] = 0;

//         memcpy(buf, data, len * sizeof(Char));

//         delete_data(data_);
//         data_     = buf;
//         capacity_ = len;
//     }
//     else {
//         // overlap
//         if (data < data_ + len && data + len > data_) {
//             memmove(data_, data, len * sizeof(Char));
//         }
//         else {
//             memcpy(data_, data, len * sizeof(Char));
//         }
//         data_[len] = 0;
//     }

//     len_ = len;

//     return *this;
// }

// String& String::assign(Char ch, size_type count)
// {
//     if (count == 0) {
//         clear();
//         return *this;
//     }

//     if (capacity_ < count) {
//         delete_data(data_);
//         data_     = new Char[count + 1];
//         capacity_ = count;
//     }

//     std::fill(data_, data_ + count, ch);
//     data_[count] = 0;
//     len_         = count;

//     return *this;
// }

// String& String::assign(const String& str)
// {
//     return assign(str.data_, str.len_);
// }

// String& String::assign(const String& str, size_type pos, size_type count)
// {
//     if (pos >= str.len_) {
//         clear();
//         return *this;
//     }

//     size_type actual_count = count;
//     if (pos + count > str.len_) {
//         actual_count = str.len_ - pos;
//     }

//     return assign(str.data_ + pos, actual_count);
// }

// String& String::assign(String&& str)
// {
//     delete_data(data_);

//     data_     = str.data_;
//     len_      = str.len_;
//     capacity_ = str.capacity_;

//     str.data_     = new Char[]{ 0 };
//     str.len_      = 0;
//     str.capacity_ = 0;

//     return *this;
// }

// String& String::assign(const_iterator begin, const_iterator end)
// {
//     if (!begin || !end || begin >= end) {
//         clear();
//         return *this;
//     }

//     size_type count = end - begin;
//     return assign(begin, count);
// }

// String& String::insert(size_type pos, Char ch)
// {
//     return insert(pos, &ch, 1);
// }

// String& String::insert(size_type pos, const Char* data, size_type count)
// {
//     if (!data || count == 0)
//         return *this;

//     if (count == NPOS)
//         count = cstrlen(data);

//     if (pos > len_)
//         pos = len_;

//     size_type new_len = len_ + count;

//     if (capacity_ < new_len) {
//         auto buf = new Char[new_len + 1];
//         if (len_ > 0)
//             memcpy(buf, data_, len_ * sizeof(Char));
//         buf[new_len] = 0;

//         delete_data(data_);
//         data_     = buf;
//         capacity_ = new_len;
//     }

//     // Move existing data to make room
//     if (pos < len_) {
//         memmove(data_ + pos + count, data_ + pos, (len_ - pos) * sizeof(Char));
//     }

//     // Copy new data
//     memcpy(data_ + pos, data, count * sizeof(Char));
//     data_[new_len] = 0;
//     len_           = new_len;

//     return *this;
// }

// String& String::insert(size_type pos, const String& str)
// {
//     return insert(pos, str.data_, str.len_);
// }

// String& String::insert(size_type pos, const String& str, size_type str_pos, size_type count)
// {
//     if (str_pos >= str.len_)
//         return *this;

//     if (count == NPOS)
//         count = str.len_ - str_pos;

//     size_type actual_count = std::min(count, str.len_ - str_pos);
//     return insert(pos, str.data_ + str_pos, actual_count);
// }

// String& String::replace(size_type pos, size_type count, Char ch)
// {
//     if (pos > len_)
//         return *this;

//     size_type actual_count = std::min(count, len_ - pos);

//     if (actual_count == 0)
//         return insert(pos, ch);

//     if (actual_count == 1) {
//         data_[pos] = ch;
//         return *this;
//     }

//     // Remove the old characters
//     if (pos + actual_count < len_) {
//         memmove(data_ + pos + 1, data_ + pos + actual_count, (len_ - pos - actual_count) * sizeof(Char));
//     }

//     data_[pos]  = ch;
//     len_        = len_ - actual_count + 1;
//     data_[len_] = 0;

//     return *this;
// }

// String& String::replace(size_type pos, size_type count, const Char* data, size_type data_count)
// {
//     if (!data || pos > len_)
//         return *this;

//     if (data_count == NPOS)
//         data_count = cstrlen(data);

//     size_type actual_count = std::min(count, len_ - pos);

//     if (data_count == actual_count) {
//         // Same size, just copy
//         memcpy(data_ + pos, data, data_count * sizeof(Char));
//         return *this;
//     }

//     size_type new_len = len_ - actual_count + data_count;

//     if (capacity_ < new_len) {
//         auto buf = new Char[new_len + 1];

//         // Copy before replaced part
//         if (pos > 0)
//             memcpy(buf, data_, pos * sizeof(Char));

//         // Copy new data
//         memcpy(buf + pos, data, data_count * sizeof(Char));

//         // Copy after replaced part
//         if (pos + actual_count < len_) {
//             memcpy(buf + pos + data_count, data_ + pos + actual_count, (len_ - pos - actual_count) * sizeof(Char));
//         }

//         buf[new_len] = 0;
//         delete_data(data_);

//         data_     = buf;
//         capacity_ = new_len;
//     }
//     else {
//         // Buffer is large enough
//         if (data_count != actual_count) {
//             // Move existing data
//             memmove(data_ + pos + data_count, data_ + pos + actual_count, (len_ - pos - actual_count) * sizeof(Char));
//         }

//         // Copy new data
//         memcpy(data_ + pos, data, data_count * sizeof(Char));
//     }

//     len_        = new_len;
//     data_[len_] = 0;

//     return *this;
// }

// String& String::replace(size_type pos, size_type count, const String& str)
// {
//     return replace(pos, count, str.data_, str.len_);
// }

// String& String::replace(size_type pos, size_type count, const String& str, size_type str_pos, size_type str_count)
// {
//     if (str_pos >= str.len_)
//         return *this;

//     if (str_count == NPOS)
//         str_count = str.len_ - str_pos;

//     size_type actual_count = std::min(str_count, str.len_ - str_pos);
//     return replace(pos, count, str.data_ + str_pos, actual_count);
// }

// Char& String::at(size_type idx)
// {
//     if (idx >= len_)
//         throw Exception(Exception::INDEX_OUT_OF_RANGE);

//     return *(data_ + idx);
// }

// const Char& String::at(size_type idx) const
// {
//     if (idx >= len_)
//         throw Exception(Exception::INDEX_OUT_OF_RANGE);

//     return *(data_ + idx);
// }

// String String::substr(size_type pos, size_type count) const
// {
//     if (count == 0)
//         return {};

//     if (pos > len_)
//         throw Exception(Exception::INDEX_OUT_OF_RANGE);

//     count = std::min(count, len_ - pos);

//     return String(data_ + pos, count);
// }

// String::size_type String::find(Char c, size_type pos) const
// {
//     for (size_type i = pos; i < len_; ++i) {
//         if (data_[i] == c)
//             return i;
//     }
//     return NPOS;
// }

// String::size_type String::find(const String& str, size_type pos) const
// {
//     if (pos > len_)
//         return NPOS;

//     if (str.len_ == 0)
//         return pos <= len_ ? pos : NPOS;

//     if (str.len_ > len_ - pos)
//         return NPOS;

//     auto begin = data_ + pos;
//     auto end   = data_ + len_;
//     auto it    = std::search(begin, end, str.data_, str.data_ + str.len_);
//     if (it == end)
//         return NPOS;
//     return static_cast<size_type>(it - data_);
// }

// String::size_type String::find(const Char* data, size_type pos) const
// {
//     if (!data)
//         return NPOS;

//     size_type data_len = cstrlen(data);
//     if (pos > len_)
//         return NPOS;
//     if (data_len == 0)
//         return pos <= len_ ? pos : NPOS;
//     if (data_len > len_ - pos)
//         return NPOS;

//     auto begin = data_ + pos;
//     auto end   = data_ + len_;
//     auto it    = std::search(begin, end, data, data + data_len);
//     if (it == end)
//         return NPOS;
//     return static_cast<size_type>(it - data_);
// }

// String::size_type String::rfind(Char c, size_type pos) const
// {
//     if (len_ == 0)
//         return NPOS;

//     if (pos == NPOS || pos > len_)
//         pos = len_;

//     for (size_type i = pos; i > 0; --i) {
//         if (data_[i - 1] == c)
//             return i - 1;
//     }

//     return NPOS;
// }

// String::size_type String::rfind(const String& str, size_type pos) const
// {
//     if (str.len_ == 0) {
//         if (pos == NPOS || pos > len_)
//             return len_;
//         return pos;
//     }

//     if (str.len_ > len_)
//         return NPOS;

//     size_type search_end = (pos == NPOS || pos > len_) ? len_ : pos + 1;
//     if (str.len_ > search_end)
//         return NPOS;

//     auto begin = data_;
//     auto end   = data_ + search_end;
//     auto it    = std::find_end(begin, end, str.data_, str.data_ + str.len_);
//     if (it == end)
//         return NPOS;
//     return static_cast<size_type>(it - data_);
// }

// String::size_type String::rfind(const Char* data, size_type pos) const
// {
//     if (!data)
//         return NPOS;

//     size_type data_len = cstrlen(data);
//     if (data_len == 0) {
//         if (pos == NPOS || pos > len_)
//             return len_;
//         return pos;
//     }
//     if (data_len > len_)
//         return NPOS;

//     size_type search_end = (pos == NPOS || pos > len_) ? len_ : pos + 1;
//     if (data_len > search_end)
//         return NPOS;

//     auto begin = data_;
//     auto end   = data_ + search_end;
//     auto it    = std::find_end(begin, end, data, data + data_len);
//     if (it == end)
//         return NPOS;
//     return static_cast<size_type>(it - data_);
// }

// String String::toLower(bool ascii_only) const
// {
//     if (len_ == 0)
//         return *this;

//     auto s = *this;

//     if (ascii_only) {
//         for (size_type i = 0; i < s.len_; i++) {
//             auto& c = s.at(i);

//             if (c >= U'A' && c <= U'Z')
//                 c += 32;
//         }
//     }
//     else {
//         for (size_type i = 0; i < s.len_; i++) {
//             auto& c = s.at(i);

//             if (c >= U'A' && c <= U'Z')
//                 c += 32;
//         }
//         // auto& f = std::use_facet<std::ctype<char32_t>>(std::locale(""));
//         // f.tolower(s.data_, s.data_ + s.len_);
//     }
//     return s;
// }

// String String::toUpper(bool ascii_only) const
// {
//     if (len_ == 0)
//         return *this;

//     auto s = *this;

//     if (ascii_only) {
//         for (size_type i = 0; i < s.len_; i++) {
//             auto& c = s.at(i);

//             if (c >= U'a' && c <= U'z')
//                 c -= 32;
//         }
//     }
//     else {
//         for (size_type i = 0; i < s.len_; i++) {
//             auto& c = s.at(i);

//             if (c >= U'a' && c <= U'z')
//                 c -= 32;
//         }
//         // auto& f = std::use_facet<std::ctype<char32_t>>(std::locale(""));
//         // f.toupper(s.data_, s.data_ + s.len_);
//     }
//     return s;
// }

// String& String::trimStart()
// {
//     if (len_ == 0)
//         return *this;

//     size_type i = 0;
//     while (i < len_ && isspace(data_[i])) ++i;

//     if (i == 0)
//         return *this;

//     if (i == len_) {
//         len_     = 0;
//         data_[0] = 0;
//         return *this;
//     }

//     len_ -= i;
//     std::memmove(data_, data_ + i, len_ * sizeof(Char));
//     data_[len_] = 0;

//     return *this;
// }

// String String::trimmedStart() const
// {
//     if (empty())
//         return {};

//     size_type i = 0;

//     while (i < len_ && isspace(data_[i])) ++i;

//     if (i == 0)
//         return *this; // 没有前导空白，直接返回副本

//     if (i == len_)
//         return {}; // 全是空白

//     return String(data_ + i, len_ - i);
// }

// String& String::trimEnd()
// {
//     if (empty())
//         return *this;

//     size_type i = len_;

//     while (i > 0 && isspace(data_[--i])) {
//     }

//     if (i != len_) {
//         len_     = i;
//         data_[i] = 0;
//     }

//     return *this;
// }

// String String::trimmedEnd() const
// {
//     if (empty())
//         return {};

//     size_type i = len_;

//     while (i > 0 && isspace(data_[--i])) {
//     }

//     if (i == len_)
//         return *this;

//     if (i == 0)
//         return {};

//     return String(data_, i);
// }

// String& String::trim()
// {
//     if (empty())
//         return *this;

//     size_type start = 0;
//     size_type end   = len_;

//     while (start < end && isspace(data_[start])) ++start;

//     while (end > start && isspace(data_[end - 1])) --end;

//     if (start == 0 && end == len_)
//         return *this;

//     if (start == end) {
//         len_     = 0;
//         data_[0] = 0;
//         return *this;
//     }

//     size_type new_len = end - start;

//     if (start > 0)
//         std::memmove(data_, data_ + start, new_len * sizeof(Char));

//     len_        = new_len;
//     data_[len_] = 0;

//     return *this;
// }

// String String::trimmed() const
// {
//     if (empty())
//         return {};

//     size_type start = 0;
//     size_type end   = len_;

//     while (start < end && isspace(data_[start])) ++start;
//     while (end > start && isspace(data_[end - 1])) --end;

//     if (start == 0 && end == len_)
//         return *this;

//     if (start == end)
//         return {};

//     return String(data_ + start, end - start);
// }

// bool String::equals(const String& other, bool ignore_case) const
// {
//     if (len_ == 0 && other.len_ == 0)
//         return true;

//     if (len_ != other.len_)
//         return false;

//     if (ignore_case) {
//         for (size_type i = 0; i < len_; ++i) {
//             if (!iequals(data_[i], other.data_[i]))
//                 return false;
//         }
//     }
//     else {
//         return memcmp(data_, other.data_, len_ * sizeof(Char)) == 0;
//     }

//     return true;
// }

// bool String::startsWith(Char c, bool ignore_case) const
// {
//     if (len_ == 0)
//         return false;

//     if (ignore_case) {
//         return iequals(*data_, c);
//     }
//     else {
//         return *data_ == c;
//     }
// }

// bool String::startsWith(const String& str, bool ignore_case) const
// {
//     if (str.len_ == 0)
//         return true;

//     if (len_ < str.len_)
//         return false;

//     if (ignore_case)
//         return memcmp(data_, str.data_, str.len_ * sizeof(Char)) == 0;

//     for (size_type i = 0; i < str.len_; i++) {
//         if (iequals(data_[i], str.data_[i]))
//             continue;

//         return false;
//     }

//     return true;
// }

// bool String::endsWith(Char c, bool ignore_case) const
// {
//     if (len_ == 0)
//         return false;

//     if (ignore_case) {
//         return iequals(c, data_[len_ - 1]);
//     }
//     else {
//         return c == data_[len_ - 1];
//     }
// }

// bool String::endsWith(const String& str, bool ignore_case) const
// {
//     if (str.len_ == 0)
//         return true;

//     if (len_ < str.len_)
//         return false;

//     if (ignore_case)
//         return memcmp(data_ + len_ - str.len_, str.data_, str.len_ * sizeof(Char)) == 0;

//     for (size_type i = 0; i < str.len_; i++) {
//         if (iequals(data_[len_ - i - 1], str.data_[str.len_ - i - 1]))
//             continue;

//         return false;
//     }
//     return true;
// }

// String String::fromUtf8(const char* data, size_type count)
// {
//     if (!data)
//         return {};

//     // If count is NPOS, find length until null terminator
//     if (count == NPOS) {
//         count = cstrlen(data);
//     }

//     if (count == 0)
//         return {};

//     String s;

//     unsigned char         c;
//     size_type             i, n, pos = 0;
//     char32_t              c32;
//     std::vector<char32_t> u32;

//     while (pos < count) {
//         c = data[pos++];
//         if ((c & 0b10000000) == 0) {
//             // 1-byte character
//             // U+0000 - U+007F
//             // char0 0xxxxxxx
//             c32 = c;
//             n   = 1;
//         }
//         else if ((c & 0b11100000) == 0b11000000) {
//             // 2-byte character
//             // U+0080 - U+07FF
//             // char0 110xxxxx char1 10xxxxxx
//             c32 = c & 0b00011111;
//             n   = 2;
//         }
//         else if ((c & 0b11110000) == 0b11100000) {
//             // 3-byte character
//             // U+0800 - U+FFFF
//             // char0 1110xxxx char1 10xxxxxx char2 10xxxxxx
//             c32 = c & 0b00001111;
//             n   = 3;
//         }
//         else if ((c & 0b11111000) == 0b11110000) {
//             // 4-byte character
//             // U+10000 - U+10FFFF
//             // char0 11110xxx char1 10xxxxxx char2 10xxxxxx char3 10xxxxxx
//             c32 = c & 0b00000111;
//             n   = 4;
//         }
//         else {
//             // Invalid sequence
//             break;
//         }
//         for (i = 1; i < n && pos < count; i++) {
//             c = data[pos++];
//             if ((c & 0b11000000) != 0b10000000)
//                 break;
//             c32 = (c32 << 6) + (c & 0b00111111);
//         }
//         u32.push_back(c32);
//     }
//     s.assign(u32.data(), u32.size());
//     return s;
// }

// String String::fromUtf8(const char8_t* data, size_type count)
// {
//     return fromUtf8(reinterpret_cast<const char*>(data), count);
// }

// String String::fromUtf16(const char16_t* data, size_type count)
// {
//     if (!data)
//         return {};

//     // If count is NPOS, find length until null terminator
//     if (count == NPOS) {
//         count = cstrlen(data);
//     }

//     if (count == 0)
//         return {};

//     String                s;
//     std::vector<char32_t> u32;

//     for (size_type pos = 0; pos < count; ++pos) {
//         char16_t c16 = data[pos];
//         char32_t c32;

//         // Check for high surrogate (0xD800-0xDBFF)
//         if (c16 >= 0xD800 && c16 <= 0xDBFF) {
//             if (pos + 1 < count) {
//                 char16_t low = data[pos + 1];

//                 // Check for low surrogate (0xDC00-0xDFFF)
//                 if (low >= 0xDC00 && low <= 0xDFFF) {
//                     pos++;
//                     // Convert surrogate pair to code point
//                     // Formula: 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00)
//                     c32 = 0x10000 + (((c16 - 0xD800) << 10) | (low - 0xDC00));
//                 }
//                 else {
//                     // Invalid surrogate pair, replace with replacement character
//                     c32 = 0xFFFD;
//                 }
//             }
//             else {
//                 // Incomplete surrogate pair at end
//                 c32 = 0xFFFD;
//             }
//         }
//         // Check for low surrogate without high surrogate (invalid)
//         else if (c16 >= 0xDC00 && c16 <= 0xDFFF) {
//             c32 = 0xFFFD; // Replacement character
//         }
//         else {
//             c32 = c16;
//         }

//         u32.push_back(c32);
//     }

//     s.assign(u32.data(), u32.size());
//     return s;
// }

// String String::fromLocal8Bit(const char* data, size_type count)
// {
//     if (!data)
//         return {};

//     // If count is NPOS, find length until null terminator
//     if (count == NPOS) {
//         count = cstrlen(data);
//     }

//     if (count == 0)
//         return {};

// #if defined(_WIN32) || defined(_WIN64)
//     // Windows: Use Win32 API to convert from local 8-bit (ANSI) to UTF-16, then to UTF-32
//     int len = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(count), nullptr, 0);
//     if (len == 0)
//         return {};

//     std::vector<wchar_t> wbuf(len);
//     MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(count), wbuf.data(), len);

//     // Convert UTF-16 to UTF-32
//     std::vector<char32_t> u32;
//     u32.reserve(len); // Approximate
//     for (size_type i = 0; i < len; ++i) {
//         char16_t c16 = static_cast<char16_t>(wbuf[i]);
//         if (c16 < 0xD800 || c16 > 0xDFFF) {
//             u32.push_back(static_cast<char32_t>(c16));
//         }
//         else if (c16 >= 0xD800 && c16 <= 0xDBFF && i + 1 < len) {
//             char16_t low = static_cast<char16_t>(wbuf[i + 1]);
//             if (low >= 0xDC00 && low <= 0xDFFF) {
//                 char32_t c32 = 0x10000 + ((c16 - 0xD800) << 10) + (low - 0xDC00);
//                 u32.push_back(c32);
//                 ++i;
//             }
//             else {
//                 // Invalid surrogate
//                 u32.push_back(0xFFFD);
//             }
//         }
//         else {
//             // Invalid surrogate
//             u32.push_back(0xFFFD);
//         }
//     }

//     String s;
//     s.assign(u32.data(), u32.size());
//     return s;
// #else
//     // Linux/Other: Not implemented yet
//     throw Exception(Exception::Code::NOT_IMPLEMENTED,
//                     String::fromUtf8("fromLocal8Bit not implemented on this platform"));
// #endif
// }

// String& String::operator=(const String& right)
// {
//     assign(right.data_, right.len_);
//     return *this;
// }

// bool String::operator==(const String& right) const
// {
//     return equals(right);
// }

// bool String::operator!=(const String& right) const
// {
//     return !(*this == right);
// }

// bool String::operator<(const String& right) const
// {
//     if (data_ && right.data_) {
//         return memcmp(data_, right.data_, std::min(len_, right.len_));
//     }
//     else if (data_) {
//     }
//     else if (right.data_) {
//     }
//     else {
//         return false;
//     }
//     return false;
// }

// bool String::operator>(const String& right) const
// {
//     return false;
// }

// String String::operator+(const String& right) const
// {
//     if (right.len_ == 0)
//         return *this;

//     if (len_ == 0)
//         return right;

//     String result;

//     return result;
// }

// String& String::operator+=(const String& right)
// {
//     return *this;
// }
// // 6529070966911826 2035/09/30/ 505
// int String::toInt(bool* ok) const
// {
//     if (ok)
//         *ok = false;

//     // Empty string
//     if (len_ == 0)
//         return 0;

//     size_type i = 0;

//     // Skip leading whitespace
//     while (i < len_ && isspace(data_[i])) ++i;

//     // No non-whitespace characters
//     if (i >= len_)
//         return 0;

//     // Handle optional sign
//     int sign = 1;
//     if (data_[i] == U'-') {
//         sign = -1;
//         ++i;
//     }
//     else if (data_[i] == U'+') {
//         ++i;
//     }

//     // Ensure we have at least one digit
//     if (i >= len_ || !isdigit(data_[i]))
//         return 0;

//     // Convert digits with overflow checking
//     long long result = 0;
//     while (i < len_ && isdigit(data_[i])) {
//         int digit = data_[i] - U'0';
//         result    = result * 10 + digit;
//         // Check overflow: result should fit in int range
//         if (sign > 0 && result > INT_MAX) {
//             return 0;
//         }
//         if (sign < 0 && -result < INT_MIN) {
//             return 0;
//         }
//         ++i;
//     }

//     if (ok)
//         *ok = true;

//     return (int)(sign * result);
// }

// double String::toDouble(bool* ok) const
// {
//     if (ok)
//         *ok = false;

//     if (len_ == 0)
//         return 0.0;

//     // Convert Char* (char32_t) to UTF-8 byte string for strtod
//     // We use a simple approach: only support ASCII characters for double parsing
//     std::string buffer;
//     buffer.reserve(len_);

//     size_type i = 0;

//     // Copy string, converting char32_t to char (ASCII only)
//     for (i = 0; i < len_; ++i) {
//         if (data_[i] > 127) {
//             // Non-ASCII character, cannot convert
//             return 0.0;
//         }
//         buffer += static_cast<char>(data_[i]);
//     }

//     // Use strtod for robust parsing
//     char*  endPtr = nullptr;
//     double result = std::strtod(buffer.c_str(), &endPtr);

//     if (ok) {
//         // Check if at least some characters were consumed
//         *ok = (endPtr != buffer.c_str());
//     }

//     return result;
// }

// std::u8string String::toUtf8() const
// {
//     if (len_ == 0)
//         return {};

//     std::u8string result;

//     for (size_t i = 0; i < len_; ++i) {
//         char32_t c = data_[i];

//         if (c < 0x80) { // 1-byte character
//             result += static_cast<char8_t>(c);
//         }
//         else if (c < 0x800) { // 2-byte character
//             result += static_cast<char8_t>(0xC0 | (c >> 6));
//             result += static_cast<char8_t>(0x80 | (c & 0x3F));
//         }
//         else if (c < 0x10000) { // 3-byte character
//             result += static_cast<char8_t>(0xE0 | (c >> 12));
//             result += static_cast<char8_t>(0x80 | ((c >> 6) & 0x3F));
//             result += static_cast<char8_t>(0x80 | (c & 0x3F));
//         }
//         else if (c < 0x110000) { // 4-byte character
//             result += static_cast<char8_t>(0xF0 | (c >> 18));
//             result += static_cast<char8_t>(0x80 | ((c >> 12) & 0x3F));
//             result += static_cast<char8_t>(0x80 | ((c >> 6) & 0x3F));
//             result += static_cast<char8_t>(0x80 | (c & 0x3F));
//         }
//     }

//     return result;
// }

// std::u16string String::toUtf16() const
// {
//     if (len_ == 0)
//         return {};

//     std::u16string result;

//     for (size_t i = 0; i < len_; ++i) {
//         char32_t c = data_[i];

//         if (c < 0x10000) { // BMP character, can be represented as single code unit
//             result += static_cast<char16_t>(c);
//         }
//         else if (c < 0x110000) { // Supplementary character, needs surrogate pair
//             // Formula: high = ((c - 0x10000) >> 10) + 0xD800
//             //          low = ((c - 0x10000) & 0x3FF) + 0xDC00
//             c -= 0x10000;
//             result += static_cast<char16_t>(0xD800 + (c >> 10));
//             result += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
//         }
//     }

//     return result;
// }

// std::string String::toLocal8Bit() const
// {
//     if (len_ == 0)
//         return {};

// #if defined(_WIN32) || defined(_WIN64)
//     // First, convert to UTF-16
//     std::u16string utf16 = toUtf16();

//     // Then convert UTF-16 to local 8-bit (ANSI)
//     int len = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(utf16.c_str()),
//                                    static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
//     if (len == 0)
//         return {};

//     std::string result(len, '\0');
//     WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<const wchar_t*>(utf16.c_str()),
//                         static_cast<int>(utf16.size()), result.data(), len, nullptr, nullptr);

//     return result;
// #else
//     // Linux/Other: Not implemented yet
//     throw Exception(Exception::Code::NOT_IMPLEMENTED,
//                     String::fromUtf8("toLocal8Bit not implemented on this platform"));
// #endif
// }

// String String::format(const char* fmt, ...)
// {
//     if (!fmt)
//         return {};

//     va_list args;
//     va_start(args, fmt);

//     // First, determine the required buffer size
//     va_list args_copy;
//     va_copy(args_copy, args);
//     int size = vsnprintf(nullptr, 0, fmt, args_copy);
//     va_end(args_copy);

//     if (size <= 0)
//         return {};

//     // Allocate buffer and format
//     std::string buffer(size + 1, '\0');
//     vsnprintf(buffer.data(), buffer.size(), fmt, args);
//     va_end(args);

//     // Convert to UTF-8 and then to String
//     return fromUtf8(buffer.c_str());
// }

// std::vector<String> String::split(Char delimiter) const
// {
//     std::vector<String> result;
//     if (empty())
//         return result;

//     size_t start = 0;
//     size_t pos = 0;

//     while (pos < len_) {
//         if (data_[pos] == delimiter) {
//             result.emplace_back(substr(start, pos - start));
//             start = pos + 1;
//         }
//         ++pos;
//     }

//     // Add the last part
//     result.emplace_back(substr(start, pos - start));

//     return result;
// }

// std::vector<String> String::split(const String& delimiter) const
// {
//     std::vector<String> result;
//     if (empty())
//         return result;

//     if (delimiter.empty()) {
//         // If delimiter is empty, split into individual characters
//         result.reserve(len_);
//         for (size_t i = 0; i < len_; ++i) {
//             result.emplace_back(&data_[i], 1);
//         }
//         return result;
//     }

//     size_t start = 0;
//     size_t pos = 0;

//     while (pos < len_) {
//         size_t found = find(delimiter, pos);
//         if (found == NPOS) {
//             // No more delimiters, add the rest
//             result.emplace_back(substr(start));
//             break;
//         }

//         // Add the substring before the delimiter
//         result.emplace_back(substr(start, found - start));
//         pos = found + delimiter.len_;
//         start = pos;
//     }

//     return result;
// }

// VN_CORE_NS_END
