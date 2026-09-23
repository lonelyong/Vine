#pragma once
#include "io_global.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <vector>

#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/String.hpp>

V_IO_NS_BEGIN

/**
 * @brief One entry of an archive, as reported by ZipArchive::entries().
 */
struct ZipEntryInfo
{
    String        name;                  ///< Entry name, using '/' separators; a directory ends with '/'.
    std::uint64_t size{ 0 };             ///< Uncompressed size in bytes; always 0 for a directory.
    std::uint32_t crc{ 0 };              ///< Checksum of the content, as recorded by the archive.
    bool          is_directory{ false }; ///< true when the entry marks a directory.
};

/**
 * @brief Creates and extracts ZIP archives.
 *
 * A ZIP is a file system in its own right: one file holding a flat list of entry
 * names that spell out a tree. This class is therefore both the ZIP codec and the
 * Vfs implementation over it - there is no second type to keep in step.
 *
 * Entries are addressed with '/' separators, so a directory tree becomes
 * entries such as "subdir/file.txt". Entries added for writing are buffered
 * in memory until saveAs() writes them to the output file.
 *
 * It comes up in one of three ways:
 *   * ZipArchive() starts an empty, writable archive with nothing behind it;
 *   * openForRead() attaches an existing archive as a read-only view;
 *   * open() attaches one that may also be changed, and commit() writes the
 *     result back over it.
 *
 * Opening reads the archive directory only, so a large package costs its entry
 * list and nothing more; entry contents are decompressed when they are read.
 */
class V_IOBASE_API ZipArchive : public Vfs
{
  public:
    /**
     * @brief What an entry name refers to.
     */
    enum class EntryKind : std::uint8_t
    {
        Missing,   ///< Nothing is there.
        File,      ///< A file: read() and openRead() work.
        Directory, ///< A directory, explicit or implied by longer names.
    };

    ZipArchive();
    ~ZipArchive() override;

    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    /**
     * @brief Moves an archive, handle included.
     *
     * @param other The archive to take over; it is left empty or closed.
     */
    ZipArchive(ZipArchive&& other) noexcept;
    ZipArchive& operator=(ZipArchive&& other) noexcept;

    /**
     * @brief Lists the entries of a ZIP file with their names, sizes and kinds.
     *
     * Only the archive directory is read, so the entries themselves are not
     * decompressed - this is what lets a backend stay lazy.
     *
     * @param path Input .zip file path.
     * @return The entries, IoError::NotFound when the file does not exist, or IoError::InvalidData when it is not an archive.
     */
    [[nodiscard]] static Result<std::vector<ZipEntryInfo>> entries(const std::filesystem::path& path);

    /**
     * @brief Lists the entries of an in-memory ZIP.
     *
     * @param data ZIP bytes, which must stay readable for the call.
     * @param size Number of bytes.
     * @return The entries, or IoError::InvalidData when the bytes are not an archive.
     */
    [[nodiscard]] static Result<std::vector<ZipEntryInfo>> entries(const void* data, std::size_t size);

    /**
     * @brief Decompresses all entries of a ZIP file into a directory.
     *
     * Entry names that could escape the destination directory are rejected.
     *
     * @param path Input .zip file path.
     * @param dir_path Destination directory, created if missing.
     * @return true on success.
     */
    static bool decompressFile(const std::filesystem::path& path, const std::filesystem::path& dir_path);

    /**
     * @brief Reads one entry of a ZIP file into a buffer.
     *
     * @param path Input .zip file path.
     * @param name Entry name to read.
     * @param out Receives the entry bytes.
     * @return true on success.
     */
    static bool readEntry(const std::filesystem::path& path, const String& name, std::vector<unsigned char>& out);

    /**
     * @brief Reads one entry of an in-memory ZIP into a buffer.
     *
     * @param data ZIP bytes.
     * @param size Number of bytes.
     * @param name Entry name to read.
     * @param out Receives the entry bytes.
     * @return true on success.
     */
    static bool readEntry(const void* data, std::size_t size, const String& name,
                          std::vector<unsigned char>& out);

