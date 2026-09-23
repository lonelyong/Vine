#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <span>
#include <vector>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>
#include <vine/String.hpp>

V_IO_NS_BEGIN

/**
 * @brief What stat() and list() report about one virtual path.
 */
struct V_IOBASE_API FileInfo
{
    String        path;                  ///< Full normalized virtual path; the empty string is the root.
    bool          is_directory{ false }; ///< true when the path names a directory.
    std::uint64_t size{ 0 };             ///< Size in bytes; always 0 for a directory.

    /**
     * @brief The last path segment, i.e. the entry's own name.
     *
     * @return The name, or an empty string for the virtual root.
     */
    [[nodiscard]] String name() const;
};

/**
 * @brief A virtual file tree addressed by '/' separated paths.
 *
 * The contract is the same for every backend:
 *
 *   * Paths are '/'-separated and relative to the virtual root, which the
 *     empty string denotes. A path can never escape a backend's own root.
 *   * Results travel back as IoError / Result: there are no out-parameters,
 *     and no failure is reduced to a bare false.
 *   * Directories are real even where the storage is not: a directory implied
 *     by a path must be visible, and createDirectories() must make an empty
 *     one observable.
 *   * The primitives are virtual; the conveniences at the bottom are derived
 *     from them once, so a backend implements only what it must.
 *
 * See ZipArchive and DirectoryVfs for the backends.
 */
class V_IOBASE_API Vfs
{
  public:
    /**
     * @brief Destroys the virtual file system.
     */
    virtual ~Vfs();

    /**
     * @brief Reports whether the backend refuses every write.
     *
     * Every mutating call on a read-only backend fails with IoError::ReadOnly
     * before anything is touched.
     *
     * @return true when writes, renames and directory changes are refused.
     */
    [[nodiscard]] virtual bool isReadOnly() const noexcept = 0;

    /**
     * @brief Reports type and size of a virtual path.
     *
     * @param path The virtual path to query; empty denotes the root.
     * @return The information, or IoError::NotFound when nothing is there,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual Result<FileInfo> stat(const String& path) const = 0;

    /**
     * @brief Lists the direct children of a directory.
     *
     * Each child carries its full normalized path and its own name, so results
     * can be fed straight back into stat() or read().
     *
     * @param dir The virtual directory to list; empty denotes the root.
     * @return The children, or IoError::NotFound when dir does not exist,
     *         IoError::NotADirectory when dir names a file,
     *         IoError::InvalidPath when dir is not a valid virtual path.
     */
    [[nodiscard]] virtual Result<std::vector<FileInfo>> list(const String& dir) const = 0;

    /**
     * @brief Reads a whole virtual file.
     *
     * @param path The virtual file path.
     * @return The file bytes, or IoError::NotFound when there is no such file,
     *         IoError::IsADirectory when path names a directory,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual Result<std::vector<unsigned char>> read(const String& path) const = 0;

    /**
     * @brief Writes a whole virtual file.
     *
     * Parent directories are implied, and an existing file is replaced - but a
     * directory, explicit or implied, is never replaced by a file.
     *
     * @param path The virtual file path.
     * @param bytes The bytes to store; may be empty.
     * @return IoError::Ok on success, IoError::IsADirectory when the name is
     *         taken by a directory, IoError::ReadOnly when the backend refuses
     *         writes, IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError write(const String& path, std::span<const unsigned char> bytes) = 0;

    /**
     * @brief Creates a virtual directory.
     *
     * The parent must already exist; use createDirectories() for a whole chain.
     *
     * @param path The directory to create; the root always exists.
     * @return IoError::Ok on success, IoError::AlreadyExists when the name is
     *         taken, IoError::NotFound when the parent is missing,
     *         IoError::NotADirectory when a file blocks the way,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError createDirectory(const String& path) = 0;

    /**
     * @brief Creates a virtual directory together with every missing parent.
     *
     * @param path The directory to create; an existing directory is not an error.
     * @return IoError::Ok on success, IoError::NotADirectory when a file blocks
     *         the way, IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError createDirectories(const String& path) = 0;

    /**
     * @brief Renames or moves a file or a whole subtree.
     *
     * An existing target is never overwritten, and a directory cannot be moved
     * into its own subtree.
     *
     * @param from The existing path to move; the root cannot be moved.
     * @param to The target path; the root cannot be a target.
     * @return IoError::Ok on success (also when from and to name the same path),
     *         IoError::NotFound when from is missing or the target's parent is,
     *         IoError::AlreadyExists when to is taken,
     *         IoError::NotADirectory when a file blocks the target's parent,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when either path is invalid or to lies below from.
     */
    [[nodiscard]] virtual IoError rename(const String& from, const String& to) = 0;

