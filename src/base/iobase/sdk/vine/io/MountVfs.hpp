#pragma once

#include <filesystem>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include <vine/io/Vfs.hpp>
#include <vine/io/io_global.hpp>

VN_IO_NS_BEGIN

/**
 * @brief A tree assembled from other backends, each serving one prefix (§10).
 *
 * mount() attaches a backend to a virtual prefix: everything at or below the
 * prefix is served by that backend, addressed relative to it. Mounts at the
 * same prefix stack into an overlay ordered by priority (higher first).
 *
 * Routing rules, fixed by design and by the decisions recorded in §15:
 * - Longest prefix wins, with no fallback: a path under a mount at "data/a" is
 *   answered by that mount even when it cannot resolve it, never by a mount at
 *   "data".
 * - Reads (stat, read, openRead) answer from the first mount of the group that
 *   has the path; entries are reported with their full virtual path.
 * - list() merges every mount of the group, deduplicating by name in priority
 *   order, and always synthesizes the directories a deeper mount implies.
 * - Writes go where the next read will look: a path that already exists is
 *   written in the mount that holds it - when that mount refuses writes the
 *   operation fails with IoError::ReadOnly and does NOT fall through to
 *   another backend. A path that exists nowhere goes to the first writable
 *   mount of the group, so patching a read-only package is done by copying the
 *   file into the writable mount first.
 * - rename() stays native inside one backend. Across backends it copies the
 *   content (one entry at a time, held in memory for the copy) and deletes the
 *   source afterwards - a file only: a directory is refused with
 *   IoError::Unsupported, and a failed copy leaves the source in place.
 * - commit() fan-outs to every writable mount (each backend once). saveAs()
 *   and toBytes() are Unsupported: a mounted tree is not one package, so its
 *   writable mounts are persisted themselves.
 *
 * The tree is read-only exactly when no mount accepts writes; a tree without
 * mounts is read-only and resolves nothing. A MountVfs can itself be mounted,
 * and mounting keeps the backend alive through a shared handle.
 */
class VN_IOBASE_API MountVfs : public Vfs
{
  public:
    /**
     * @brief Constructs an empty mounted tree.
     */
    MountVfs();

    /**
     * @brief Destroys the tree, releasing its mounts.
     */
    ~MountVfs() override;

    /**
     * @brief Mounts a backend under a virtual prefix.
     *
     * The prefix is normalized like any virtual path (the empty path is the
     * virtual root). Mounting twice at the same prefix, or mounting a backend
     * that is already mounted, is allowed and stacks an overlay.
     *
     * @param prefix   The virtual directory the backend serves.
     * @param backend  The backend to mount; never null, and kept alive by the
     *                 mount.
     * @param priority Higher wins inside one prefix: reads answer from the
     *                 highest-priority mount that has the path, and a new path
     *                 is written to the highest-priority writable one. Equal
     *                 priorities are ordered by mount order.
     * @param read_only Refuses every write through this mount even when the
     *                 backend itself is writable.
     * @return IoError::Ok, IoError::InvalidPath when prefix is not a valid
     *         virtual path, or IoError::InvalidData when backend is null.
     */
    [[nodiscard]] IoError mount(const std::filesystem::path& prefix, std::shared_ptr<Vfs> backend, int priority = 0,
                                bool read_only = false);

    // Vfs
    using Vfs::read;    // the override below would otherwise hide the base's sink-push overload
    using Vfs::addFile; // the overrides below would otherwise hide the base's content-source overloads

    /**
     * @brief Reports whether no mount accepts writes.
     *
     * @return true when every mount is read-only or backed by a read-only
     *         backend, or when nothing is mounted.
     */
    [[nodiscard]] bool isReadOnly() const noexcept override;

    /**
     * @brief Reports type and size of one path.
     *
     * Answers from the first mount that has the path; a directory implied by a
     * deeper mount exists even when no backend holds it.
     *
     * @param path The virtual path; empty denotes the root.
     * @return The entry, IoError::InvalidPath for a malformed path, or
     *         IoError::NotFound when no mount resolves it.
     */
    [[nodiscard]] Result<VfsEntryInfo> stat(const std::filesystem::path& path) const override;

