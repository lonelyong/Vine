#pragma once
#include "io_global.hpp"

#include <filesystem>
#include <span>
#include <vector>

#include <vine/io/Vfs.hpp>
#include <vine/String.hpp>

V_IO_NS_BEGIN

/**
 * @brief One-shot compression and ZIP archive operations.
 *
 * The base layer over the codecs: compress/decompress work on byte ranges with
 * the zlib format, and the archive calls handle a whole file in one go - pack a
 * directory tree, extract an archive, list its entries, read one entry. Nothing
 * here keeps state; an archive that is opened and then queried repeatedly goes
 * through ZipArchive.
 *
 * Every call reports through IoError / Result, like the rest of the library: the
 * bytes go in as a span, and what comes back is either bytes or the failure.
 */
class V_IOBASE_API Zip
{
  public:
    Zip() = delete;

    /**
     * @brief Compresses a byte range with the zlib format.
     *
     * @param bytes The bytes to compress; may be empty.
     * @return The compressed bytes, or IoError::IoFailure when the codec fails.
     */
    [[nodiscard]] static Result<std::vector<unsigned char>> compress(std::span<const unsigned char> bytes);

    /**
     * @brief Decompresses a zlib-format byte range.
     *
     * The destination is grown as needed while decompressing.
     *
     * @param bytes The compressed bytes; may be empty.
     * @return The decompressed bytes, or IoError::InvalidData when the bytes are
     *         not a zlib stream, IoError::CapacityExceeded when the result would
     *         outgrow the size limit, IoError::IoFailure when the codec fails.
     */
    [[nodiscard]] static Result<std::vector<unsigned char>> decompress(std::span<const unsigned char> bytes);

    /**
     * @brief Compresses a directory tree into a ZIP file.
     *
     * Entry names are relative to dir_path and use '/' separators.
     *
     * @param dir_path Directory to compress.
     * @param zip_path Output .zip file path.
     * @return IoError::Ok on success, IoError::NotFound when dir_path is not a
     *         directory, or the failure the write reports.
     */
    [[nodiscard]] static IoError compressDirectory(const std::filesystem::path& dir_path,
                                                   const std::filesystem::path& zip_path);

    /**
     * @brief Decompresses a ZIP file into a directory.
     *
     * Entry names that could escape the destination directory are rejected.
     *
     * @param zip_path Input .zip file path.
     * @param dir_path Destination directory, created if missing.
     * @return IoError::Ok on success, IoError::NotFound when zip_path does not
     *         exist, IoError::InvalidData when it is not an archive,
     *         IoError::InvalidPath when an entry name would escape dir_path,
     *         IoError::IoFailure when reading or writing fails.
     */
    [[nodiscard]] static IoError decompressFile(const std::filesystem::path& zip_path,
                                                const std::filesystem::path& dir_path);

    /**
     * @brief Lists the entries of a ZIP file in the tree vocabulary.
     *
     * An entry is reported the way every other backend reports one: its full
     * virtual path, its kind and its size - so a directory never carries the
     * trailing '/' the archive stores it with, and its size is 0. Only the
     * archive directory is read, so the entries themselves are not decompressed
     * - this is what keeps listing a large package cheap.
     *
     * @param path Input .zip file path.
     * @return The entries, IoError::NotFound when the file does not exist, or IoError::InvalidData when it is not an archive.
     */
    [[nodiscard]] static Result<std::vector<VfsEntryInfo>> entries(const std::filesystem::path& path);

    /**
     * @brief Lists the entries of an in-memory ZIP in the tree vocabulary.
     *
     * @param bytes ZIP bytes, which must stay readable for the call.
     * @return The entries, or IoError::InvalidData when the bytes are not an archive.
     */
    [[nodiscard]] static Result<std::vector<VfsEntryInfo>> entries(std::span<const unsigned char> bytes);

    /**
     * @brief Reads one entry of a ZIP file.
     *
     * The content is checked against the checksum the archive recorded, the way
     * an opened archive checks it - libzip itself does not.
     *
     * @param path Input .zip file path.
     * @param name Entry name to read; a trailing '/' marks a directory.
     * @return The entry bytes, IoError::NotFound when the file or the entry is
     *         missing, IoError::InvalidData when the file is not an archive,
     *         IoError::InvalidPath when name is empty, IoError::IsADirectory when
     *         name marks a directory, IoError::IoFailure when the entry cannot be
     *         read or its content does not match the recorded checksum.
     */
    [[nodiscard]] static Result<std::vector<unsigned char>> readEntry(const std::filesystem::path& path,
                                                                     const String& name);

    /**
     * @brief Reads one entry of an in-memory ZIP.
     *
     * The content is checked against the checksum the archive recorded, the way
     * an opened archive checks it - libzip itself does not.
     *
     * @param bytes ZIP bytes, which must stay readable for the call.
     * @param name Entry name to read; a trailing '/' marks a directory.
     * @return The entry bytes, IoError::NotFound when the entry is missing,
     *         IoError::InvalidData when the bytes are not an archive,
     *         IoError::InvalidPath when name is empty, IoError::IsADirectory when
     *         name marks a directory, IoError::IoFailure when the entry cannot be
     *         read or its content does not match the recorded checksum.
     */
    [[nodiscard]] static Result<std::vector<unsigned char>> readEntry(std::span<const unsigned char> bytes,
                                                                     const String& name);
};

V_IO_NS_END
