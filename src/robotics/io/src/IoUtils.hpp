#pragma once

#include <array>
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <vine/geometry/Array.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/String.hpp>

V_ROBOTICS_IO_NS_BEGIN

namespace detail
{

/**
 * @brief The virtual path a UTF-8 text spells.
 *
 * Document attributes hold paths as text while the file system speaks
 * std::filesystem::path, so this is where the two meet. The bytes are taken as UTF-8,
 * which is what the file system and every archive below it store.
 *
 * @param text The path as written, in UTF-8.
 * @return The path; an empty text is the virtual root.
 */
inline std::filesystem::path vfsPath(const vine::String& text)
{
    return std::filesystem::path(text.as_std_u8str());
}

/**
 * @brief Reads a whole virtual file as UTF-8 text.
 *
 * The file system itself deals in bytes; this is the text view the XML parsers need.
 *
 * @param vfs The file system to read from.
 * @param path The virtual file path.
 * @return The text, or the failure read() would report.
 */
inline vine::io::Result<vine::String> readText(const vine::io::Vfs& vfs, const std::filesystem::path& path)
{
    const auto bytes = vfs.read(path);
    if (!bytes) {
        return bytes.error();
    }
    const std::vector<unsigned char>& data = bytes.value();
    if (data.empty()) {
        return vine::String{};
    }
    return vine::String(reinterpret_cast<const char8_t*>(data.data()), data.size());
}

/**
 * @brief Writes UTF-8 text as a virtual file.
 *
 * @param vfs The file system to write to.
 * @param path The virtual file path.
 * @param text The UTF-8 text to store.
 * @return The failure addFile() would report.
 */
inline vine::io::IoError writeText(vine::io::Vfs& vfs, const std::filesystem::path& path, const vine::String& text)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    return vfs.addFile(path, std::span<const unsigned char>(bytes, text.size()));
}

/**
 * @brief Appends a printf-formatted warning line to a diagnostics buffer.
 *
 * Per-operation diagnostics live on the parse/export context, keeping the IO
 * classes stateless and reentrant.
 *
 * @param msgs The diagnostics buffer to append to.
 * @param fmt printf-style format string.
 * @param ... Arguments.
 */
inline void appendWarning(std::string& msgs, const char* fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int length = std::vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);
    if (length > 0) {
        std::string buffer(static_cast<std::size_t>(length), '\0');
        std::vsnprintf(buffer.data(), static_cast<std::size_t>(length) + 1, fmt, args);
        if (!msgs.empty()) {
            msgs += '\n';
        }
        msgs += buffer;
    }
    va_end(args);
}

/**
 * @brief Converts a double to its shortest round-trip string.
 *
 * @param value The value to convert.
 * @return The decimal string.
 */
inline String doubleToStr(double value)
{
    std::array<char, 32> buffer;
    const auto           result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc()) {
        return String(u8"0");
    }
    const std::size_t length = static_cast<std::size_t>(result.ptr - buffer.data());
    return String(reinterpret_cast<const char8_t*>(buffer.data()), length);
}

/**
 * @brief Parses a double from a string.
 *
 * @param str The decimal string.
 * @param out Receives the value.
 * @return true when the whole string was a valid double.
 */
inline bool strToDouble(const String& str, double& out)
{
    std::string     text   = str.as_std_str();
    double          value  = 0.0;
    const auto      result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc() && result.ptr == text.data() + text.size()) {
        out = value;
        return true;
    }
    return false;
}

/**
 * @brief Converts joint values to a space-separated string.
 *
 * @param q The joint values.
 * @return The space-separated string.
 */
inline String qToStr(const kinematics::Q& q)
{
    std::string out;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (i) {
            out.push_back(' ');
        }
        std::array<char, 32> buffer;
        const auto           result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), q[i]);
        out.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
    }
    return String(reinterpret_cast<const char8_t*>(out.data()), out.size());
}

/**
 * @brief Parses a space-separated string into joint values.
 *
 * @param str The space-separated numbers.
 * @param out Receives the joint values.
 * @return true when at least one value was parsed.
 */
inline bool strToQ(const String& str, kinematics::Q& out)
{
    out = kinematics::Q{};
    std::istringstream stream(str.as_std_str());
    double             value = 0.0;
    while (stream >> value) {
        out.append(value);
    }
    return !out.empty();
}

/**
 * @brief Converts a 3-vector to a space-separated string.
 *
 * @param v The vector.
 * @return The space-separated string.
 */
inline String vec3ToStr(const math::Vec3d& v)
{
    std::string out = std::string(doubleToStr(v.x).as_std_str()) + ' ' + std::string(doubleToStr(v.y).as_std_str())
                      + ' ' + std::string(doubleToStr(v.z).as_std_str());
    return String(reinterpret_cast<const char8_t*>(out.data()), out.size());
}

/**
 * @brief Parses a space-separated string into a 3-vector.
 *
 * @param str The space-separated numbers.
 * @param out Receives the vector.
 * @return true on success.
 */
inline bool strToVec3(const String& str, math::Vec3d& out)
{
    double x = 0.0, y = 0.0, z = 0.0;
    std::istringstream stream(str.as_std_str());
    if (!(stream >> x >> y >> z)) {
        return false;
    }
    out = math::Vec3d(x, y, z);
    return true;
}