    /**
     * @brief Lists the children of one directory.
     *
     * Merges every mount of the resolved group; a name claimed by more than
     * one mount is reported by the highest-priority one, and the directories
     * implied by deeper mounts are always present.
     *
     * @param dir The directory; empty denotes the root.
     * @return The children (full virtual paths), IoError::InvalidPath for a
     *         malformed path, IoError::NotFound when the directory does not
     *         exist, or IoError::NotADirectory when the path is a file.
     */
    [[nodiscard]] Result<std::vector<VfsEntryInfo>> list(const std::filesystem::path& dir) const override;

    /**
     * @brief Reads a whole entry.
     *
     * @param path The entry to read.
     * @return The bytes, IoError::InvalidPath, IoError::NotFound, or a failure
     *         reported by the owning backend.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> read(const std::filesystem::path& path) const override;

    /**
     * @brief Opens one entry for streaming.
     *
     * Forwards to the mount that holds the entry, so the stream type and its
     * error reporting are the backend's own.
     *
     * @param path The entry to open.
     * @return The stream, IoError::InvalidPath, IoError::NotFound, or a
     *         failure reported by the owning backend.
     */
    [[nodiscard]] Result<std::unique_ptr<VfsReadStream>> openRead(const std::filesystem::path& path) const override;

    /**
     * @brief Adds or replaces a file from a memory block.
     *
     * @param path The virtual path.
     * @param bytes The content.
     * @return IoError::Ok, IoError::InvalidPath, IoError::ReadOnly when the
     *         owning mount (or, for a new path, every mount of the group)
     *         refuses writes, or a failure reported by the target backend.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes) override;

    /**
     * @brief Adds or replaces a file by importing a real file.
     *
     * @param path The virtual path.
     * @param real_path The file to import.
     * @return IoError::Ok, IoError::InvalidPath, IoError::ReadOnly, or a
     *         failure reported by the target backend.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path,
                                  const std::filesystem::path& real_path) override;

    /**
     * @brief Adds or replaces a file from scattered buffers.
     *
     * The target backend decides when the pieces are pulled, so a deferring
     * backend keeps deferring.
     *
     * @param path The virtual path.
     * @param fragments The pieces, in order.
     * @return IoError::Ok, IoError::InvalidPath, IoError::InvalidData for a
     *         dataless piece, IoError::ReadOnly, or a failure reported by the
     *         target backend.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path, std::span<const Fragment> fragments) override;

    /**
     * @brief Adds or replaces a file fed by a content source.
     *
     * @param path The virtual path.
     * @param source The content source; never null, and pulled by the target
     *               backend at its own time.
     * @return IoError::Ok, IoError::InvalidPath, IoError::InvalidData for a
     *         null source, IoError::ReadOnly, or a failure reported by the
     *         target backend.
     */
    [[nodiscard]] IoError addFile(const std::filesystem::path& path, std::shared_ptr<DataSource> source) override;

    /**
     * @brief Creates one directory.
     *
     * @param path The directory to create; its parent must exist in the target
     *             backend.
     * @return IoError::Ok, IoError::InvalidPath, IoError::ReadOnly, or a
     *         failure reported by the target backend.
     */
    [[nodiscard]] IoError createDirectory(const std::filesystem::path& path) override;

    /**
     * @brief Creates a directory and every missing parent.
     *
     * @param path The directory to create.
     * @return IoError::Ok, IoError::InvalidPath, IoError::ReadOnly, or a
     *         failure reported by the target backend.
     */
    [[nodiscard]] IoError createDirectories(const std::filesystem::path& path) override;

    /**
     * @brief Renames or moves an entry.
     *
     * Inside one backend the operation is native. Across backends a file is
     * read into memory, written to the target, and then the source is removed -
     * a failure before that step keeps the source - while a directory is
     * refused with IoError::Unsupported and an occupied target with
     * IoError::AlreadyExists.
     *
     * @param from The existing entry.
     * @param to The new path.
     * @return IoError::Ok, IoError::InvalidPath, IoError::NotFound when from
     *         does not exist, IoError::ReadOnly when the two ends do not both
     *         accept writes, IoError::AlreadyExists, IoError::Unsupported, or
     *         a failure reported by a backend.
     */
    [[nodiscard]] IoError rename(const std::filesystem::path& from, const std::filesystem::path& to) override;