    // The surface below is the target shape: an archive that is either empty (being built) or
    // opened (a source plus the changes made to it), chunk-wise reads, and a persistence step that
    // copies untouched entries without decompressing them. The entries() / readEntry() group above
    // is folded into it as the callers migrate.

    /**
     * @brief Opens an existing ZIP file, reading only its directory.
     *
     * The handle is kept open, so the file has to stay readable (and must not be
     * replaced) while the archive is used. Entry contents are decompressed one at
     * a time, when they are read.
     *
     * @param path The .zip file path.
     * @return The archive, IoError::NotFound when the file does not exist,
     *         IoError::InvalidData when it is not an archive.
     */
    [[nodiscard]] static Result<ZipArchive> open(const std::filesystem::path& path);

    /**
     * @brief Opens an in-memory ZIP, reading only its directory.
     *
     * @param bytes The ZIP bytes; the archive takes them over, so the caller may let go.
     * @return The archive, or IoError::InvalidData when the bytes are not an archive.
     */
    [[nodiscard]] static Result<ZipArchive> open(std::vector<unsigned char> bytes);

    /**
     * @brief Opens an existing ZIP file as a read-only view.
     *
     * Same as open(), but every change is refused with IoError::ReadOnly, so a
     * package that is only meant to be read cannot be modified by accident.
     *
     * @param path The .zip file path; it has to stay readable while the view is used.
     * @return The archive, IoError::NotFound when the file does not exist,
     *         IoError::InvalidData when it is not an archive.
     */
    [[nodiscard]] static Result<ZipArchive> openForRead(const std::filesystem::path& path);

    /**
     * @brief Opens in-memory ZIP bytes as a read-only view.
     *
     * @param bytes The ZIP bytes; the archive takes them over.
     * @return The archive, or IoError::InvalidData when the bytes are not an archive.
     */
    [[nodiscard]] static Result<ZipArchive> openForRead(std::vector<unsigned char> bytes);

    /**
     * @brief Reports whether this archive was opened from an existing one.
     *
     * @return true when the directory index came from a ZIP, false for an empty archive.
     */
    [[nodiscard]] bool isOpen() const noexcept;

    // Vfs
    /**
     * @brief Reports whether this archive refuses changes.
     *
     * @return true for an archive attached by openForRead(), false otherwise.
     */
    [[nodiscard]] bool isReadOnly() const noexcept override;

    /**
     * @brief Reports type and size of a virtual path.
     *
     * @param path The virtual path to query; empty denotes the root.
     * @return The information, or IoError::NotFound when nothing is there,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] Result<FileInfo> stat(const String& path) const override;

    /**
     * @brief Lists the direct children of a virtual directory.
     *
     * @param dir The virtual directory to list; empty denotes the root.
     * @return The children, IoError::NotFound when dir does not exist,
     *         IoError::NotADirectory when dir names a file,
     *         IoError::InvalidPath when dir is not a valid virtual path.
     */
    [[nodiscard]] Result<std::vector<FileInfo>> list(const String& dir) const override;

    /**
     * @brief Writes a whole virtual file.
     *
     * @param path The virtual file path.
     * @param bytes The bytes to store; may be empty.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::IsADirectory when the name is taken by a directory,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError write(const String& path, std::span<const unsigned char> bytes) override;

    /**
     * @brief Creates a virtual directory.
     *
     * @param path The directory to create; the root always exists.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::AlreadyExists when the name is taken, IoError::NotFound when
     *         the parent is missing, IoError::NotADirectory when a file blocks the
     *         way, IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError createDirectory(const String& path) override;

    /**
     * @brief Creates a virtual directory together with every missing parent.
     *
     * @param path The directory to create; an existing directory is not an error.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::AlreadyExists when a file owns the name,
     *         IoError::NotADirectory when a file blocks the way,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError createDirectories(const String& path) override;

    /**
     * @brief Renames or moves a file or a whole subtree.
     *
     * @param from The existing path to move; the root cannot be moved.
     * @param to The target path; the root cannot be a target.
     * @return IoError::Ok on success (also when both name the same path),
     *         IoError::ReadOnly on a read-only archive, IoError::NotFound when from
     *         is missing or the target's parent is, IoError::AlreadyExists when to is
     *         taken, IoError::NotADirectory when a file blocks the target's parent,
     *         IoError::InvalidPath when either path is invalid or to lies below from.
     */
    [[nodiscard]] IoError rename(const String& from, const String& to) override;

