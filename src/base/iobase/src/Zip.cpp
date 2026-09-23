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

V_IO_NS_BEGIN

namespace
{

/**
 * @brief Returns true when an entry name cannot escape the extraction root.
 */
bool isSafeEntry(const std::string& name)
{
    return !name.empty() && name.front() != '/' && name.find("..") == std::string::npos;
}

/// The largest decompressed size one call will produce.
constexpr std::size_t kMaxDecompressedBytes = 1u << 30;

/**
 * @brief Computes the CRC-32 of a buffer.
 *
 * The codec takes a 32-bit length, so the buffer is walked in chunks.
 *
 * @param bytes The bytes to checksum.
 * @return The checksum; 0 for an empty buffer.
 */
std::uint32_t crc32Of(std::span<const unsigned char> bytes)
{
    std::uint32_t crc    = 0;
    std::size_t   offset = 0;
    while (offset < bytes.size()) {
        const std::size_t step = std::min<std::size_t>(bytes.size() - offset, 1u << 30);
        crc = static_cast<std::uint32_t>(::crc32(crc, bytes.data() + offset, static_cast<uInt>(step)));
        offset += step;
    }
    return crc;
}

} // namespace

Result<std::vector<unsigned char>> Zip::compress(std::span<const unsigned char> bytes)
{
    const uLongf               bound = compressBound(static_cast<uLong>(bytes.size()));
    std::vector<unsigned char> out(static_cast<std::size_t>(bound));
    uLongf                     dest_len = bound;
    if (::compress(out.data(), &dest_len, bytes.data(), static_cast<uLong>(bytes.size())) != Z_OK) {
        return IoError::IoFailure;
    }
    out.resize(static_cast<std::size_t>(dest_len));
    return out;
}

Result<std::vector<unsigned char>> Zip::decompress(std::span<const unsigned char> bytes)
{
    // The codec cannot say "not enough room" before trying, so the destination is
    // retried at growing sizes until it fits - or until the limit says the stream
    // will not fit anywhere sensible.
    std::size_t capacity = std::max<std::size_t>(bytes.size() * 2, 256);
    while (capacity <= kMaxDecompressedBytes) {
        std::vector<unsigned char> out(capacity);
        uLongf                     dest_len = static_cast<uLongf>(capacity);
        const int rc = ::uncompress(out.data(), &dest_len, bytes.data(), static_cast<uLong>(bytes.size()));
        if (rc == Z_OK) {
            out.resize(static_cast<std::size_t>(dest_len));
            return out;
        }
        if (rc == Z_MEM_ERROR) {
            return IoError::IoFailure;
        }
        if (rc != Z_BUF_ERROR) {
            return IoError::InvalidData; // not a zlib stream
        }
        capacity *= 2;
    }
    return IoError::CapacityExceeded;
}

IoError Zip::compressDirectory(const std::filesystem::path& dir_path, const std::filesystem::path& zip_path)
{
    ZipArchive    archive;
    const IoError imported = archive.addDirectory(String{}, dir_path);
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
        return IoError::InvalidData;
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
        if (!isSafeEntry(name)) {
            result = IoError::InvalidPath; // an entry name that would escape the destination
            break;
        }

        const std::filesystem::path target =
            dir_path / std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(st.name)));
        if (name.back() == '/') {
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
        return IoError::InvalidData;
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
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* const source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        zip_source_free(source);
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    const auto result = detail::readEntries(archive);
    zip_close(archive);
    zip_error_fini(&error);
    if (!result) {
        return result.error();
    }
    return detail::toEntryInfos(result.value());
}

Result<std::vector<unsigned char>> Zip::readEntry(const std::filesystem::path& path, const String& name)
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
    const auto*       name_utf8 = reinterpret_cast<const char*>(name.data());
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return IoError::InvalidData;
    }
    const zip_int64_t index = zip_name_locate(archive, name_utf8, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        return IoError::NotFound;
    }
    if (name.as_std_u8str().back() == u8'/') {
        zip_close(archive);
        return IoError::IsADirectory; // the trailing '/' is how a ZIP marks a directory
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, index, 0, &st) != 0) {
        zip_close(archive);
        return IoError::IoFailure;
    }
    zip_file_t* const file = zip_fopen_index(archive, index, 0);
    if (file == nullptr) {
        zip_close(archive);
        return IoError::IoFailure;
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(st.size));
    zip_uint64_t               total = 0;
    while (total < st.size) {
        const zip_int64_t got = zip_fread(file, out.data() + total, st.size - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<zip_uint64_t>(got);
    }
    zip_fclose(file);
    zip_close(archive);
    if (total != st.size) {
        return IoError::IoFailure;
    }
    // libzip does not look at the checksum while it reads, so the content is
    // checked here against what the archive directory recorded - the same check
    // the opened archive performs.
    if ((st.valid & ZIP_STAT_CRC) != 0 && st.crc != 0 && crc32Of(out) != static_cast<std::uint32_t>(st.crc)) {
        return IoError::IoFailure;
    }
    return out;
}

Result<std::vector<unsigned char>> Zip::readEntry(std::span<const unsigned char> bytes, const String& name)
{
    if (name.empty()) {
        return IoError::InvalidPath;
    }
    if (bytes.empty()) {
        return IoError::InvalidData;
    }
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* const source = zip_source_buffer_create(bytes.data(), bytes.size(), 0, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        zip_source_free(source);
        zip_error_fini(&error);
        return IoError::InvalidData;
    }

    const auto*       name_utf8 = reinterpret_cast<const char*>(name.data());
    const zip_int64_t index     = zip_name_locate(archive, name_utf8, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        zip_error_fini(&error);
        return IoError::NotFound;
    }
    if (name.as_std_u8str().back() == u8'/') {
        zip_close(archive);
        zip_error_fini(&error);
        return IoError::IsADirectory; // the trailing '/' is how a ZIP marks a directory
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, index, 0, &st) != 0) {
        zip_close(archive);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }
    zip_file_t* const file = zip_fopen_index(archive, index, 0);
    if (file == nullptr) {
        zip_close(archive);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(st.size));
    zip_uint64_t               total = 0;
    while (total < st.size) {
        const zip_int64_t got = zip_fread(file, out.data() + total, st.size - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<zip_uint64_t>(got);
    }
    zip_fclose(file);
    zip_close(archive);
    zip_error_fini(&error);
    if (total != st.size) {
        return IoError::IoFailure;
    }
    // libzip does not look at the checksum while it reads, so the content is
    // checked here against what the archive directory recorded - the same check
    // the opened archive performs.
    if ((st.valid & ZIP_STAT_CRC) != 0 && st.crc != 0 && crc32Of(out) != static_cast<std::uint32_t>(st.crc)) {
        return IoError::IoFailure;
    }
    return out;
}

V_IO_NS_END
