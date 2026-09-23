#include <vine/io/Zip.hpp>

#include <algorithm>

#include <zlib.h>

#include <vine/io/ZipArchive.hpp>

V_IO_NS_BEGIN

bool Zip::compress(const void* data, std::size_t size, std::vector<unsigned char>& out)
{
    const uLongf bound = compressBound(static_cast<uLong>(size));
    out.resize(static_cast<std::size_t>(bound));
    uLongf dest_len = bound;
    if (::compress(out.data(), &dest_len, static_cast<const Bytef*>(data), static_cast<uLong>(size)) != Z_OK) {
        return false;
    }
    out.resize(static_cast<std::size_t>(dest_len));
    return true;
}

bool Zip::decompress(const void* data, std::size_t size, std::vector<unsigned char>& out)
{
    std::size_t capacity = std::max<std::size_t>(size * 2, 256);
    while (capacity <= (1u << 30)) {
        out.resize(capacity);
        uLongf      dest_len = static_cast<uLongf>(capacity);
        const int   rc       = ::uncompress(out.data(), &dest_len, static_cast<const Bytef*>(data), static_cast<uLong>(size));
        if (rc == Z_OK) {
            out.resize(static_cast<std::size_t>(dest_len));
            return true;
        }
        if (rc != Z_BUF_ERROR) {
            return false;
        }
        capacity *= 2;
    }
    return false;
}

bool Zip::compressDirectory(const std::filesystem::path& dir_path, const std::filesystem::path& zip_path)
{
    ZipArchive    archive;
    const IoError imported = archive.importDirectory(String{}, dir_path);
    return imported == IoError::Ok && archive.saveAs(zip_path) == IoError::Ok;
}

bool Zip::decompressFile(const std::filesystem::path& zip_path, const std::filesystem::path& dir_path)
{
    return ZipArchive::decompressFile(zip_path, dir_path);
}

V_IO_NS_END
