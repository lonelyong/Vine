#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zip.h>
#include <zlib.h>

#include <vine/String.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>

#include "VfsInternal.hpp"

VN_IO_NS_BEGIN

namespace detail
{

/**
 * @brief Computes the CRC-32 of a byte range, continuing from @p seed.
 *
 * The codec takes a 32-bit length per call, so the range is walked in chunks: an entry larger than 4 GiB gets one
 * checksum, not a truncated one - which is what a single crc32() call with a narrowed length would produce.
 *
 * @param seed The running checksum to continue; 0 starts a new one.
 * @param bytes The bytes to fold in.
 * @return The checksum.
 */
inline std::uint32_t crc32Of(std::uint32_t seed, std::span<const unsigned char> bytes) noexcept
{
    std::uint32_t crc    = seed;
    std::size_t   offset = 0;
    while (offset < bytes.size()) {
        const std::size_t step = std::min<std::size_t>(bytes.size() - offset, 1U << 30);
        crc                    = static_cast<std::uint32_t>(::crc32(crc, bytes.data() + offset, static_cast<uInt>(step)));
        offset += step;
    }
    return crc;
}

/**
 * @brief Computes the CRC-32 of a byte range.
 *
 * @param bytes The bytes to checksum.
 * @return The checksum; 0 for an empty range, which is why a recorded 0 is read as "no checksum".
 */
inline std::uint32_t crc32Of(std::span<const unsigned char> bytes) noexcept
{
    return crc32Of(0, bytes);
}

/**
 * @brief Reads one entry's whole content and verifies it against the archive directory.
 *
 * The one implementation of "read an entry whole", because the three call sites that spelled it themselves had already
 * drifted: one did not force the decompressor past the last byte, so damage went unnoticed, and another folded the
 * checksum in a single call, which truncates the length past 4 GiB.
 *
 * The probe past the end is what makes a damaged member a failure: decompression finishes, and the content is checked,
 * only once the stream is read past its last byte. The checksum is folded here as well as by the reader underneath,
 * so the guarantee does not depend on what that reader happens to validate.
 *
 * @param archive The open archive.
 * @param index The entry's index in that archive.
 * @param size The uncompressed size the directory states.
 * @param expected_crc The checksum it recorded, or 0 when none is recorded.
 * @return The content; IoError::CapacityExceeded when the stated size passes kMaxWholeContentBytes, IoError::IoFailure
 *         when the entry cannot be read whole or does not match what the directory promised.
 */
inline Result<std::vector<unsigned char>> readWholeEntry(zip_t* archive, zip_int64_t index, std::uint64_t size, std::uint32_t expected_crc)
{
    if (size > kMaxWholeContentBytes) {
        return IoError::CapacityExceeded; // a size the directory states, not one that may be allocated blindly
    }
    zip_file_t* const file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), ZIP_FL_UNCHANGED);
    if (file == nullptr) {
        return IoError::IoFailure;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::size_t                done = 0;
    bool                       ok   = true;
    while (done < bytes.size()) {
        const zip_int64_t got = zip_fread(file, bytes.data() + done, bytes.size() - done);
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    if (ok && done == bytes.size()) {
        std::byte probe{};
        if (zip_fread(file, &probe, 1) < 0) {
            ok = false;
        }
    }
    zip_fclose(file);

    if (!ok || done != bytes.size()) {
        return IoError::IoFailure;
    }
    if (expected_crc != 0 && crc32Of(bytes) != expected_crc) {
        return IoError::IoFailure;
    }
    return bytes;
}

/**
 * @brief Maps a libzip error code onto this library's vocabulary.
 *
 * libzip says why an archive could not be opened or written; reporting every reason as InvalidData would hide a
 * permission problem behind a format problem, which is the one thing a caller has to be able to tell apart.
 *
 * @param code A ZIP_ER_* code.
 * @return The failure class; IoError::IoFailure for a code this layer does not name, which is true without being
 *         specific.
 */
inline IoError ioErrorOf(int code) noexcept
{
    switch (code) {
    case ZIP_ER_NOENT: return IoError::NotFound;
    case ZIP_ER_EXISTS: return IoError::AlreadyExists;
    case ZIP_ER_NOZIP:
    case ZIP_ER_INCONS:
    case ZIP_ER_CRC:
    case ZIP_ER_INVAL: return IoError::InvalidData;
    case ZIP_ER_OPNOTSUPP: return IoError::Unsupported;
    default: return IoError::IoFailure;
    }
}

/**
 * @brief Maps a libzip error object onto this library's vocabulary.
 *
 * The system half is read first: a code that says "the operating system refused" carries the errno that explains it,
 * and that is worth more than the generic class libzip wraps it in.
 *
 * @param error The error libzip reported.
 * @return The failure class.
 */
inline IoError ioErrorOf(const zip_error_t& error) noexcept
{
    if (zip_error_system_type(&error) == ZIP_ET_SYS) {
        switch (zip_error_code_system(&error)) {
        case EACCES:
        case EPERM: return IoError::PermissionDenied;
        case ENOENT: return IoError::NotFound;
        case ENOSPC: return IoError::CapacityExceeded;
        default: break;
        }
    }
    return ioErrorOf(zip_error_code_zip(&error));
}

/**
 * @brief Owns a zip_error_t for one scope, so no return path forgets zip_error_fini().
 *
 * libzip may attach a description to the error it writes, so the object has to be finished on every path - including
 * the ones that return a failure, which are exactly the paths a hand-written fini() forgets.
 */
class ZipError final
{
  public:
    /** @brief Initializes an empty error. */
    ZipError() noexcept
    {
        zip_error_init(&error_);
    }

    ~ZipError()
    {
        zip_error_fini(&error_);
    }

    ZipError(const ZipError&)            = delete;
    ZipError& operator=(const ZipError&) = delete;

    /**
     * @brief The error, for a libzip call that writes one.
     *
     * @return Pointer to the owned error.
     */
    [[nodiscard]] zip_error_t* get() noexcept
    {
        return &error_;
    }

    /**
     * @brief The error, for reading what libzip wrote.
     *
     * @return The owned error.
     */
    [[nodiscard]] const zip_error_t& ref() const noexcept
    {
        return error_;
    }

  private:
    zip_error_t error_{};
};

/**
 * @brief One entry of an open archive's directory, in the form the ZIP stores it.
 *
 * The name is the stored spelling, where a directory ends with '/', and a file
 * carries the checksum the archive recorded. This stays ZIP-private: callers see
 * the same entry through VfsEntryInfo, which carries the kind in its own words and
 * never a trailing '/'.
 */
struct StoredEntry {
    std::filesystem::path name;                  // Virtual path the stored name resolves to.
    std::string           stored;                // The name exactly as the archive holds it, trailing '/' included.
    std::uint64_t         size{ 0 };             // Uncompressed size in bytes; 0 for a directory.
    std::uint32_t         crc{ 0 };              // Checksum recorded by the archive; 0 for a directory.
    bool                  is_directory{ false }; // true when the stored name marks a directory.
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
 * @return IoError::Ok, IoError::InvalidData for an empty name, or IoError::InvalidPath when the
 *         name escapes or resolves to the virtual root.
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
        if (zip_stat_index(archive, i, ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC | ZIP_FL_ENC_RAW, &st) != 0 || st.name == nullptr) {
            return IoError::InvalidData;
        }

        StoredEntry       entry;
        const std::size_t length = std::strlen(st.name);
        const IoError     named  = fromStoredName(st.name, length, entry.name, entry.is_directory);
        if (named != IoError::Ok) {
            return IoError::InvalidData; // malformed or unsafe names make the archive unusable
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
    is_directory           = length != 0 && (bytes[length - 1] == '/' || bytes[length - 1] == '\\');
    const std::size_t kept = is_directory ? length - 1 : length;
    if (kept == 0) {
        return IoError::InvalidData; // an entry naming the root says nothing a tree can use
    }

    std::filesystem::path decoded;

    // These bytes are candidate UTF-8, not known UTF-8: the whole point of this function is that they may be text in a
    // legacy code page instead. So the question is put to vn::isValidUtf8() (core), which is fed the raw bytes rather
    // than a text view of them, and the hand-over is deliberately NOT String::fromUtf8(): that factory's contract is the
    // caller asserting "these bytes are UTF-8", which is exactly what cannot be asserted before the validator has
    // answered. Only once it has does the same byte range become the UTF-8 text a path is built from.
    if (isValidUtf8(std::string_view(bytes, kept))) {
        const std::u8string raw(reinterpret_cast<const char8_t*>(bytes), kept);
        decoded = std::filesystem::path(raw);
    }
    else {
        // A legacy name: the bytes are text in whatever code page the writer used, and the only
        // one this machine can stand behind is its own. The conversion reports bytes it cannot
        // spell by throwing, and the last resort keeps every byte as its own code point.
        try {
            decoded = std::filesystem::path(std::string(bytes, kept));
        }
        catch (const std::exception&) {
            decoded = std::filesystem::path(promoteBytesToText(bytes, kept));
        }
    }

    // ZIP specifies '/' as the directory separator, but archives produced on Windows sometimes
    // store the host spelling with '\\'. Normalize only at this archive boundary; VFS callers
    // still have to use canonical '/' paths, and dot-segment aliases are rejected.
    std::u8string normalized_text = decoded.generic_u8string();
    for (char8_t& ch : normalized_text) {
        if (ch == u8'\\') {
            ch = u8'/';
        }
    }

    std::filesystem::path normalized;
    const IoError         path_error = normalizeVfsPath(std::filesystem::path(normalized_text), normalized);
    if (path_error != IoError::Ok) {
        return path_error;
    }
    if (normalized.empty() || normalized.generic_u8string() != normalized_text) {
        return IoError::InvalidPath;
    }
    out = std::move(normalized);
    return IoError::Ok;
}

/**
 * @brief An entry found by name, with the kind the archive recorded for it.
 */
struct LocatedEntry {
    zip_int64_t index{ -1 };           // Position of the entry in the archive.
    bool        is_directory{ false }; // true when the stored name marks a directory.
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
 * while a VfsEntryInfo carries the kind. An entry naming the root is left out - the
 * root always exists, and reporting it would only repeat that.
 *
 * The result is ordered by the UTF-8 bytes of the full path, which is the order every listing in this module answers
 * in (Vfs::list): an archive's storage order is a write detail, not part of the tree.
 *
 * @param stored The entries as the archive stores them.
 * @return The entries as VfsEntryInfo, ordered by path.
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
        info.path = entry.name;
        info.kind = entry.is_directory ? VfsEntryKind::Directory : VfsEntryKind::File;
        if (info.kind != VfsEntryKind::Directory) {
            info.size = entry.size;
            info.crc  = entry.crc;
        }
        listed.push_back(std::move(info));
    }

    std::sort(listed.begin(), listed.end(), [](const VfsEntryInfo& left, const VfsEntryInfo& right) {
        return left.path.generic_u8string() < right.path.generic_u8string();
    });
    return listed;
}

} // namespace detail

VN_IO_NS_END
