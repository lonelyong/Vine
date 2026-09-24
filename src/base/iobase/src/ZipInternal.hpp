#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <zip.h>

#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/String.hpp>

#include "VfsInternal.hpp"

VN_IO_NS_BEGIN

namespace detail
{

/**
 * @brief One entry of an open archive's directory, in the form the ZIP stores it.
 *
 * The name is the stored spelling, where a directory ends with '/', and a file
 * carries the checksum the archive recorded. This stays ZIP-private: callers see
 * the same entry through VfsEntryInfo, which carries the kind in is_directory and
 * never a trailing '/'.
 */
struct StoredEntry
{
    std::filesystem::path name;           ///< Virtual path the stored name resolves to.
    std::string           stored;         ///< The name exactly as the archive holds it, trailing '/' included.
    std::uint64_t         size{ 0 };      ///< Uncompressed size in bytes; 0 for a directory.
    std::uint32_t         crc{ 0 };       ///< Checksum recorded by the archive; 0 for a directory.
    bool                  is_directory{ false }; ///< true when the stored name marks a directory.
};

/**
 * @brief Tells whether a stored name marks a directory.
 *
 * @param stored The name exactly as the archive holds it.
 * @return true when it ends with '/', false otherwise.
 */
inline bool storedNameIsDirectory(const char* stored)
{
    if (stored == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(stored);
    return length != 0 && stored[length - 1] == '/';
}

/**
 * @brief Spells bytes that are text in no encoding as one code point per byte.
 *
 * Each byte below 0x80 stays as it is; every other byte becomes the code point with the
 * same value, which is the classic byte-preserving spelling. It is injective, so two
 * different names never collapse into one, and it cannot fail - which is what a listing
 * needs, because an entry that cannot be named cannot be reached at all.
 *
 * @param bytes The bytes to spell.
 * @param length Number of bytes to take.
 * @return The spelling, in UTF-8.
 */
inline std::u8string promoteBytesToText(const char* bytes, std::size_t length)
{
    std::u8string text;
    text.reserve(length * 2); // every byte outside ASCII takes two code units
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char value = static_cast<unsigned char>(bytes[i]);
        if (value < 0x80) {
            text.push_back(static_cast<char8_t>(value));
        }
        else {
            text.push_back(static_cast<char8_t>(0xC0 | (value >> 6)));
            text.push_back(static_cast<char8_t>(0x80 | (value & 0x3F)));
        }
    }
    return text;
}

/**
 * @brief Turns a stored name into a virtual path.
 *
 * @param bytes The stored name; a directory ends with '/'.
 * @param length Number of bytes to take.
 * @param out Receives the virtual path; untouched on failure.
 * @param is_directory Receives whether the stored name marked a directory.
 * @return IoError::Ok, or IoError::InvalidData when the name cannot be decoded.
 */
inline IoError fromStoredName(const char* bytes, std::size_t length, std::filesystem::path& out, bool& is_directory);

/**
 * @brief Reads the entry directory of an open archive.
 *
 * A ZIP marks a directory by a trailing '/'; such an entry carries no data, so
 * its size and checksum are reported as 0.
 *
 * @param archive The open archive to read.
 * @return The entries in archive order, or IoError::InvalidData when the
 *         directory cannot be read.
 */
inline Result<std::vector<StoredEntry>> readEntries(zip_t* archive)
{
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0) {
        return IoError::InvalidData;
    }

