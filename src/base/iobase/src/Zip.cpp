#include <vine/io/Zip.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <system_error>

#include <zip.h>
#include <zlib.h>

#include <vine/io/ZipArchive.hpp>

#include "ZipInternal.hpp"

VN_IO_NS_BEGIN

namespace
{

/// Bytes one deflate/inflate step may produce or consume; small enough to stay a cache-sized buffer.
constexpr std::size_t kCodecChunkBytes = 64U * 1024U;

/**
 * @brief Reads one named entry out of an open archive, verifying it against the directory.
 *
 * The two readEntry() overloads differ only in how the archive is opened, so everything after that lives here: the
 * lookup, the directory check and the read-and-verify rule exist once, and the two entries cannot drift apart.
 *
 * @param archive The open archive.
 * @param name The virtual path to read.
 * @return The content, IoError::NotFound when the name is not there, IoError::IsADirectory when it marks a directory,
 *         or what the read reports.
 */
Result<std::vector<unsigned char>> readLocatedEntry(zip_t* archive, const std::filesystem::path& name)
{
    // The lookup first compares the bytes we hold with the bytes the archive stored, and falls back to comparing
    // decoded names, so a legacy name is found by what it says rather than by the spelling this side happens to use.
    detail::LocatedEntry located;
    if (!detail::locateEntry(archive, name, located)) {
        return IoError::NotFound;
    }
    if (located.is_directory) {
        return IoError::IsADirectory; // the trailing '/' is how a ZIP marks a directory
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, located.index, 0, &st) != 0) {
        return IoError::IoFailure;
    }
    return detail::readWholeEntry(archive, located.index, static_cast<std::uint64_t>(st.size), (st.valid & ZIP_STAT_CRC) != 0 ? static_cast<std::uint32_t>(st.crc) : 0);
}

} // namespace

Result<std::vector<unsigned char>> Zip::compress(std::span<const unsigned char> bytes)
{
    // Streamed rather than one compress() call: the codec takes a 32-bit length, so a single call would silently
    // compress only the first 4 GiB of a larger buffer on a platform where uLong is 32 bits.
    z_stream stream{};
    if (::deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return IoError::IoFailure;
    }

    std::array<unsigned char, kCodecChunkBytes> buffer{};
    std::vector<unsigned char>                  out;
    out.reserve(std::max<std::size_t>(bytes.size() / 2, 64));

    std::size_t offset = 0;
    while (true) {
        if (stream.avail_in == 0 && offset < bytes.size()) {
            const std::size_t step = std::min<std::size_t>(bytes.size() - offset, buffer.size());
            stream.next_in        = const_cast<Bytef*>(bytes.data() + offset); // zlib reads it, never writes it
            stream.avail_in       = static_cast<uInt>(step);
            offset += step;
        }

        stream.next_out  = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        const int rc     = ::deflate(&stream, offset == bytes.size() ? Z_FINISH : Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            ::deflateEnd(&stream);
            return IoError::IoFailure;
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(buffer.size() - stream.avail_out));
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc == Z_BUF_ERROR && stream.avail_in == 0 && offset == bytes.size()) {
            ::deflateEnd(&stream);
            return IoError::IoFailure; // nothing left to read and no progress: the stream never finished
        }
    }

    ::deflateEnd(&stream);
    return out;
}

Result<std::vector<unsigned char>> Zip::decompress(std::span<const unsigned char> bytes)
{
    // Streamed for the same reason compress() is, and it is what makes the size limit enforceable while the content
    // grows instead of after it has already been allocated.
    z_stream stream{};
    if (::inflateInit(&stream) != Z_OK) {
        return IoError::IoFailure;
    }

    std::array<unsigned char, kCodecChunkBytes> buffer{};
    std::vector<unsigned char>                  out;

    std::size_t offset = 0;
    while (true) {
        if (stream.avail_in == 0 && offset < bytes.size()) {
            const std::size_t step = std::min<std::size_t>(bytes.size() - offset, buffer.size());
            stream.next_in        = const_cast<Bytef*>(bytes.data() + offset); // zlib reads it, never writes it
            stream.avail_in       = static_cast<uInt>(step);
            offset += step;
        }

        stream.next_out  = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        const int         rc       = ::inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced > detail::kMaxWholeContentBytes - out.size()) {
            ::inflateEnd(&stream);
            return IoError::CapacityExceeded; // the bound on what one call may materialize
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));

        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            ::inflateEnd(&stream);
            return rc == Z_MEM_ERROR ? IoError::IoFailure : IoError::InvalidData; // not a zlib stream, or truncated
        }
        if (produced == 0 && stream.avail_in == 0 && offset == bytes.size()) {
            ::inflateEnd(&stream);
            return IoError::InvalidData; // the input ended before the stream did
        }
    }

    ::inflateEnd(&stream);
    return out;
}