    /**
     * @brief Removes a file or an empty directory.
     *
     * @param path The file or empty directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::NotFound when nothing is there, IoError::NotEmpty when the
     *         directory still holds entries, IoError::InvalidPath when path is not a
     *         valid virtual path.
     */
    [[nodiscard]] IoError remove(const String& path) override;

    /**
     * @brief Removes a file or a whole subtree.
     *
     * @param path The file or directory to remove; the root cannot be removed.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::NotFound when nothing is there, IoError::InvalidPath when path
     *         is not a valid virtual path.
     */
    [[nodiscard]] IoError removeAll(const String& path) override;

    /**
     * @brief Brings a real file into the archive.
     *
     * The real file is read when the archive is persisted, so it has to stay
     * readable until then; nothing is buffered up front.
     *
     * @param path The virtual file path.
     * @param real_path The physical file to read from.
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::NotFound when real_path cannot be reached,
     *         IoError::IsADirectory when the name is taken by a directory,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] IoError importFile(const String& path, const std::filesystem::path& real_path) override;

    /**
     * @brief Reads a whole virtual file, decompressing it on demand.
     *
     * Use openRead() instead when the content is large: this call holds the whole
     * entry in memory.
     *
     * @param path The virtual file path.
     * @return The file bytes, IoError::NotFound when there is no such file,
     *         IoError::IsADirectory when path names a directory,
     *         IoError::IoFailure when the content cannot be read,
     *         IoError::InvalidPath when path is not a valid virtual path.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> read(const String& path) const override;

    /**
     * @brief Opens a chunk-wise reader over one entry.
     *
     * @param name Entry name.
     * @return The reader, IoError::NotFound when there is no such entry,
     *         IoError::IsADirectory when name is a directory.
     */
    [[nodiscard]] Result<std::unique_ptr<VfsReadStream>> openRead(const String& name) const;

    /**
     * @brief Adds or replaces an entry whose content comes from a pull source.
     *
     * The source is only read when the archive is persisted, so content that is
     * generated on the fly never has to be materialized first. The archive keeps
     * the source alive; size() has to match what the source produces.
     *
     * @param name Entry name, using '/' separators.
     * @param source The source to pull from.
     * @return true on success.
     */
    bool addFile(const String& name, std::shared_ptr<DataSource> source);

    /**
     * @brief Adds or replaces an entry stored as several separate byte ranges.
     *
     * The pieces are borrowed, not copied, so publisher data that already lives
     * in more than one buffer does not have to be concatenated. They have to stay
     * alive until the archive is persisted.
     *
     * @param name Entry name, using '/' separators.
     * @param fragments The pieces, in order.
     * @return true on success.
     */
    bool addFile(const String& name, std::span<const Fragment> fragments);

    /**
     * @brief Writes the whole archive to a ZIP file, streaming entry by entry.
     *
     * Entries that still come from the archive this one was opened on are copied
     * as they are: their compressed data is moved without being decompressed or
     * recompressed. File-backed and pull-source entries are read while writing, so
     * the peak memory is one entry, not the archive.
     *
     * @param path Target .zip file path.
     * @return IoError::Ok on success, IoError::IoFailure when the archive cannot be written.
     */
    [[nodiscard]] IoError saveAs(const std::filesystem::path& path) const override;

    /**
     * @brief Writes the whole archive as ZIP bytes into an output stream.
     *
     * A ZIP needs to seek back while it is written, so an arbitrary output stream
     * cannot be the target: the bytes are assembled first and handed over in one
     * go. Use saveAs(path) when the archive may be large.
     *
     * @param out Target output stream.
     * @return IoError::Ok on success, IoError::IoFailure when the archive cannot be written.
     */
    [[nodiscard]] IoError saveAs(std::ostream& out) const override;

