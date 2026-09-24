#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include <vine/io/Vfs.hpp>
#include <vine/io/io_global.hpp>
#include <vine/String.hpp>

VN_IO_NS_BEGIN

/**
 * @brief Real-directory backed VFS (debug backend).
 *
 * Maps the virtual tree straight onto a real directory: a write becomes a real
 * file, a read reads one, and saveAs() only confirms the target because every
 * write is already immediate. Useful for inspecting what would go into a ZIP
 * package, and for loading from an unpacked tree.
 */
class VN_IOBASE_API DirectoryVfs : public Vfs
{
  public:
    /**
     * @brief Constructs a VFS rooted at an existing directory.
     *
     * @param root The real directory to map onto.
     */
    explicit DirectoryVfs(const std::filesystem::path& root);

    /**
     * @brief Destroys the VFS.
     */
    ~DirectoryVfs() override;

    /**
     * @brief Opens a real directory as a VFS.
     *
     * @param dir The directory to map onto (must exist).
     * @return The VFS, or null when the directory does not exist.
     */
    static std::unique_ptr<DirectoryVfs> openDirectory(const std::filesystem::path& dir);

    // Vfs
    using Vfs::addFile; // the overrides below would otherwise hide the base's content-source overloads

    /**
     * @brief An on-disk directory accepts every write, so this is false.
     *
     * @return false always.
     */
    [[nodiscard]] bool isReadOnly() const noexcept override;

    /**
     * @brief Reports type and size of a real path under the root.
     *
     * @param path The virtual path to query; empty denotes the root.
     * @return The information, or IoError::NotFound when nothing is there,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] Result<VfsEntryInfo> stat(const std::filesystem::path& path) const override;

    /**
     * @brief Lists the direct children of a real directory.
     *
     * @param dir The virtual directory to list; empty denotes the root.
     * @return The children, or IoError::NotFound when dir does not exist,
     *         IoError::NotADirectory when dir names a file,
     *         IoError::InvalidPath when dir is not a valid virtual path.
     */
    [[nodiscard]] Result<std::vector<VfsEntryInfo>> list(const std::filesystem::path& dir) const override;

    /**
     * @brief Reads a real file as a whole.
     *
     * @param path The virtual file path.
     * @return The file bytes, or IoError::NotFound when there is no such file,
     *         IoError::IsADirectory when path names a directory,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> read(const std::filesystem::path& path) const override;

    /**
     * @brief Adds a whole real file, creating missing parents.
     *
     * @param path The virtual file path.
     * @param bytes The bytes to store; may be empty.
     * @return IoError::Ok on success, IoError::IsADirectory when the name is
     *         taken by a directory, IoError::InvalidPath when path is not a
     *         valid virtual path, IoError::IoFailure when writing fails.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes) override;

    /**
     * @brief Creates a real directory under the root.
     *
     * @param path The directory to create; the root always exists.
     * @return IoError::Ok on success, IoError::AlreadyExists when the name is
     *         taken, IoError::NotFound when the parent is missing,
     *         IoError::NotADirectory when a file blocks the way,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError createDirectory(const std::filesystem::path& path) override;

    /**
     * @brief Creates a real directory together with every missing parent.
     *
     * @param path The directory to create; an existing directory is not an error.
     * @return IoError::Ok on success, IoError::AlreadyExists when a file owns
     *         the name, IoError::NotADirectory when a file blocks the way,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError createDirectories(const std::filesystem::path& path) override;

    /**
     * @brief Renames or moves a real file or directory under the root.
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
     *         IoError::InvalidPath when either path is invalid or to lies below from.
     */
    [[nodiscard]] IoError rename(const std::filesystem::path& from, const std::filesystem::path& to) override;

    /**
     * @brief Removes a real file or an empty real directory.
     *
     * @param path The file or empty directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::NotEmpty when the directory still holds entries,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError remove(const std::filesystem::path& path) override;

    /**
     * @brief Removes a real file or a whole real subtree.
     *
     * @param path The file or directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError removeAll(const std::filesystem::path& path) override;

    /**
     * @brief Adds a real file to the tree by copying it.
     *
     * The copy is immediate and streamed, so the source may change or vanish
     * afterwards.
     *
     * @param path The virtual file path.
     * @param real_path The physical file to copy in.
     * @return IoError::Ok on success, IoError::NotFound when real_path cannot be
     *         reached, IoError::IsADirectory when the name is taken by a
     *         directory, IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path, const std::filesystem::path& real_path) override;

    /**
     * @brief Confirms the tree is persisted.
     *
     * Writes are immediate, so an existing directory target means there is
     * nothing left to do.
     *
     * @param path The directory to confirm as the target.
     * @return IoError::Ok when path is a directory, IoError::Unsupported
     *         otherwise - a directory backend produces no archive.
     */
    [[nodiscard]] IoError commit() override;
    [[nodiscard]] IoError saveAs(const std::filesystem::path& path) const override;

    /**
     * @brief Not supported: a directory backend produces no archive.
     *
     * @param out Unused.
     * @return IoError::Unsupported always.
     */
    [[nodiscard]] IoError saveAs(std::ostream& out) const override;

    /**
     * @brief Not supported: a directory backend produces no archive.
     *
     * @return IoError::Unsupported always.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> toBytes() const override;

  private:
    /**
     * @brief Validates a virtual path and maps it to a real path under the root.
     *
     * @param vfs_path The virtual path; empty denotes the root.
     * @param normalized Receives the normalized virtual path; untouched on failure.
     * @param out Receives the mapped real path; untouched on failure.
     * @return IoError::Ok, or IoError::InvalidPath for an invalid virtual path.
     */
    [[nodiscard]] IoError resolve(const std::filesystem::path& vfs_path, std::filesystem::path& normalized,
                                  std::filesystem::path& out) const;

    std::filesystem::path root_;
};

VN_IO_NS_END
