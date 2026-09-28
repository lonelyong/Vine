#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <vine/io/IoError.hpp>
#include <vine/io/io_global.hpp>

VN_IO_NS_BEGIN

/**
 * @brief The "length is not known up front" answer of DataSource::size().
 *
 * A source that cannot state its byte count - content generated with a data-dependent length, or a stream nobody measured -
 * reports this instead. It also tells the consumer that the source cannot be pulled twice: rewind() is a no-op and read()
 * reports the end right away, because only a sequential, single pass is possible.
 *
 * A backend may take such a source or refuse it with IoError::Unsupported, and both are honest answers. A ZIP stores it:
 * the entry then gets a zip64 header whose length is written once the last byte has arrived, which costs a few header
 * bytes. A backend whose format pins the length somewhere that cannot be patched afterwards has to have it before the
 * first byte, and a caller whose source was refused has to spool the content to something measurable first.
 */
inline constexpr std::uint64_t kUnknownSize = std::numeric_limits<std::uint64_t>::max();

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
     * @return The exact byte count, or kUnknownSize when the length cannot be
     *         stated up front (then only one sequential pull is possible); a save
     *         fails when it disagrees with what read() produces.
     */
    [[nodiscard]] virtual std::uint64_t size() const = 0;

    /**
     * @brief Restarts the content from the beginning.
     *
     * Called before every pull, so the same source can feed more than one save.
     * Implementations that track a read position reset it here. A source whose
     * size() is kUnknownSize cannot restart and reports the end at once.
     */
    virtual void rewind() = 0;

    /**
     * @brief Reads the next chunk.
     *
     * @param out Buffer to fill; the source may fill less than its size.
     * @return The number of bytes written, or 0 at the end of the content.
     */
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) = 0;

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * A 0-byte read says "the content ended"; this says whether it ended because
     * the source ran out of data or because it could not produce what it promised
     * (a stream that failed, content that did not check out). A consumer looks here
     * after read() returned 0.
     *
     * @return IoError::Ok while the content reads cleanly, IoError::IoFailure when
     *         it could not be produced; the default is a clean source.
     */
    [[nodiscard]] virtual IoError error() const { return IoError::Ok; }
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

VN_IO_NS_END
