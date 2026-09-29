#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>
#include <vine/io/Stream.hpp>
#include <vine/String.hpp>

VN_IO_NS_BEGIN

/**
 * @brief What a virtual path refers to.
 *
 * kindOf() answers with any of the three: Missing is a query that found nothing, either
 * because no entry is there or because the path is not a valid virtual path. A
 * VfsEntryInfo is the answer to a query that succeeded, so the kind it carries is never
 * Missing - what an entry info describes is something that is there.
 */
enum class VfsEntryKind : std::uint8_t
{
    Missing,   ///< Nothing is there: the path is absent, or not a valid virtual path.
    File,      ///< A file: read() works.
    Directory, ///< A directory, explicit or implied by longer names.
};

/**
 * @brief What stat() and list() report about one virtual path.
 *
 * Only an entry that is there is described here: an absent path, or one that is not a
 * valid virtual path, comes back from stat()/list() as an IoError instead, so the kind is
 * always File or Directory. Missing belongs to VfsEntryKind because that is what kindOf()
 * answers with, not because it can appear here.
 */
struct VN_IOBASE_API VfsEntryInfo
{
    std::filesystem::path path;                       ///< Full normalized virtual path; the empty path is the root.
    VfsEntryKind          kind{ VfsEntryKind::File }; ///< What the path names: File or Directory, never Missing.
    std::uint64_t         size{ 0 };                  ///< Size in bytes; 0 for a directory, kUnknownSize when the content has not been produced yet (a source nobody measured).
    std::uint32_t         crc{ 0 };                   ///< Content checksum recorded by the backend (a ZIP records CRC-32); 0 when it has none yet.

    /**
     * @brief The last path segment, i.e. the entry's own name.
     *
     * @return The name, or an empty path for the virtual root.
     */
    [[nodiscard]] std::filesystem::path name() const;
};

/**
 * @brief Sequential reader over one entry of a virtual file system.
 *
 * A source outlives the tree it came from, so closing or destroying the VFS does
 * not invalidate it; it does depend on the storage behind the VFS staying
 * readable (a file that is deleted or replaced breaks in-flight reads). Because it
 * IS a DataSource, an entry can be handed straight to another tree's addFile() -
 * that is how content moves between trees without ever being held whole.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API VfsEntrySource : public DataSource
{
  public:
    /**
     * @brief Reads the next chunk of the entry.
     *
     * @param out Buffer to fill.
     * @return The number of bytes read, or 0 at the end of the entry. A failure also
     *         reports 0 here - error() tells the two apart.
     */
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) override = 0;

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * Decompression finishes (and the content is checked) only once the entry is read
     * past its last byte, so a caller that wants to know whether what it read was
     * intact looks here after read() returned 0.
     *
     * @return IoError::Ok while the entry reads cleanly, IoError::IoFailure when the
     *         content could not be decoded or did not check out.
     */
    [[nodiscard]] virtual IoError error() const override = 0;

    /**
     * @brief Reports the uncompressed size of the entry.
     *
     * @return The size in bytes.
     */
    [[nodiscard]] virtual std::uint64_t size() const noexcept override = 0;

    /**
     * @brief Restarts the entry from its first byte.
     *
     * The DataSource half of this class: what a consumer calls before pulling the
     * entry again. It is seek(0) - and a check that was turned off by an earlier
     * seek to the middle is turned back on by it.
     */
    void rewind() override { static_cast<void>(seek(0)); }

    /**
     * @brief Reports whether seek() is cheap for this entry.
     *
     * A stored entry can be positioned directly; a compressed entry has to be
     * decompressed up to the target offset, so seeking it is allowed but slow.
     *
     * @return true when the reader can position itself without decompressing.
     */
    [[nodiscard]] virtual bool seekable() const noexcept = 0;

    /**
     * @brief Positions the reader at an absolute offset.
     *
     * @param offset Target offset, counted from the start of the entry.
     * @return IoError::Ok on success, IoError::OutOfRange when offset is past the end.
     */
    [[nodiscard]] virtual IoError seek(std::uint64_t offset) = 0;
};