    std::vector<StoredEntry> entries;
    entries.reserve(static_cast<std::size_t>(count));
    for (zip_int64_t i = 0; i < count; ++i) {
        struct zip_stat st;
        zip_stat_init(&st);
        // ZIP_FL_ENC_RAW: the name comes back exactly as the archive stores it, after
        // the UTF-8 flag and the Unicode Path extra field have had their say. libzip
        // would otherwise translate anything that is not UTF-8 as CP437, which invents
        // text the archive never held - and a repack would then write that invention out.
        if (zip_stat_index(archive, i, ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC | ZIP_FL_ENC_RAW, &st) != 0 ||
            st.name == nullptr) {
            return IoError::InvalidData;
        }

        StoredEntry entry;
        const std::size_t length = std::strlen(st.name);
        const IoError     named  = fromStoredName(st.name, length, entry.name, entry.is_directory);
        if (named != IoError::Ok) {
            return named; // a name that cannot be read back cannot be handed out
        }
        entry.stored = std::string(st.name, length);
        if (!entry.is_directory && (st.valid & ZIP_STAT_SIZE) != 0) {
            entry.size = static_cast<std::uint64_t>(st.size);
        }
        if (!entry.is_directory && (st.valid & ZIP_STAT_CRC) != 0) {
            entry.crc = static_cast<std::uint32_t>(st.crc);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

/**
 * @brief The name a ZIP stores for an entry, in UTF-8.
 *
 * This is for entries the archive created itself. An entry read from another archive
 * keeps the bytes that archive held (StoredEntry::stored), so a legacy name is written
 * back as it was rather than re-encoded behind the caller's back.
 *
 * @param name The virtual path of the entry.
 * @param is_directory true when the entry is a directory marker.
 * @return The name as the archive stores it.
 */
inline std::string toStoredName(const std::filesystem::path& name, bool is_directory)
{
    std::string stored = toUtf8Generic(name);
    if (is_directory && !stored.empty() && stored.back() != '/') {
        stored.push_back('/');
    }
    return stored;
}

/**
 * @brief Turns a stored name into a virtual path.
 *
 * A ZIP holds a name as bytes and records no code page, so the bytes have to be read as
 * something. The order is fixed, and it never fails:
 *   1. well-formed UTF-8 - what a modern writer stores, and what the archive's own UTF-8
 *      flag or Info-ZIP Unicode Path extra field already upgraded - is taken as it is;
 *   2. otherwise the host's own code page is tried, which is what a legacy writer meant
 *      when it wrote on a machine like this one: a GBK name reads back as its text;
 *   3. a name that is text in neither keeps its bytes, one code point each, so the entry
 *      is still reachable and a repack writes the original bytes back unchanged.
 *
 * @param bytes The stored name; a directory ends with '/'.
 * @param length Number of bytes to take.
 * @param out Receives the virtual path; untouched on failure.
 * @param is_directory Receives whether the stored name marked a directory.
 * @return IoError::Ok, or IoError::InvalidData when the stored name is empty.
 */
inline IoError fromStoredName(const char* bytes, std::size_t length, std::filesystem::path& out, bool& is_directory)
{
    is_directory = length != 0 && bytes[length - 1] == '/';
    const std::size_t kept = is_directory ? length - 1 : length;
    if (kept == 0) {
        return IoError::InvalidData; // an entry naming the root says nothing a tree can use
    }

    const std::u8string raw(reinterpret_cast<const char8_t*>(bytes), kept);
    if (isValidUtf8(raw)) {
        out = std::filesystem::path(raw);
        return IoError::Ok;
    }

    // A legacy name: the bytes are text in whatever code page the writer used, and the only
    // one this machine can stand behind is its own. The conversion reports bytes it cannot
    // spell by throwing, and a name still has to come out - so the last resort keeps every
    // byte as its own code point instead of failing.
    try {
        out = std::filesystem::path(std::string(bytes, kept));
    } catch (const std::exception&) {
        out = std::filesystem::path(promoteBytesToText(bytes, kept));
    }
    return IoError::Ok;
}

/**
 * @brief An entry found by name, with the kind the archive recorded for it.
 */
struct LocatedEntry
{
    zip_int64_t index{ -1 };           ///< Position of the entry in the archive.
    bool        is_directory{ false }; ///< true when the stored name marks a directory.
};

/**
 * @brief Finds the entry an archive holds under a virtual path.
 *
 * The fast path compares the path's UTF-8 spelling with the stored bytes, which is what an
 * archive written by this library holds. A legacy archive stores something else - the same
 * name in the writer's code page - so the entries are then decoded through the same rule
 * the listing uses and compared as text, which is what lets a caller reach an entry whose
 * stored bytes are not the spelling it asks with.
 *
 * @param archive The open archive to search.
 * @param name The virtual path to look for.
 * @param out Receives the entry when it is found; untouched otherwise.
 * @return true when the archive holds the entry, false otherwise.
 */
inline bool locateEntry(zip_t* archive, const std::filesystem::path& name, LocatedEntry& out)
{
    const std::string spelled = toUtf8Generic(name);
    zip_int64_t       index   = zip_name_locate(archive, spelled.c_str(), ZIP_FL_ENC_RAW);
    if (index >= 0) {
        out.index = index;
        struct zip_stat st;
        zip_stat_init(&st);
        out.is_directory = zip_stat_index(archive, index, ZIP_STAT_NAME | ZIP_FL_ENC_RAW, &st) == 0 && storedNameIsDirectory(st.name);
        return true;
    }

    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, i, ZIP_STAT_NAME | ZIP_FL_ENC_RAW, &st) != 0 || st.name == nullptr) {
            continue;
        }
        std::filesystem::path decoded;
        bool                  is_directory = false;
        if (fromStoredName(st.name, std::strlen(st.name), decoded, is_directory) != IoError::Ok) {
            continue;
        }
        if (toUtf8Generic(decoded) == spelled) {
            out.index        = i;
            out.is_directory = is_directory;
            return true;
        }
    }
    return false;
}

/**
 * @brief Converts a stored directory listing into the record the whole IOBase speaks.
 *
 * The two spellings differ in one way: a ZIP marks a directory by a trailing '/',
 * while a virtual path carries that in is_directory. An entry naming the root is
 * left out - the root always exists, and reporting it would only repeat that.
 *
 * @param stored The entries as the archive stores them.
 * @return The entries as VfsEntryInfo, in the order they were stored.
 */
inline std::vector<VfsEntryInfo> toEntryInfos(const std::vector<StoredEntry>& stored)
{
    std::vector<VfsEntryInfo> listed;
    listed.reserve(stored.size());
    for (const StoredEntry& entry : stored) {
        if (entry.name.empty()) {
            continue;
        }

        VfsEntryInfo info;
        info.path         = entry.name;
        info.is_directory = entry.is_directory;
        if (!entry.is_directory) {
            info.size = entry.size;
            info.crc  = entry.crc;
        }
        listed.push_back(std::move(info));
    }
    return listed;
}

} // namespace detail

VN_IO_NS_END
