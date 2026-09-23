#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>
#include <vine/io/Stream.hpp>
#include <vine/String.hpp>

V_IO_NS_BEGIN

namespace detail
{

/**
 * @brief Converts a filesystem path to the UTF-8 byte form libzip expects.
 *
 * @param path The path to convert; native separators are kept as they are.
 * @return The path as a UTF-8 byte string.
 */
inline std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

/**
 * @brief Builds a String from UTF-8 bytes of a known length.
 *
 * @param bytes The UTF-8 bytes; they do not have to be NUL-terminated.
 * @param length Number of bytes to take.
 * @return The decoded string.
 */
inline String fromUtf8(const char* bytes, std::size_t length)
{
    return String(reinterpret_cast<const char8_t*>(bytes), static_cast<String::size_type>(length));
}

/**
 * @brief Normalizes a VFS path and validates it.
 *
 * A virtual path is a '/'-separated sequence of segments relative to the
 * virtual root, which knows no working directory: a leading '/' would name no
 * entry, so it is refused instead of being folded away. Empty segments and "."
 * are folded away, ".." cancels the previous segment, and a path that would
 * climb above the root is invalid.
 * A backslash, an embedded NUL and a ':' are rejected because different
 * backends would read them differently - a drive letter in particular turns a
 * path into an absolute one, and joining it would replace a backend's root.
 *
 * @param path The raw virtual path; the empty string denotes the virtual root.
 * @param out Receives the normalized path, without leading or trailing '/' and
 *            without empty segments; the empty string denotes the root.
 *            Untouched on failure.
 * @return IoError::Ok, or IoError::InvalidPath when the path is not valid.
 */
inline IoError normalizeVfsPath(const String& path, String& out)
{
    const std::u8string& raw = path.as_std_u8str();

    if (raw.find(u8'\\') != std::u8string::npos || raw.find(u8'\0') != std::u8string::npos) {
        return IoError::InvalidPath;
    }
    if (!raw.empty() && raw.front() == u8'/') {
        return IoError::InvalidPath; // no working directory: an absolute spelling names no entry
    }

    std::u8string normalized;
    normalized.reserve(raw.size());

    for (std::size_t begin = 0; begin <= raw.size();) {
        const std::size_t        slash = raw.find(u8'/', begin);
        const std::size_t        end   = slash == std::u8string::npos ? raw.size() : slash;
        const std::u8string_view segment(raw.data() + begin, end - begin);
        begin = end + 1;

        if (segment.empty() || segment == u8".") {
            continue; // the root itself, a repeated separator, or "."
        }
        if (segment == u8"..") {
            if (normalized.empty()) {
                return IoError::InvalidPath; // climbs above the virtual root
            }
            const std::size_t cut = normalized.find_last_of(u8'/');
            normalized.resize(cut == std::u8string::npos ? 0 : cut);
            continue;
        }
        if (segment.find(u8':') != std::u8string_view::npos) {
            return IoError::InvalidPath; // drive letter or scheme-like segment
        }
        if (!normalized.empty()) {
            normalized.push_back(u8'/');
        }
        normalized.append(segment);
    }

    out = String(std::move(normalized));
    return IoError::Ok;
}

/**
 * @brief Reports whether a fragment list holds a piece that points at no bytes.
 *
 * An empty piece is fine, but a non-empty one has to carry real bytes: the
 * pieces are borrowed rather than copied, so a null pointer would be
 * dereferenced once the content is stored.
 *
 * @param fragments The pieces to inspect.
 * @return true when a non-empty piece has a null data pointer.
 */
inline bool hasDatalessFragment(std::span<const Fragment> fragments)
{
    for (const Fragment& piece : fragments) {
        if (piece.size != 0 && piece.data == nullptr) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Reports whether one normalized virtual path lies strictly below another.
 *
 * @param parent The ancestor path (normalized); empty denotes the virtual root.
 * @param path The path to test (normalized).
 * @return true when path names an entry inside parent rather than parent itself.
 */
inline bool isPathBelow(const String& parent, const String& path)
{
    const std::u8string prefix = parent.empty() ? std::u8string() : parent.as_std_u8str() + u8"/";
    return path.size() > prefix.size() && path.as_std_u8str().compare(0, prefix.size(), prefix) == 0;
}

/**
 * @brief The parent of a normalized virtual path.
 *
 * @param path A normalized, non-empty virtual path.
 * @return The parent path, or the empty string when path sits at the root.
 */
inline String parentOf(const String& path)
{
    const std::size_t slash = path.as_std_u8str().find_last_of(u8'/');
    return slash == std::u8string::npos ? String{} : String(path.as_std_u8str().substr(0, slash));
}

/**
 * @brief The last segment of a normalized virtual path.
 *
 * @param path A normalized virtual path; empty denotes the virtual root.
 * @return The name, or an empty string for the root.
 */
inline String nameOf(const String& path)
{
    const std::size_t slash = path.as_std_u8str().find_last_of(u8'/');
    return slash == std::u8string::npos ? path : String(path.as_std_u8str().substr(slash + 1));
}

/**
 * @brief Reports whether a flat path index holds anything below a directory.
 *
 * @tparam Index A map from normalized virtual path to any value; only keys matter.
 * @param index The index to scan.
 * @param normalized The directory path (normalized); empty denotes the root.
 * @return true when at least one key lies strictly below normalized.
 */
template <typename Index>
bool indexHasChildOf(const Index& index, const String& normalized)
{
    for (const auto& entry : index) {
        if (isPathBelow(normalized, entry.first)) {
            return true;
        }
    }
    return false;
}

} // namespace detail

V_IO_NS_END