    /**
     * @brief Serializes the whole archive as ZIP bytes.
     *
     * Holds the entire archive in memory twice over, so it is meant for small
     * archives; large ones go through saveAs(path).
     *
     * @return The ZIP bytes, or IoError::IoFailure when the archive cannot be built.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> toBytes() const override;

    /**
     * @brief Writes the changes back to the file this archive was opened from.
     *
     * The new archive is built next to the target and then replaces it in one
     * step, so a failure leaves the previous file untouched; afterwards the source
     * handle is re-opened on the new file.
     *
     * @return IoError::Ok on success, IoError::ReadOnly on a read-only archive,
     *         IoError::Unsupported when this archive was not opened from a file,
     *         IoError::IoFailure when the archive cannot be written.
     */
    [[nodiscard]] IoError commit() override;

  private:
    /**
     * @brief Reports whether a change is allowed.
     *
     * @return IoError::ReadOnly on a read-only archive, IoError::Ok otherwise.
     */
    [[nodiscard]] IoError guard() const;

    /**
     * @brief Reports whether a path is taken by a directory.
     *
     * @param normalized A normalized virtual path; empty denotes the root.
     * @return true when the path names a directory.
     */
    [[nodiscard]] bool isDirectoryPath(const String& normalized) const;

    /**
     * @brief Reports whether a file blocks a path or one of its ancestors.
     *
     * @param normalized A normalized virtual path; empty denotes the root.
     * @return IoError::NotADirectory when the nearest existing part of the path is a
     *         file, IoError::Ok otherwise.
     */
    [[nodiscard]] IoError ancestorBlockerOf(const String& normalized) const;

    /**
     * @brief Inserts a directory marker, checked by the caller.
     *
     * A directory with content is implied by its entries; an empty one needs a
     * marker of its own, which is what createDirectory() and createDirectories()
     * ask for here. The path must already be normalized.
     *
     * @param path A normalized virtual path; empty denotes the root, which never
     *        needs a marker.
     * @return true on success.
     */
    bool insertDirectory(const String& path);

    /**
     * @brief Inserts or replaces an entry whose content is buffered here.
     *
     * @param path A normalized virtual path.
     * @param bytes The content; it is copied into the entry table.
     * @return true on success.
     */
    bool insertBytes(const String& path, std::span<const unsigned char> bytes);

    /**
     * @brief Inserts or replaces a file-backed entry.
     *
     * The file is read when the archive is persisted, so it has to stay readable
     * until then.
     *
     * @param path A normalized virtual path.
     * @param src_path The physical file to read from.
     * @return true on success.
     */
    bool insertFileBacked(const String& path, const std::filesystem::path& src_path);

    /**
     * @brief Reports what a stored entry name refers to.
     *
     * A directory that exists only as the middle of longer names counts as one.
     *
     * @param name A stored entry name; empty denotes the root.
     * @return The kind of the name.
     */
    [[nodiscard]] EntryKind kindOf(const String& name) const;

    /**
     * @brief Reports the uncompressed size of one stored entry.
     *
     * @param name A stored entry name.
     * @return The size in bytes, or 0 when there is no such entry.
     */
    [[nodiscard]] std::uint64_t sizeOf(const String& name) const;

    /**
     * @brief Lists the direct children of one stored directory.
     *
     * @param dir A stored directory name; empty denotes the archive root.
     * @return The children, one segment deep and deduplicated.
     */
    [[nodiscard]] std::vector<ZipEntryInfo> children(const String& dir) const;

    /**
     * @brief Lists every entry this archive holds, as stored.
     *
     * @return The entry list: names, kinds and sizes.
     */
    [[nodiscard]] std::vector<ZipEntryInfo> index() const;

    /**
     * @brief Removes one stored entry.
     *
     * @param name A stored entry name.
     * @return true when the entry was there, false when it was not.
     */
    bool removeEntry(const String& name);

    /**
     * @brief Renames one stored entry.
     *
     * @param from The existing entry name.
     * @param to The new entry name; it must not be taken.
     * @return true on success.
     */
    bool renameEntry(const String& from, const String& to);

