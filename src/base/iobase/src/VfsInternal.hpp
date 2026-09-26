#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>
#include <vine/io/Stream.hpp>
#include <vine/String.hpp>

VN_IO_NS_BEGIN

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
 * @brief Converts a filesystem path to the UTF-8 form a virtual path uses.
 *
 * A virtual path is always '/'-separated, so the generic form is the one to take: the
 * native form would spell the separators as '\' on Windows and store them that way in
 * an archive. The bytes are UTF-8 rather than the local code page, because that is what
 * the virtual world - and a ZIP entry name - is made of; the native narrow form
 * (`path::string()`) would silently rewrite a non-ASCII name.
 *
 * @param path The path to convert; native separators are replaced by '/'.
 * @return The path as UTF-8 bytes, with '/' separators.
 */
inline std::string toUtf8Generic(const std::filesystem::path& path)
{
    const std::u8string u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
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
 * There is NO UTF-8 requirement: a virtual path carries a name as bytes, and
 * on a byte-based host the bytes are the name (on Windows the path machinery
 * has already turned the narrow form into text). What a STORED archive name
 * means is decided by the archive layer, which keeps a name that is text in no
 * encoding reachable instead of refusing it (see detail::fromStoredName); a
 * validator that demanded UTF-8 here would make such an entry unreadable - the
 * one outcome the encoding rules forbid.
 *
 * @param path The raw virtual path; the empty path denotes the virtual root.
 * @param out Receives the normalized path, without leading or trailing '/' and
 *            without empty segments; the empty path denotes the root.
 *            Untouched on failure.
 * @return IoError::Ok, or IoError::InvalidPath when the path is not valid.
 */
inline IoError normalizeVfsPath(const std::filesystem::path& path, std::filesystem::path& out)
{
    const std::u8string raw = path.generic_u8string();

#if defined(_WIN32)
    // The host would fold a '\' into '/', which would make the same spelling mean two
    // different trees on two platforms - so it is refused rather than folded.
    if (path.native().find(L'\\') != std::wstring::npos) {
        return IoError::InvalidPath;
    }
#endif

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

    out = std::filesystem::path(normalized);
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
 * @brief Joins a directory and a child name into one virtual path.
 *
 * The result is always spelled the generic way, with '/' separators: the host's
 * operator/ spells them '\' on Windows, and such a path is refused on purpose -
 * a virtual path has to mean the same thing on every platform.
 *
 * @param parent The directory path; empty denotes the virtual root.
 * @param child The path to place inside parent.
 * @return The joined path, in the generic spelling.
 */
inline std::filesystem::path joinVfs(const std::filesystem::path& parent, const std::filesystem::path& child)
{
    if (parent.empty()) {
        return std::filesystem::path(child.generic_u8string());
    }
    return std::filesystem::path(parent.generic_u8string() + u8"/" + child.generic_u8string());
}

/**
 * @brief Reports whether one normalized virtual path lies strictly below another.
 *
 * @param parent The ancestor path (normalized); empty denotes the virtual root.
 * @param path The path to test (normalized).
 * @return true when path names an entry inside parent rather than parent itself.
 */
inline bool isPathBelow(const std::filesystem::path& parent, const std::filesystem::path& path)
{
    const std::u8string prefix = parent.empty() ? std::u8string() : parent.generic_u8string() + u8"/";
    const std::u8string text   = path.generic_u8string();
    return text.size() > prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/**
 * @brief The parent of a normalized virtual path.
 *
 * @param path A normalized virtual path.
 * @return The parent path, or the empty path when path sits at the root.
 */
inline std::filesystem::path parentOf(const std::filesystem::path& path)
{
    return path.parent_path();
}

/**
 * @brief The last segment of a normalized virtual path.
 *
 * @param path A normalized virtual path; empty denotes the virtual root.
 * @return The name, or an empty path for the root.
 */
inline std::filesystem::path nameOf(const std::filesystem::path& path)
{
    return path.filename();
}

} // namespace detail

VN_IO_NS_END
