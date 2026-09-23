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

V_IO_NS_BEGIN

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
    String        name;                  ///< Stored name; a directory ends with '/'.
    std::uint64_t size{ 0 };             ///< Uncompressed size in bytes; 0 for a directory.
    std::uint32_t crc{ 0 };              ///< Checksum recorded by the archive; 0 for a directory.
    bool          is_directory{ false }; ///< true when the stored name marks a directory.
};

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
        if (zip_stat_index(archive, i, ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC, &st) != 0 || st.name == nullptr) {
            return IoError::InvalidData;
        }

        StoredEntry entry;
        entry.name         = fromUtf8(st.name, std::strlen(st.name));
        entry.is_directory = !entry.name.empty() && entry.name.as_std_u8str().back() == u8'/';
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
 * @brief The name a ZIP stores for an entry; directories end with '/'.
 *
 * @param name The virtual path of the entry.
 * @param is_directory true when the entry is a directory marker.
 * @return The name as the archive stores it.
 */
inline std::string toStoredName(const String& name, bool is_directory)
{
    std::string stored(reinterpret_cast<const char*>(name.data()), static_cast<std::size_t>(name.size()));
    if (is_directory && !stored.empty() && stored.back() != '/') {
        stored.push_back('/');
    }
    return stored;
}

/**
 * @brief Removes the trailing '/' a ZIP stores for a directory.
 *
 * @param name The stored name.
 * @param is_directory Receives whether the stored name marked a directory.
 * @return The name as a virtual path.
 */
inline String fromStoredName(const String& name, bool& is_directory)
{
    is_directory = !name.empty() && name.as_std_u8str().back() == u8'/';
    if (!is_directory) {
        return name;
    }
    std::u8string trimmed = name.as_std_u8str();
    trimmed.pop_back();
    return String(std::move(trimmed));
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
        bool   is_directory = false;
        String path         = fromStoredName(entry.name, is_directory);
        if (path.empty()) {
            continue;
        }

        VfsEntryInfo info;
        info.path         = std::move(path);
        info.is_directory = is_directory;
        if (!is_directory) {
            info.size = entry.size;
            info.crc  = entry.crc;
        }
        listed.push_back(std::move(info));
    }
    return listed;
}

} // namespace detail

V_IO_NS_END
