#pragma once

#include <array>
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <vine/geometry/Array.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/io/robot_io_global.hpp>
#include <vine/String.hpp>

VN_ROBOTICS_IO_NS_BEGIN

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
inline std::filesystem::path vfsPath(const vn::String& text)
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
inline vn::io::Result<vn::String> readText(const vn::io::Vfs& vfs, const std::filesystem::path& path)
{
    const auto bytes = vfs.read(path);
    if (!bytes) {
        return bytes.error();
    }
    const std::vector<unsigned char>& data = bytes.value();
    if (data.empty()) {
        return vn::String{};
    }
    return vn::String::fromUtf8(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

/**
 * @brief Writes UTF-8 text as a virtual file.
 *
 * @param vfs The file system to write to.
 * @param path The virtual file path.
 * @param text The UTF-8 text to store.
 * @return The failure addFile() would report.
 */
inline vn::io::IoError writeText(vn::io::Vfs& vfs, const std::filesystem::path& path, const vn::String& text)
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
    return String::fromUtf8(std::string_view(buffer.data(), length));
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
    return String::fromUtf8(out);
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
    return String::fromUtf8(out);
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

static_assert(sizeof(vn::math::Vec3f) == 3U * sizeof(float), "a geometry bin holds a packed float3 array");
static_assert(sizeof(vn::math::Vec2f) == 2U * sizeof(float), "a geometry bin holds a packed float2 array");

/**
 * @brief A sink that appends raw bytes to a typed array.
 *
 * Reading an entry straight into the array it describes needs no byte buffer of its own: the array grows as the bytes
 * arrive. A byte count that does not end on an element boundary is not content that can be read, so it is reported by
 * finalize() - the same rule every other sink follows.
 *
 * @tparam T The element type; it has to be a plain type, since the bytes are the array's own layout.
 */
template <typename T>
class ArraySink final : public vn::io::DataSink
{
  public:
    /**
     * @brief Builds a sink that fills an array.
     *
     * @param out The array to fill; it is cleared.
     */
    explicit ArraySink(std::vector<T>& out) : out_(out)
    {
        static_assert(std::is_trivially_copyable_v<T>, "an array read from bytes has to be a plain type");
        static_assert(sizeof(T) <= sizeof(pending_), "the partial element has to fit the buffer");
        out_.clear();
    }

    /**
     * @brief Appends one chunk.
     *
     * @param bytes The bytes to append.
     * @return IoError::Ok always; the array only fails by throwing, which the caller sees as such.
     */
    vn::io::IoError write(std::span<const std::byte> bytes) override
    {
        std::size_t at = 0;
        if (pending_size_ > 0) {
            // Completing a partly received element first, so the tail of the previous chunk is not dropped.
            const std::size_t take = std::min(sizeof(T) - pending_size_, bytes.size());
            std::memcpy(pending_.data() + pending_size_, bytes.data(), take);
            pending_size_ += take;
            at = take;
            if (pending_size_ == sizeof(T)) {
                T value{};
                std::memcpy(&value, pending_.data(), sizeof(T));
                out_.push_back(value);
                pending_size_ = 0;
            }
        }

        if (at < bytes.size()) {
            const std::size_t whole = (bytes.size() - at) / sizeof(T);
            if (whole > 0) {
                const std::size_t base = out_.size();
                out_.resize(base + whole);
                std::memcpy(reinterpret_cast<std::byte*>(out_.data()) + base * sizeof(T), bytes.data() + at,
                            whole * sizeof(T));
                at += whole * sizeof(T);
            }
            const std::size_t rest = bytes.size() - at;
            if (rest > 0) {
                std::memcpy(pending_.data(), bytes.data() + at, rest);
                pending_size_ = rest;
            }
        }
        return vn::io::IoError::Ok;
    }

    /**
     * @brief Reports whether the content ended on an element boundary.
     *
     * @return IoError::Ok when it did, IoError::InvalidData when a partial element is left over.
     */
    [[nodiscard]] vn::io::IoError finalize() const noexcept
    {
        return pending_size_ == 0 ? vn::io::IoError::Ok : vn::io::IoError::InvalidData;
    }

  private:
    std::vector<T>&          out_;
    std::array<std::byte, 16> pending_{};      ///< Bytes of an element that has not arrived whole.
    std::size_t              pending_size_{ 0 };
};

/**
 * @brief Reads exactly as many bytes as the span holds.
 *
 * @param source The entry being read.
 * @param out Receives the bytes.
 * @return true when they all arrived, false when the entry ended first.
 */
inline bool readExact(vn::io::VfsEntrySource& source, std::span<std::byte> out)
{
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t got = source.read(out.subspan(done));
        if (got == 0) {
            return false; // the entry ended before the bytes it said it holds
        }
        done += got;
    }
    return true;
}

/**
 * @brief Reads exactly one array's bytes out of an entry that is being streamed.
 *
 * @tparam T The array's element type.
 * @param source The entry being read.
 * @param count Elements to read.
 * @param out Receives the array; it is cleared first.
 * @return true when the bytes arrived and ended on an element boundary.
 */
template <typename T>
bool readBlock(vn::io::VfsEntrySource& source, std::uint64_t count, std::vector<T>& out)
{
    ArraySink<T>  sink(out);
    std::uint64_t bytes = count * sizeof(T);
    std::uint64_t done  = 0;
    std::array<std::byte, 64U * 1024U> chunk{};
    while (done < bytes) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - done));
        const std::size_t got  = source.read(std::span<std::byte>(chunk.data(), want));
        if (got == 0) {
            return false;
        }
        if (sink.write(std::span<const std::byte>(chunk.data(), got)) != vn::io::IoError::Ok) {
            return false;
        }
        done += got;
    }
    return sink.finalize() == vn::io::IoError::Ok;
}

} // namespace detail

VN_ROBOTICS_IO_NS_END