    /**
     * @brief Removes one empty directory or one file.
     *
     * @param path The entry to remove.
     * @return IoError::Ok, IoError::InvalidPath, IoError::NotFound,
     *         IoError::ReadOnly, or a failure reported by the owning backend.
     */
    [[nodiscard]] IoError remove(const std::filesystem::path& path) override;

    /**
     * @brief Removes one entry and everything below it.
     *
     * @param path The entry to remove.
     * @return IoError::Ok, IoError::InvalidPath, IoError::NotFound,
     *         IoError::ReadOnly, or a failure reported by the owning backend.
     */
    [[nodiscard]] IoError removeAll(const std::filesystem::path& path) override;

    /**
     * @brief Persists every writable mount, each backend once.
     *
     * Read-only mounts are skipped; the first failure is returned after the
     * remaining writable mounts have been tried.
     *
     * @return IoError::Ok when every tried mount committed, otherwise the
     *         first error reported.
     */
    [[nodiscard]] IoError commit() override;

    /**
     * @brief A mounted tree is not one package, so this is Unsupported.
     *
     * @param path The target file.
     * @return IoError::Unsupported always.
     */
    [[nodiscard]] IoError saveAs(const std::filesystem::path& path) const override;

    /**
     * @brief A mounted tree is not one package, so this is Unsupported.
     *
     * @param out Target output stream.
     * @return IoError::Unsupported always.
     */
    [[nodiscard]] IoError saveAs(std::ostream& out) const override;

    /**
     * @brief A mounted tree is not one package, so this is Unsupported.
     *
     * @return IoError::Unsupported always.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> toBytes() const override;

  private:
    /**
     * @brief One attachment of a backend to a prefix.
     */
    struct Mount
    {
        std::filesystem::path prefix;            ///< Normalized prefix; empty is the virtual root.
        std::shared_ptr<Vfs>  backend;           ///< The mounted tree; never null.
        int                   priority{ 0 };     ///< Higher wins inside one prefix.
        bool                  read_only{ false }; ///< Refuses writes through this mount.
    };

    /**
     * @brief Selects the mounts that can answer one normalized path.
     *
     * @param normalized The normalized path.
     * @return The mounts of the longest matching prefix, priority descending
     *         and mount order for ties; empty when no mount matches.
     */
    [[nodiscard]] std::vector<const Mount*> route(const std::filesystem::path& normalized) const;

    /**
     * @brief Finds the first mount of a group that holds a path.
     *
     * @param group The candidate mounts, in routing order.
     * @param normalized The normalized path.
     * @return The mount, or null when none holds the path.
     */
    [[nodiscard]] const Mount* firstHit(const std::vector<const Mount*>& group,
                                        const std::filesystem::path& normalized) const;

    /**
     * @brief Finds the mount a write to one path has to go through.
     *
     * An existing path is owned by the first hit, writable or not; a new path
     * belongs to the first writable mount of the group.
     *
     * @param group The candidate mounts, in routing order.
     * @param normalized The normalized path.
     * @return The target mount, or null when the write has to be refused.
     */
    [[nodiscard]] const Mount* writeTarget(const std::vector<const Mount*>& group,
                                           const std::filesystem::path& normalized) const;

    /**
     * @brief Finds the first mount of a group that accepts writes.
     *
     * @param group The candidate mounts, in routing order.
     * @return The mount, or null when none accepts writes.
     */
    [[nodiscard]] const Mount* firstWritable(const std::vector<const Mount*>& group) const;

    /**
     * @brief Reports whether a path is a directory that mounts imply.
     *
     * True for the path of a mount and for every ancestor of a mount prefix,
     * so the tree is navigable even when no backend holds those directories.
     *
     * @param normalized The normalized path.
     * @return true when some mount implies the directory.
     */
    [[nodiscard]] bool isImpliedDirectory(const std::filesystem::path& normalized) const;

    /**
     * @brief Reports whether a mount accepts writes.
     *
     * @param mount The mount to test.
     * @return true when neither the mount nor its backend is read-only.
     */
    [[nodiscard]] static bool acceptsWrites(const Mount& mount);

    std::vector<Mount> mounts_;
};

VN_IO_NS_END