    /**
     * @brief Removes a file or an empty directory.
     *
     * @param path The file or empty directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::NotEmpty when the directory still holds entries,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError remove(const String& path) = 0;

    /**
     * @brief Removes a file or a whole subtree.
     *
     * @param path The file or directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError removeAll(const String& path) = 0;

    /**
     * @brief Brings a whole real directory into the tree.
     *
     * Every regular file below dir becomes a virtual file, its relative path
     * appended to prefix. Empty subdirectories are kept as directories. Files are
     * brought in one by one through importFile(), so a backend keeps whatever
     * laziness importFile() has.
     *
     * @param prefix The virtual directory to import into; empty means the root.
     * @param dir The physical directory to walk; it is not removed.
     * @return IoError::Ok on success, IoError::NotFound when dir is not a directory,
     *         or the first failure an import reports.
     */
    [[nodiscard]] IoError importDirectory(const String& prefix, const std::filesystem::path& dir);

    /**
     * @brief Brings a real file into the tree.
     *
     * The content of real_path becomes readable at path. A backend may read the
     * real file eagerly, or defer the read to saveAs() - so it has to stay
     * readable until the tree is persisted.
     *
     * @param path The virtual file path.
     * @param real_path The physical file to bring in.
     * @return IoError::Ok on success, IoError::NotFound when real_path cannot be
     *         reached, IoError::IsADirectory when the name is taken by a
     *         directory, IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError importFile(const String& path, const std::filesystem::path& real_path) = 0;

    /**
     * @brief Writes the changes back to the target this tree was opened on.
     *
     * A backend whose writes already reach their storage reports Ok and does
     * nothing more; one that was not opened on a target reports Unsupported.
     *
     * @return IoError::Ok on success, IoError::Unsupported when there is no target
     *         to write back to, IoError::IoFailure when writing fails.
     */
    [[nodiscard]] virtual IoError commit() = 0;

    /**
     * @brief Persists the tree to a file.
     *
     * @param path The target file.
     * @return IoError::Ok on success, IoError::Unsupported when the backend
     *         cannot produce a file, IoError::IoFailure when writing fails.
     */
    [[nodiscard]] virtual IoError saveAs(const std::filesystem::path& path) const = 0;

    /**
     * @brief Persists the tree into an output stream.
     *
     * @param out Target output stream.
     * @return IoError::Ok on success, IoError::Unsupported when the backend
     *         cannot produce a stream, IoError::IoFailure when writing fails.
     */
    [[nodiscard]] virtual IoError saveAs(std::ostream& out) const = 0;

    /**
     * @brief Serializes the tree into memory.
     *
     * Holds the whole result in memory, so it is meant for trees of moderate
     * size; a large one goes through saveAs(path).
     *
     * @return The serialized bytes, or IoError::Unsupported when the backend
     *         cannot produce them, IoError::IoFailure when it fails.
     */
    [[nodiscard]] virtual Result<std::vector<unsigned char>> toBytes() const = 0;

    /**
     * @brief Checks whether a virtual file or directory exists.
     *
     * The virtual root always exists.
     *
     * @param path The virtual path to check.
     * @return true when path names an existing file, directory or the root.
     */
    [[nodiscard]] bool exists(const String& path) const;

    /**
     * @brief Checks whether a virtual path names a file (not a directory).
     *
     * @param path The virtual path to check.
     * @return true when path names an existing file.
     */
    [[nodiscard]] bool isFile(const String& path) const;

    /**
     * @brief Checks whether a virtual path names a directory.
     *
     * The virtual root is always a directory.
     *
     * @param path The virtual path to check.
     * @return true when path names an existing directory or the root.
     */
    [[nodiscard]] bool isDirectory(const String& path) const;
};

V_IO_NS_END
