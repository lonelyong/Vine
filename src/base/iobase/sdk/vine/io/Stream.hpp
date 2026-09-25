#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>

VN_IO_NS_BEGIN

/**
 * @brief One contiguous piece of a fragmented entry.
 *
 * A publisher that already holds its data as several buffers (positions, normals
 * and indices, say) hands over a list of these instead of copying everything
 * into one block. The bytes stay owned by the caller and must stay alive until
 * the save finishes.
 */
struct Fragment
{
    const unsigned char* data{ nullptr }; ///< Start of the piece.
    std::uint64_t        size{ 0 };       ///< Length of the piece in bytes.
};

/**
 * @brief A chunk-wise data source: the backend pulls from it when the tree is persisted.
 *
 * Used for content that is generated on the fly or lives in more than one buffer,
 * so it never has to be assembled into one contiguous block first. The source has
 * to stay alive until the save finishes, and read() is called on the thread that
 * performs the save.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API DataSource
{
  public:
    virtual ~DataSource() = default;

    /**
     * @brief Reports the total number of bytes this source produces.
     *
     * @return The exact byte count; a save fails when it disagrees with what read() produces.
     */
    [[nodiscard]] virtual std::uint64_t size() const = 0;

    /**
     * @brief Restarts the content from the beginning.
     *
     * Called before every pull, so the same source can feed more than one save.
     * Implementations that track a read position reset it here.
     */
    virtual void rewind() = 0;

    /**
     * @brief Reads the next chunk.
     *
     * @param out Buffer to fill; the source may fill less than its size.
     * @return The number of bytes written, or 0 at the end of the content.
     */
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) = 0;
};

/**
 * @brief A chunk-wise data sink: a backend pushes entry content into it.
 *
 * This is the streaming counterpart of reading a whole entry into memory, for
 * consumers that walk the bytes once (hashing, parsing, copying to a device
 * buffer).
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API DataSink
{
  public:
    virtual ~DataSink() = default;

    /**
     * @brief Consumes one chunk.
     *
     * @param bytes The chunk; only valid for the duration of the call.
     * @return IoError::Ok to continue; any other value stops the transfer and is
     *         reported by the call that is feeding the sink.
     */
    [[nodiscard]] virtual IoError write(std::span<const std::byte> bytes) = 0;
};

/**
 * @brief Sequential reader over one entry of a virtual file system.
 *
 * A stream outlives the tree it came from, so closing or destroying the VFS does
 * not invalidate it; it does depend on the storage behind the VFS staying
 * readable (a file that is deleted or replaced breaks in-flight reads).
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API VfsReadStream
{
  public:
    virtual ~VfsReadStream() = default;

    /**
     * @brief Reads the next chunk of the entry.
     *
     * @param out Buffer to fill.
     * @return The number of bytes read, or 0 at the end of the entry. A failure also
     *         reports 0 here - error() tells the two apart.
     */
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) = 0;

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
    [[nodiscard]] virtual IoError error() const = 0;

    /**
     * @brief Reports the uncompressed size of the entry.
     *
     * @return The size in bytes.
     */
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

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

VN_IO_NS_END