/**
 * @brief A virtual file tree addressed by '/' separated paths.
 *
 * The contract is the same for every backend:
 *
 *   * Paths are '/'-separated and relative to the virtual root, which the
 *     empty string denotes; a leading '/' names no entry and is refused with
 *     IoError::InvalidPath. A path can never escape a backend's own root.
 *   * Results travel back as IoError / Result: there are no out-parameters,
 *     and no failure is reduced to a bare false.
 *   * A new file needs a free name: addFile() writes neither over a file
 *     (AlreadyExists) nor over a directory, explicit or implied
 *     (IsADirectory), refuses a path an ancestor file blocks
 *     (NotADirectory), and refuses the root. Missing parents are implied, so
 *     a parent that is not there is not an error.
 *   * Directories are real even where the storage is not: a directory implied
 *     by a path must be visible, and createDirectories() must make an empty
 *     one observable.
 *   * The primitives are virtual; the conveniences at the bottom are derived
 *     from them once, and the content-source addFile() overloads fall back to
 *     the buffered one - so a backend implements only what it must.
 *
 * See ZipArchive and DirectoryVfs for the backends.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API Vfs
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
    [[nodiscard]] virtual Result<VfsEntryInfo> stat(const std::filesystem::path& path) const = 0;

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
    [[nodiscard]] virtual Result<std::vector<VfsEntryInfo>> list(const std::filesystem::path& dir) const = 0;

    /**
     * @brief Reads a whole virtual file.
     *
     * Use openRead() when the content is large or consumed once: this call
     * holds the whole file in memory.
     *
     * @param path The virtual file path.
     * @return The file bytes, or IoError::NotFound when there is no such file,
     *         IoError::IsADirectory when path names a directory,
     *         IoError::InvalidPath when path is not a valid virtual path,
     *         IoError::IoFailure when the content cannot be read.
     */
    [[nodiscard]] virtual Result<std::vector<unsigned char>> read(const std::filesystem::path& path) const = 0;

    /**
     * @brief Opens a chunk-wise reader over one virtual file.
     *
     * The streaming counterpart of read(): the content is pulled in chunks and
     * never has to be held as one buffer. The source outlives the tree it came
     * from (a ZipArchive's reader keeps the source handle alive); it does depend on
     * the storage staying readable, so a file that is deleted or replaced breaks
     * in-flight reads. Being a DataSource, it can be handed straight to another
     * tree's addFile() - that is how an entry moves between trees without ever
     * being held whole (see copy()).
     *
     * @param path The virtual file path.
     * @return The reader, or IoError::NotFound when there is no such file,
     *         IoError::IsADirectory when path names a directory,
     *         IoError::InvalidPath when path is not a valid virtual path,
     *         IoError::IoFailure when the content cannot be opened.
     */
    [[nodiscard]] virtual Result<std::unique_ptr<VfsEntrySource>> openRead(const std::filesystem::path& path) const = 0;

    /**
     * @brief Pushes a whole virtual file into a sink, chunk by chunk.
     *
     * The push variant of openRead(), for consumers that walk the bytes once
     * (hashing, parsing, uploading) and therefore never want the entry in
     * memory. The default pulls through openRead() and hands the sink one
     * chunk per read; an error the sink reports stops the transfer and is
     * returned as it is. Content that only turns out to be broken at the end
     * of the entry (a damaged archive member) is reported like read() reports
     * it.
     *
     * @param path The virtual file path; openRead()'s errors apply here too.
     * @param sink Receives the content; its write() runs on the calling thread.
     * @return IoError::Ok on success, the sink's own error when it refused a
     *         chunk, IoError::IoFailure when the content could not be read
     *         whole.
     */
    [[nodiscard]] virtual IoError read(const std::filesystem::path& path, DataSink& sink) const;

    /**
     * @brief Adds a whole virtual file whose content is buffered here.
     *
     * Parent directories are implied, but the name has to be free: no entry is
     * replaced, so a file or a directory that already owns the name is refused
     * rather than written over.
     *
     * @param path The virtual file path.
     * @param bytes The bytes to store; may be empty.
     * @return IoError::Ok on success, IoError::AlreadyExists when a file owns
     *         the name, IoError::IsADirectory when the root or a directory owns
     *         it, IoError::NotADirectory when an ancestor is a file,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes) = 0;

    /**
     * @brief Adds a whole virtual file whose content comes from the local file system.
     *
     * The content of real_path becomes readable at path. A backend may read the
     * real file eagerly, or defer the read to saveAs() - so it has to stay
     * readable until the tree is persisted. Parent directories are implied, and
     * the name has to be free, like every other addFile().
     *
     * @param path The virtual file path.
     * @param real_path The physical file to bring in.
     * @return IoError::Ok on success, IoError::NotFound when real_path cannot be
     *         reached, IoError::AlreadyExists when a file owns the name,
     *         IoError::IsADirectory when the root or a directory owns it,
     *         IoError::NotADirectory when an ancestor is a file,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError addFile(const std::filesystem::path& path, const std::filesystem::path& real_path) = 0;

    /**
     * @brief Adds a whole virtual file stored as several separate byte ranges.
     *
     * The pieces are borrowed, not copied: a backend may keep them as they are,
     * so they have to stay alive until the tree is persisted. The default
     * implementation assembles them into one buffer and delegates to
     * addFile(path, span); ZipArchive overrides it to keep the pieces borrowed.
     *
     * @param path The virtual file path.
     * @param fragments The pieces, in order; an empty list adds an empty file.
     * @return IoError::Ok on success, IoError::InvalidData when a non-empty
     *         piece points at no bytes, IoError::AlreadyExists when a file owns
     *         the name, IoError::IsADirectory when the root or a directory owns
     *         it, IoError::NotADirectory when an ancestor is a file,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError addFile(const std::filesystem::path& path, std::span<const Fragment> fragments);

    /**
     * @brief Adds a whole virtual file whose content comes from a pull source.
     *
     * The content is pulled through read(); a backend may pull it right here
     * (the default does, then delegates to addFile(path, span)) or defer the
     * pull to saveAs() (ZipArchive does). Either way the source has to stay
     * alive until the tree is persisted.
     *
     * A source that states a length (size() != kUnknownSize) has to produce exactly
     * that many bytes: stopping short fails the write, here for the default and at
     * saveAs() for a backend that defers.
     * A source that states none (kUnknownSize) is pulled once, in order, until it
     * reports the end by returning nothing; a backend may refuse such a source with
     * IoError::Unsupported when its storage needs the length up front.
     *
     * @param path The virtual file path.
     * @param source The source to pull from; must not be null.
     * @return IoError::Ok on success, IoError::InvalidData when source is null,
     *         IoError::AlreadyExists when a file owns the name,
     *         IoError::IsADirectory when the root or a directory owns it,
     *         IoError::NotADirectory when an ancestor is a file,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError addFile(const std::filesystem::path& path, std::shared_ptr<DataSource> source);

    /**
     * @brief Adds a whole real directory, and everything below it, into the tree.
     *
     * Every regular file below dir becomes a virtual file, its relative path
     * appended to prefix. Empty subdirectories are kept as directories. Files are
     * brought in one by one through addFile() with a local path, so a backend
     * keeps whatever laziness that overload has.
     *
     * @param prefix The virtual directory to import into; empty means the root.
     * @param dir The physical directory to walk; it is not removed.
     * @return IoError::Ok on success, IoError::NotFound when dir is not a directory,
     *         or the first failure an import reports - an entry the tree already
     *         holds (IoError::AlreadyExists) is one of them, since nothing is
     *         replaced.
     */
    [[nodiscard]] IoError addDirectory(const std::filesystem::path& prefix, const std::filesystem::path& dir);

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
    [[nodiscard]] virtual IoError createDirectory(const std::filesystem::path& path) = 0;

    /**
     * @brief Creates a virtual directory together with every missing parent.
     *
     * @param path The directory to create; an existing directory is not an error.
     * @return IoError::Ok on success, IoError::NotADirectory when a file blocks
     *         the way, IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError createDirectories(const std::filesystem::path& path) = 0;

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
    [[nodiscard]] virtual IoError rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;

    /**
     * @brief Removes a file or an empty directory.
     *
     * @param path The file or empty directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::NotEmpty when the directory still holds entries,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError remove(const std::filesystem::path& path) = 0;

    /**
     * @brief Removes a file or a whole subtree.
     *
     * @param path The file or directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::NotFound when nothing is there,
     *         IoError::ReadOnly when the backend refuses writes,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] virtual IoError removeAll(const std::filesystem::path& path) = 0;

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
     * @brief Reports what a virtual path refers to, in one query.
     *
     * Derived from stat(), so the root always reports Directory. An invalid
     * path reports Missing like an absent one: use stat() when the two have to
     * be told apart. When a single yes/no answer is all that is wanted,
     * exists() / isFile() / isDirectory() say it more directly.
     *
     * @param path The virtual path to query; empty denotes the root.
     * @return The kind of the path.
     */
    [[nodiscard]] VfsEntryKind kindOf(const std::filesystem::path& path) const;

    /**
     * @brief Moves one entry from another tree into this one, chunk by chunk.
     *
     * Derived from openRead() on the source and the content-source addFile() here,
     * so nothing is ever held whole. A backend that keeps content sources lazily
     * (ZipArchive) pulls the bytes when THIS tree is persisted instead of now -
     * the source entry and its storage therefore have to stay readable until then.
     *
     * @param source The tree to read from; it is not modified.
     * @param from The entry to copy, named in source.
     * @param to The path to write in this tree; it must be free, since an entry
     *           is never replaced.
     * @return IoError::Ok on success, or the first error either side reports
     *         (openRead()'s errors for from, addFile()'s for to - IoError::AlreadyExists
     *         when to is taken).
     */
    [[nodiscard]] IoError copy(const Vfs& source, const std::filesystem::path& from,
                               const std::filesystem::path& to);

    /**
     * @brief Checks whether a virtual file or directory exists.
     *
     * The virtual root always exists.
     *
     * @param path The virtual path to check.
     * @return true when path names an existing file, directory or the root.
     */
    [[nodiscard]] bool exists(const std::filesystem::path& path) const;

    /**
     * @brief Checks whether a virtual path names a file (not a directory).
     *
     * @param path The virtual path to check.
     * @return true when path names an existing file.
     */
    [[nodiscard]] bool isFile(const std::filesystem::path& path) const;

    /**
     * @brief Checks whether a virtual path names a directory.
     *
     * The virtual root is always a directory.
     *
     * @param path The virtual path to check.
     * @return true when path names an existing directory or the root.
     */
    [[nodiscard]] bool isDirectory(const std::filesystem::path& path) const;
};

VN_IO_NS_END