IoError Zip::compressDirectory(const std::filesystem::path& dir_path, const std::filesystem::path& zip_path)
{
    ZipArchive    archive;
    const IoError imported = archive.addDirectory(std::filesystem::path{}, dir_path);
    if (imported != IoError::Ok) {
        return imported;
    }
    return archive.saveAs(zip_path);
}

IoError Zip::decompressFile(const std::filesystem::path& zip_path, const std::filesystem::path& dir_path)
{
    std::error_code ec;
    const auto      status = std::filesystem::status(zip_path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    const std::string path_utf8 = detail::toUtf8(zip_path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return detail::ioErrorOf(error); // "not an archive" is only one of the reasons libzip can give
    }
    if (!std::filesystem::create_directories(dir_path, ec) && ec) {
        zip_close(archive);
        return IoError::IoFailure;
    }

    IoError           result = IoError::Ok;
    const zip_int64_t count  = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, i, 0, &st) != 0 || st.name == nullptr) {
            result = IoError::InvalidData;
            break;
        }
        const std::string name = st.name;

        // Decoded the way the tree decodes it, so an entry the VFS can read can also be
        // written out; a name that is text in no encoding this host knows is reported
        // instead of being guessed at.
        bool                  is_directory = false;
        std::filesystem::path entry_name;
        const IoError         decoded = detail::fromStoredName(st.name, name.size(), entry_name, is_directory);
        if (decoded != IoError::Ok) {
            result = decoded == IoError::InvalidData ? IoError::InvalidPath : decoded;
            break;
        }

        const std::filesystem::path target = dir_path / entry_name;
        if (is_directory) {
            if (!std::filesystem::create_directories(target, ec) && ec) {
                result = IoError::IoFailure;
                break;
            }
            continue;
        }

        zip_file_t* const file = zip_fopen_index(archive, i, 0);
        if (file == nullptr) {
            result = IoError::IoFailure;
            break;
        }
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            zip_fclose(file);
            result = IoError::IoFailure;
            break;
        }

        std::array<char, 16 * 1024> chunk{};
        bool                        ok = true;
        while (ok) {
            const zip_int64_t got = zip_fread(file, chunk.data(), chunk.size());
            if (got < 0) {
                ok = false;
                break;
            }
            if (got == 0) {
                break;
            }
            out.write(chunk.data(), static_cast<std::streamsize>(got));
            if (!out) {
                ok = false;
                break;
            }
        }
        zip_fclose(file);
        if (!ok) {
            result = IoError::IoFailure;
            break;
        }
    }
    zip_close(archive);
    return result;
}

Result<std::vector<VfsEntryInfo>> Zip::entries(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return detail::ioErrorOf(error);
    }
    const auto result = detail::readEntries(archive);
    zip_close(archive);
    if (!result) {
        return result.error();
    }
    return detail::toEntryInfos(result.value());
}

Result<std::vector<VfsEntryInfo>> Zip::entries(std::span<const unsigned char> bytes)
{
    if (bytes.empty()) {
        return IoError::InvalidData;
    }
    detail::ZipError error;
    zip_source_t* const source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, error.get());
    if (source == nullptr) {
        return detail::ioErrorOf(error.ref());
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, error.get());
    if (archive == nullptr) {
        const IoError why = detail::ioErrorOf(error.ref());
        zip_source_free(source);
        return why;
    }
    const auto result = detail::readEntries(archive);
    zip_close(archive);
    if (!result) {
        return result.error();
    }
    return detail::toEntryInfos(result.value());
}

Result<std::vector<unsigned char>> Zip::readEntry(const std::filesystem::path& path, const std::filesystem::path& name)
{
    if (name.empty()) {
        return IoError::InvalidPath;
    }

    std::error_code ec;
    const auto      status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return detail::ioErrorOf(error);
    }
    auto content = readLocatedEntry(archive, name);
    zip_close(archive);
    return content;
}

Result<std::vector<unsigned char>> Zip::readEntry(std::span<const unsigned char> bytes, const std::filesystem::path& name)
{
    if (name.empty()) {
        return IoError::InvalidPath;
    }
    if (bytes.empty()) {
        return IoError::InvalidData;
    }
    detail::ZipError error;
    zip_source_t* const source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, error.get());
    if (source == nullptr) {
        return detail::ioErrorOf(error.ref());
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, error.get());
    if (archive == nullptr) {
        const IoError why = detail::ioErrorOf(error.ref());
        zip_source_free(source);
        return why;
    }

    auto content = readLocatedEntry(archive, name);
    zip_close(archive);
    return content;
}

VN_IO_NS_END