/**
 * @brief Returns the parent directory part of a VFS path.
 *
 * @param path The VFS path.
 * @return The parent path, or an empty path when path has no parent.
 */
inline std::filesystem::path vfsParentDir(const std::filesystem::path& path)
{
    return path.parent_path();
}

/**
 * @brief Converts a filesystem path to its file name as a virtual path.
 *
 * The name is taken from the path's own form: the native narrow form would encode a
 * non-ASCII name in the local code page, which the file system - and every archive
 * below it - would then read as broken UTF-8.
 *
 * @param path The filesystem path.
 * @return The leaf name.
 */
inline std::filesystem::path pathLeafName(const std::filesystem::path& path)
{
    return path.filename();
}

/**
 * @brief Checks whether a string ends with the given suffix.
 *
 * @param text The string to test.
 * @param suffix The suffix to look for.
 * @return true when text ends with suffix.
 */
inline bool endsWith(const String& text, const char* suffix)
{
    const std::string& t = text.as_std_str();
    const std::string  s(suffix);
    return s.size() <= t.size() && t.compare(t.size() - s.size(), s.size(), s) == 0;
}

/**
 * @brief Appends a float3 array as a little-endian byte buffer.
 *
 * @param arr The values to write.
 * @param out Receives the bytes.
 */
inline void vec3ArrayToBytes(std::span<const vine::math::Vec3f> arr, std::vector<unsigned char>& out)
{
    out.clear();
    out.reserve(arr.size() * 3u * sizeof(float));
    for (const auto& v : arr) {
        const float       vals[3] = { v.x, v.y, v.z };
        const auto* const p       = reinterpret_cast<const unsigned char*>(vals);
        out.insert(out.end(), p, p + sizeof(vals));
    }
}

/**
 * @brief Appends a float2 array as a little-endian byte buffer.
 *
 * @param arr The values to write.
 * @param out Receives the bytes.
 */
inline void vec2ArrayToBytes(std::span<const vine::math::Vec2f> arr, std::vector<unsigned char>& out)
{
    out.clear();
    out.reserve(arr.size() * 2u * sizeof(float));
    for (const auto& v : arr) {
        const float       vals[2] = { v.x, v.y };
        const auto* const p       = reinterpret_cast<const unsigned char*>(vals);
        out.insert(out.end(), p, p + sizeof(vals));
    }
}

/**
 * @brief Appends a uint32 array as a little-endian byte buffer.
 *
 * @param arr The values to write.
 * @param out Receives the bytes.
 */
inline void uint32ArrayToBytes(std::span<const std::uint32_t> arr, std::vector<unsigned char>& out)
{
    out.clear();
    out.reserve(arr.size() * sizeof(std::uint32_t));
    for (const std::uint32_t v : arr) {
        const auto* const p = reinterpret_cast<const unsigned char*>(&v);
        out.insert(out.end(), p, p + sizeof(v));
    }
}

/**
 * @brief Parses a byte buffer into a float3 array.
 *
 * @param bytes The buffer.
 * @param out Receives the array.
 * @return true when the byte count is a multiple of 12.
 */
inline bool bytesToVec3Array(const std::vector<unsigned char>& bytes, vine::geometry::Vec3fArray& out)
{
    if (bytes.size() % (3u * sizeof(float)) != 0) {
        return false;
    }
    out.clear();
    out.reserve(bytes.size() / (3u * sizeof(float)));
    const float*      p     = reinterpret_cast<const float*>(bytes.data());
    const std::size_t count = bytes.size() / (3u * sizeof(float));
    for (std::size_t i = 0; i < count; ++i) {
        out.emplace_back(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
    }
    return true;
}

/**
 * @brief Parses a byte buffer into a float2 array.
 *
 * @param bytes The buffer.
 * @param out Receives the array.
 * @return true when the byte count is a multiple of 8.
 */
inline bool bytesToVec2Array(const std::vector<unsigned char>& bytes, vine::geometry::Vec2fArray& out)
{
    if (bytes.size() % (2u * sizeof(float)) != 0) {
        return false;
    }
    out.clear();
    out.reserve(bytes.size() / (2u * sizeof(float)));
    const float*      p     = reinterpret_cast<const float*>(bytes.data());
    const std::size_t count = bytes.size() / (2u * sizeof(float));
    for (std::size_t i = 0; i < count; ++i) {
        out.emplace_back(p[i * 2], p[i * 2 + 1]);
    }
    return true;
}

/**
 * @brief Parses a byte buffer into a uint32 array.
 *
 * @param bytes The buffer.
 * @param out Receives the array.
 * @return true when the byte count is a multiple of 4.
 */
inline bool bytesToUInt32Array(const std::vector<unsigned char>& bytes, vine::geometry::UInt32Array& out)
{
    if (bytes.size() % sizeof(std::uint32_t) != 0) {
        return false;
    }
    out.clear();
    out.reserve(bytes.size() / sizeof(std::uint32_t));
    const std::uint32_t* p     = reinterpret_cast<const std::uint32_t*>(bytes.data());
    const std::size_t    count = bytes.size() / sizeof(std::uint32_t);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(p[i]);
    }
    return true;
}

} // namespace detail

V_ROBOTICS_IO_NS_END