    /**
     * @brief Reads one entry by the name the archive stores it under.
     *
     * @param name An entry name as stored, already normalized by the caller.
     * @return The entry bytes, or the failure the entry read reports.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> readStored(const String& name) const;

    /**
     * @brief Where the content of one entry comes from.
     */
    struct Entry
    {
        std::vector<unsigned char>  data;                  ///< Buffered content.
        std::filesystem::path       src;                   ///< Content file when from_file is true.
        std::shared_ptr<DataSource> generator;             ///< Pull source, read while the archive is written.
        std::uint64_t               size{ 0 };              ///< Size recorded by the source archive.
        std::uint32_t               crc{ 0 };               ///< Checksum recorded by the source archive.
        std::uint64_t               source_index{ 0 };     ///< Index in the source archive when from_source is true.
        bool                        from_file{ false };
        bool                        from_source{ false }; ///< Content still lives in the opened archive.
        bool                        is_directory{ false }; ///< Directory marker (empty directories need one).
    };

    /**
     * @brief The open source archive, shared with the readers handed out for it.
     */
    struct ArchiveHandle
    {
        ArchiveHandle();
        ~ArchiveHandle();
        ArchiveHandle(const ArchiveHandle&) = delete;
        ArchiveHandle& operator=(const ArchiveHandle&) = delete;

        std::vector<unsigned char> bytes;              ///< Bytes the handle borrows, when memory-backed.
        void*                      archive{ nullptr }; ///< libzip's zip_t.
    };

    /**
     * @brief A reader over one entry: decompressing it, or walking buffered bytes.
     */
    class EntryReadStream;

    /**
     * @brief Gets the whole content of an entry that is buffered, fragmented or generated.
     *
     * @param entry The entry to read; must not be source-backed.
     * @return The content bytes, or IoError::IoFailure when a generator misbehaves.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> contentOf(const Entry& entry) const;

    /**
     * @brief Reports the uncompressed size of one entry.
     *
     * A file-backed entry whose file cannot be reached reports 0 here; reading it
     * is what fails.
     *
     * @param entry The entry to size.
     * @return The size in bytes.
     */
    [[nodiscard]] std::uint64_t entrySize(const Entry& entry) const;

    /**
     * @brief Adds every entry of this archive to an open target archive.
     *
     * Entries that still live in the source archive are copied through without
     * being decompressed; everything else is read from wherever its content is.
     *
     * @param target The open target archive, created or truncated by the caller.
     * @return true on success.
     */
    bool emit(void* target) const;

    /**
     * @brief Builds the archive in memory.
     *
     * @return The ZIP bytes, or IoError::IoFailure when it cannot be built.
     */
    [[nodiscard]] Result<std::vector<unsigned char>> buildToBytes() const;

    /**
     * @brief Opens the given ZIP bytes and fills the entry table from its directory.
     *
     * @param bytes The ZIP bytes; the archive takes them over.
     * @return IoError::Ok on success.
     */
    [[nodiscard]] IoError adoptBytes(std::vector<unsigned char> bytes);

    /**
     * @brief Reads the directory of an open source archive into the entry table.
     *
     * @param handle The source handle, already opened; the table is left untouched on failure.
     * @param path The file the handle came from; empty for a memory-backed archive.
     * @return IoError::Ok on success.
     */
    [[nodiscard]] IoError adoptHandle(std::shared_ptr<ArchiveHandle> handle, const std::filesystem::path& path);

    /**
     * @brief Opens a ZIP file and fills the entry table from its directory.
     *
     * @param path The .zip file path.
     * @return IoError::Ok on success, IoError::NotFound when the file is missing,
     *         IoError::InvalidData when it is not an archive, IoError::IoFailure otherwise.
     */
    [[nodiscard]] IoError adoptFile(const std::filesystem::path& path);

    std::map<String, Entry>        entries_;      ///< Entry name to where its content comes from.
    std::shared_ptr<ArchiveHandle> handle_;       ///< The source archive, when this one was opened.
    std::filesystem::path          source_path_;  ///< File the handle came from, when it is file-backed.
    bool                           read_only_{ false }; ///< Set by openForRead().
};

V_IO_NS_END
