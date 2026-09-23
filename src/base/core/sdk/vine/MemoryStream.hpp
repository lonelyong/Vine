#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <ios>
#include <istream>
#include <limits>
#include <ostream>
#include <span>
#include <streambuf>
#include <vector>

V_CORE_NS_BEGIN

/**
 * @brief A stream buffer over one contiguous, growing block of memory.
 *
 * Reads and writes share a single `std::vector<std::byte>`: the put area spans the whole physical capacity, so appending never disturbs the get area, while the get area ends at the logical size, so a reader never sees unwritten bytes.
 * Random access is supported in both directions, and seeking past the logical end of an output stream zero-fills the gap.
 *
 * Three invariants keep the buffer consistent:
 *  - `buffer_.size()` is the physical capacity, `size_` the logical data size.
 *  - The put area ends at `buffer_.size()`, so `*pptr()` is always constructed memory.
 *  - The get area ends at `size_`, so reads stop at the logical end.
 *
 * Writes go through `buffer_.data()`, never through `buffer_[i]`, because only the first `size_` elements are logically initialized.
 * A moved-from buffer is left valid and empty.
 */
class V_CORE_API MemoryStreamBuf : public std::streambuf
{
  public:
    /**
     * @brief Constructs an empty buffer.
     */
    MemoryStreamBuf();

    /**
     * @brief Constructs a buffer holding a copy of the given bytes.
     *
     * @param src Bytes to copy.
     */
    explicit MemoryStreamBuf(std::span<const std::byte> src);

    /**
     * @brief Constructs a buffer that takes ownership of the given bytes.
     *
     * @param src Bytes to take ownership of, moved from.
     */
    explicit MemoryStreamBuf(std::vector<std::byte>&& src);

    /**
     * @brief Constructs a buffer by taking over another one.
     *
     * @param other Buffer to move from, left valid and empty.
     */
    MemoryStreamBuf(MemoryStreamBuf&& other) noexcept;

    /**
     * @brief Takes over another buffer.
     *
     * @param other Buffer to move from, left valid and empty.
     * @return This buffer.
     */
    MemoryStreamBuf& operator=(MemoryStreamBuf&& other) noexcept;

    MemoryStreamBuf(const MemoryStreamBuf&) = delete;
    MemoryStreamBuf& operator=(const MemoryStreamBuf&) = delete;

    /**
     * @brief Destroys the buffer and releases its storage.
     */
    ~MemoryStreamBuf() override;

    /**
     * @brief Returns a pointer to the first data byte.
     *
     * @return Pointer to the logical start, valid until the next write, or nullptr when the buffer is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns the logical data size.
     *
     * Single-character inserters write straight into the put area, which advances the put pointer without touching `size_`, so the put position is folded in here.
     * Writing in the middle of the data never shrinks the result.
     *
     * @return Number of bytes written so far.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Hands over the content and empties the buffer.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     *
     * Named `clearData()` rather than `clear()` so it cannot hide `std::ios::clear()` on the stream wrappers.
     */
    void clearData() noexcept;

    /**
     * @brief Replaces the content with a copy of the given bytes.
     *
     * The stream is rewound: reading starts at the first byte and writing appends after the last one.
     *
     * @param src Bytes to copy, or an empty span to clear the buffer.
     */
    void reset(std::span<const std::byte> src);

  protected:
    /**
     * @brief Returns the byte the get area points at, or reports end of data.
     *
     * @return The current byte, or eof when the read position is at the logical end.
     */
    std::streambuf::int_type underflow() override;

    /**
     * @brief Appends one byte once the put area is exhausted.
     *
     * @param ch The byte to append, or eof.
     * @return The byte, or eof when @p ch was eof.
     */
    std::streambuf::int_type overflow(std::streambuf::int_type ch) override;

    /**
     * @brief Writes a byte block, growing the storage as needed.
     *
     * @param s Bytes to write.
     * @param count Number of bytes to write.
     * @return The number of bytes written.
     */
    std::streamsize xsputn(const char* s, std::streamsize count) override;

    /**
     * @brief Reads a byte block from the logical content.
     *
     * @param s Buffer to read into.
     * @param count Number of bytes to read.
     * @return The number of bytes read, which is short at the logical end.
     */
    std::streamsize xsgetn(char* s, std::streamsize count) override;

    /**
     * @brief Moves the read and/or write position.
     *
     * @param off Offset to apply, relative to @p dir.
     * @param dir Reference point of @p off.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it is out of range.
     */
    std::streambuf::pos_type seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir,
                                     std::ios_base::openmode which) override;

    /**
     * @brief Moves the read and/or write position to an absolute offset.
     *
     * @param pos Absolute offset to move to.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it is out of range.
     */
    std::streambuf::pos_type seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which) override;

  private:
    [[nodiscard]] std::size_t readPosition() const noexcept;
    [[nodiscard]] std::size_t writePosition() const noexcept;
    void syncGetArea() noexcept;
    void grow(std::size_t needed);
    void setPointers(std::size_t read_off, std::size_t write_off) noexcept;
    void advancePut(std::size_t offset) noexcept;
    void advanceGet(std::size_t count) noexcept;
    void resetState() noexcept;

    static constexpr std::size_t s_minCapacity = 256;
    static constexpr int         s_maxBump = std::numeric_limits<int>::max();

    std::vector<std::byte> buffer_;
    std::size_t            size_ = 0;
};

/**
 * @brief An input stream over a contiguous memory buffer.
 */
class V_CORE_API InputMemoryStream : public std::istream
{
  public:
    /**
     * @brief Constructs an empty input stream.
     */
    InputMemoryStream();

    /**
     * @brief Constructs an input stream holding a copy of the given bytes.
     *
     * @param data Bytes to copy.
     */
    explicit InputMemoryStream(std::span<const std::byte> data);

    /**
     * @brief Constructs an input stream that takes ownership of the given bytes.
     *
     * @param data Bytes to take ownership of, moved from.
     */
    explicit InputMemoryStream(std::vector<std::byte>&& data);

    /**
     * @brief Constructs an input stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    InputMemoryStream(InputMemoryStream&& other) noexcept;

    /**
     * @brief Takes over another input stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    InputMemoryStream& operator=(InputMemoryStream&& other) noexcept;

    InputMemoryStream(const InputMemoryStream&) = delete;
    InputMemoryStream& operator=(const InputMemoryStream&) = delete;

    /**
     * @brief Returns a pointer to the first data byte.
     *
     * @return Pointer to the logical start, valid until the next write, or nullptr when the stream is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes available.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Hands over the content and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

  private:
    MemoryStreamBuf buf_;
};

/**
 * @brief An output stream over a contiguous memory buffer.
 */
class V_CORE_API OutputMemoryStream : public std::ostream
{
  public:
    /**
     * @brief Constructs an empty output stream.
     */
    OutputMemoryStream();

    /**
     * @brief Constructs an output stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    OutputMemoryStream(OutputMemoryStream&& other) noexcept;

    /**
     * @brief Takes over another output stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    OutputMemoryStream& operator=(OutputMemoryStream&& other) noexcept;

    OutputMemoryStream(const OutputMemoryStream&) = delete;
    OutputMemoryStream& operator=(const OutputMemoryStream&) = delete;

    /**
     * @brief Returns a pointer to the first data byte.
     *
     * @return Pointer to the logical start, valid until the next write, or nullptr when the stream is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes written so far.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Hands over the content and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     */
    void clearData() noexcept;

  private:
    MemoryStreamBuf buf_;
};

/**
 * @brief A readable and writable stream over a contiguous memory buffer.
 */
class V_CORE_API MemoryStream : public std::iostream
{
  public:
    /**
     * @brief Constructs an empty stream.
     */
    MemoryStream();

    /**
     * @brief Constructs a stream holding a copy of the given bytes.
     *
     * @param data Bytes to copy.
     */
    explicit MemoryStream(std::span<const std::byte> data);

    /**
     * @brief Constructs a stream that takes ownership of the given bytes.
     *
     * @param data Bytes to take ownership of, moved from.
     */
    explicit MemoryStream(std::vector<std::byte>&& data);

    /**
     * @brief Constructs a stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    MemoryStream(MemoryStream&& other) noexcept;

    /**
     * @brief Takes over another stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    MemoryStream& operator=(MemoryStream&& other) noexcept;

    MemoryStream(const MemoryStream&) = delete;
    MemoryStream& operator=(const MemoryStream&) = delete;

    /**
     * @brief Returns a pointer to the first data byte.
     *
     * @return Pointer to the logical start, valid until the next write, or nullptr when the stream is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes held by the stream.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Hands over the content and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     */
    void clearData() noexcept;

    /**
     * @brief Replaces the content with a copy of the given bytes.
     *
     * The stream is rewound: reading starts at the first byte and writing appends after the last one.
     *
     * @param data Bytes to copy, or an empty span to clear the stream.
     */
    void reset(std::span<const std::byte> data);

  private:
    MemoryStreamBuf buf_;
};

/**
 * @brief A stream buffer over a chain of fixed-size chunks.
 *
 * Writes are append-only and land in `chunk_size_`-byte chunks, so a large stream never needs one contiguous allocation.
 * Reads are random access: a seek only records an offset, and the chunk holding it is located on the next read.
 *
 * Unlike `MemoryStreamBuf`, the get area is a cache rather than the source of truth: `read_pos_` stays authoritative whenever no get area is published, which is what allows seeking to the end and appending afterwards to both behave.
 * A moved-from buffer is left valid and empty.
 */
class V_CORE_API ChunkedMemoryStreamBuf : public std::streambuf
{
  public:
    /**
     * @brief A non-owning view over one chunk.
     */
    struct BufferPart
    {
        const std::byte* data;
        std::size_t      size;
    };

    /**
     * @brief Constructs an empty buffer.
     *
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit ChunkedMemoryStreamBuf(std::size_t chunk_size = s_defaultChunkSize);

    /**
     * @brief Constructs a buffer holding a copy of the given bytes.
     *
     * @param src Bytes to copy.
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit ChunkedMemoryStreamBuf(std::span<const std::byte> src, std::size_t chunk_size = s_defaultChunkSize);

    /**
     * @brief Constructs a buffer by taking over another one.
     *
     * @param other Buffer to move from, left valid and empty.
     */
    ChunkedMemoryStreamBuf(ChunkedMemoryStreamBuf&& other) noexcept;

    /**
     * @brief Takes over another buffer.
     *
     * @param other Buffer to move from, left valid and empty.
     * @return This buffer.
     */
    ChunkedMemoryStreamBuf& operator=(ChunkedMemoryStreamBuf&& other) noexcept;

    ChunkedMemoryStreamBuf(const ChunkedMemoryStreamBuf&) = delete;
    ChunkedMemoryStreamBuf& operator=(const ChunkedMemoryStreamBuf&) = delete;

    /**
     * @brief Destroys the buffer and releases its chunks.
     */
    ~ChunkedMemoryStreamBuf() override;

    /**
     * @brief Returns the content as one contiguous block.
     *
     * The first call costs O(n) to coalesce the chunks; later calls are O(1) until the next write.
     *
     * @return Pointer to the coalesced content, valid until the next write, or nullptr when empty.
     */
    [[nodiscard]] const std::byte* data() const;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes written so far.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns one view per chunk without copying.
     *
     * @return Zero-copy views covering the logical content in order, valid until the next write.
     */
    [[nodiscard]] std::vector<BufferPart> chunks() const;

    /**
     * @brief Copies the content into one vector and empties the buffer.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     *
     * Named `clearData()` rather than `clear()` so it cannot hide `std::ios::clear()` on the stream wrappers.
     */
    void clearData() noexcept;

    inline static constexpr std::size_t s_defaultChunkSize = 64 * 1024;

  protected:
    /**
     * @brief Returns the byte the get area points at, or reports end of data.
     *
     * @return The current byte, or eof when the read position is at the end of the content.
     */
    std::streambuf::int_type underflow() override;

    /**
     * @brief Returns the current byte and advances the read position.
     *
     * @return The current byte, or eof when the read position is at the end of the content.
     */
    std::streambuf::int_type uflow() override;

    /**
     * @brief Appends one byte to the chunk chain.
     *
     * @param ch The byte to append, or eof.
     * @return The byte, or eof when @p ch was eof.
     */
    std::streambuf::int_type overflow(std::streambuf::int_type ch) override;

    /**
     * @brief Appends a byte block to the chunk chain.
     *
     * @param s Bytes to append.
     * @param count Number of bytes to append.
     * @return The number of bytes appended.
     */
    std::streamsize xsputn(const char* s, std::streamsize count) override;

    /**
     * @brief Reads a byte block, crossing chunk boundaries as needed.
     *
     * @param s Buffer to read into.
     * @param count Number of bytes to read.
     * @return The number of bytes read, which is short at the end of the content.
     */
    std::streamsize xsgetn(char* s, std::streamsize count) override;

    /**
     * @brief Moves the read position.
     *
     * Writes are append-only, so a seek that only aims at the put area fails.
     *
     * @param off Offset to apply, relative to @p dir.
     * @param dir Reference point of @p off.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it is out of range.
     */
    std::streambuf::pos_type seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir,
                                     std::ios_base::openmode which) override;

    /**
     * @brief Moves the read position to an absolute offset.
     *
     * @param pos Absolute offset to move to.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it is out of range.
     */
    std::streambuf::pos_type seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which) override;

  private:
    void appendRaw(const std::byte* src, std::size_t len);
    [[nodiscard]] std::size_t readPosition() const noexcept;
    bool activateGetArea() noexcept;
    void republishGetArea() noexcept;
    void detachGetArea() noexcept;
    void publishGetArea(const std::vector<char>& chunk, std::size_t in_chunk_off) noexcept;
    void advanceGet(std::size_t count) noexcept;
    void ensureCoalesced() const;
    void resetState() noexcept;

    static constexpr int s_maxBump = std::numeric_limits<int>::max();

    std::size_t                    chunk_size_;
    std::vector<std::vector<char>> chunks_;
    std::size_t                    read_chunk_idx_ = 0;
    std::size_t                    active_chunk_start_ = 0;
    std::size_t                    read_pos_ = 0;

    mutable std::vector<std::byte> coalesced_;
    mutable bool                   coalesced_valid_ = false;

    std::size_t size_ = 0;
};

/**
 * @brief An input stream over a chain of fixed-size chunks.
 */
class V_CORE_API InputChunkedMemoryStream : public std::istream
{
  public:
    /**
     * @brief Constructs an input stream holding a copy of the given bytes.
     *
     * @param data Bytes to copy.
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit InputChunkedMemoryStream(std::span<const std::byte> data,
                                      std::size_t chunk_size = ChunkedMemoryStreamBuf::s_defaultChunkSize);

    /**
     * @brief Constructs an input stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    InputChunkedMemoryStream(InputChunkedMemoryStream&& other) noexcept;

    /**
     * @brief Takes over another input stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    InputChunkedMemoryStream& operator=(InputChunkedMemoryStream&& other) noexcept;

    InputChunkedMemoryStream(const InputChunkedMemoryStream&) = delete;
    InputChunkedMemoryStream& operator=(const InputChunkedMemoryStream&) = delete;

    /**
     * @brief Returns the content as one contiguous block.
     *
     * @return Pointer to the coalesced content, valid until the next write, or nullptr when empty.
     */
    [[nodiscard]] const std::byte* data() const;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes available.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns one view per chunk without copying.
     *
     * @return Zero-copy views covering the logical content in order.
     */
    [[nodiscard]] std::vector<ChunkedMemoryStreamBuf::BufferPart> chunks() const;

    /**
     * @brief Copies the content into one vector and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

  private:
    ChunkedMemoryStreamBuf buf_;
};

/**
 * @brief An output stream over a chain of fixed-size chunks.
 */
class V_CORE_API OutputChunkedMemoryStream : public std::ostream
{
  public:
    /**
     * @brief Constructs an empty output stream.
     *
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit OutputChunkedMemoryStream(std::size_t chunk_size = ChunkedMemoryStreamBuf::s_defaultChunkSize);

    /**
     * @brief Constructs an output stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    OutputChunkedMemoryStream(OutputChunkedMemoryStream&& other) noexcept;

    /**
     * @brief Takes over another output stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    OutputChunkedMemoryStream& operator=(OutputChunkedMemoryStream&& other) noexcept;

    OutputChunkedMemoryStream(const OutputChunkedMemoryStream&) = delete;
    OutputChunkedMemoryStream& operator=(const OutputChunkedMemoryStream&) = delete;

    /**
     * @brief Returns the content as one contiguous block.
     *
     * @return Pointer to the coalesced content, valid until the next write, or nullptr when empty.
     */
    [[nodiscard]] const std::byte* data() const;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes written so far.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns one view per chunk without copying.
     *
     * @return Zero-copy views covering the logical content in order.
     */
    [[nodiscard]] std::vector<ChunkedMemoryStreamBuf::BufferPart> chunks() const;

    /**
     * @brief Copies the content into one vector and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     */
    void clearData() noexcept;

  private:
    ChunkedMemoryStreamBuf buf_;
};

/**
 * @brief A readable and writable stream over a chain of fixed-size chunks.
 */
class V_CORE_API ChunkedMemoryStream : public std::iostream
{
  public:
    /**
     * @brief Constructs an empty stream.
     *
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit ChunkedMemoryStream(std::size_t chunk_size = ChunkedMemoryStreamBuf::s_defaultChunkSize);

    /**
     * @brief Constructs a stream holding a copy of the given bytes.
     *
     * @param data Bytes to copy.
     * @param chunk_size Bytes per chunk, or 0 to select the default.
     */
    explicit ChunkedMemoryStream(std::span<const std::byte> data,
                                 std::size_t chunk_size = ChunkedMemoryStreamBuf::s_defaultChunkSize);

    /**
     * @brief Constructs a stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    ChunkedMemoryStream(ChunkedMemoryStream&& other) noexcept;

    /**
     * @brief Takes over another stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    ChunkedMemoryStream& operator=(ChunkedMemoryStream&& other) noexcept;

    ChunkedMemoryStream(const ChunkedMemoryStream&) = delete;
    ChunkedMemoryStream& operator=(const ChunkedMemoryStream&) = delete;

    /**
     * @brief Returns the content as one contiguous block.
     *
     * @return Pointer to the coalesced content, valid until the next write, or nullptr when empty.
     */
    [[nodiscard]] const std::byte* data() const;

    /**
     * @brief Returns the logical data size.
     *
     * @return Number of bytes held by the stream.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns one view per chunk without copying.
     *
     * @return Zero-copy views covering the logical content in order.
     */
    [[nodiscard]] std::vector<ChunkedMemoryStreamBuf::BufferPart> chunks() const;

    /**
     * @brief Copies the content into one vector and empties the stream.
     *
     * @return The bytes that were buffered, trimmed to their logical size.
     */
    [[nodiscard]] std::vector<std::byte> release();

    /**
     * @brief Discards the content and rewinds.
     */
    void clearData() noexcept;

  private:
    ChunkedMemoryStreamBuf buf_;
};

/**
 * @brief A stream buffer over a byte window the caller owns.
 *
 * Nothing is ever allocated: the buffer reads and writes the span it was constructed with and refuses to write past its writable room.
 * That is what a fixed-size destination needs, such as a mapped block, a protocol header or a stack buffer, where a growing stream would either overshoot or turn an overflow into a silent allocation.
 * Since the window never moves, `data()` stays valid for the buffer's whole lifetime, unlike `MemoryStreamBuf`.
 *
 * A buffer built from a writable window fills it and exposes the written prefix, while one built by readOnly() is a zero-copy view that exposes the whole window and rejects every write.
 *
 * Three contract points differ from `MemoryStreamBuf`:
 *  - The write position starts at offset 0, so writing overwrites the window from the start instead of appending.
 *  - Running out of room fails: `overflow()` reports eof and `xsputn()` returns only the bytes it could write, so `std::ostream::write()` sets `badbit` while `size()` says how much landed.
 *  - `seekp()` only moves the write position: it writes no byte and leaves `size()` alone, so the bytes of a gap stay whatever the caller left there.
 *
 * No put area is published, so every single byte goes through `overflow()`; bulk writes still reach the window through `xsputn()` in one copy.
 * The window must outlive the buffer, and a moved-from buffer is left valid and empty.
 */
class V_CORE_API SpanStreamBuf : public std::streambuf
{
  public:
    /**
     * @brief Constructs a buffer with no window.
     */
    SpanStreamBuf() noexcept;

    /**
     * @brief Constructs a buffer over an existing writable window.
     *
     * @param window Bytes to write into, borrowed and never freed.
     */
    explicit SpanStreamBuf(std::span<std::byte> window) noexcept;

    /**
     * @brief Constructs a buffer by taking over another one.
     *
     * @param other Buffer to move from, left valid and empty.
     */
    SpanStreamBuf(SpanStreamBuf&& other) noexcept;

    /**
     * @brief Takes over another buffer.
     *
     * @param other Buffer to move from, left valid and empty.
     * @return This buffer.
     */
    SpanStreamBuf& operator=(SpanStreamBuf&& other) noexcept;

    SpanStreamBuf(const SpanStreamBuf&) = delete;
    SpanStreamBuf& operator=(const SpanStreamBuf&) = delete;

    /**
     * @brief Destroys the buffer, leaving the window untouched.
     */
    ~SpanStreamBuf() override;

    /**
     * @brief Returns the start of the window.
     *
     * @return Pointer to the window, stable for the buffer's whole lifetime, or nullptr when the window is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns how many bytes can be read.
     *
     * That is the written prefix of a writable window, or the whole window of a read-only view.
     * Seeking does not change it, so it is a prefix length rather than a highest offset ever visited.
     *
     * @return Number of readable bytes, never more than `capacity()`.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns the window size.
     *
     * @return Number of bytes the window can hold.
     */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /**
     * @brief Rewinds to the start of the window.
     *
     * No byte is written, and the window keeps its content.
     * A writable window forgets its written prefix so it can be filled again, while a read-only view stays readable in full.
     */
    void clearData() noexcept;

    /**
     * @brief Makes a zero-copy read-only buffer over existing content.
     *
     * A named factory rather than another constructor: a `std::span<std::byte>` argument would convert to both overloads and make the call ambiguous.
     *
     * @param content Bytes to read, borrowed and never freed.
     * @return A buffer exposing the whole window, with every write refused.
     */
    [[nodiscard]] static SpanStreamBuf readOnly(std::span<const std::byte> content) noexcept;

  protected:
    /**
     * @brief Returns the byte the get area points at, or reports end of data.
     *
     * @return The current byte, or eof when the read position reached the end of the written prefix.
     */
    std::streambuf::int_type underflow() override;

    /**
     * @brief Appends one byte to the written prefix.
     *
     * @param ch The byte to append, or eof.
     * @return The byte, or eof when the window is full.
     */
    std::streambuf::int_type overflow(std::streambuf::int_type ch) override;

    /**
     * @brief Writes as much of a byte block as fits in the window.
     *
     * @param s Bytes to write.
     * @param count Number of bytes to write.
     * @return The number of bytes written, which is short when the window runs out.
     */
    std::streamsize xsputn(const char* s, std::streamsize count) override;

    /**
     * @brief Reads a byte block from the written prefix.
     *
     * @param s Buffer to read into.
     * @param count Number of bytes to read.
     * @return The number of bytes read, which is short at the end of the written prefix.
     */
    std::streamsize xsgetn(char* s, std::streamsize count) override;

    /**
     * @brief Moves the read and/or write position inside the window.
     *
     * @param off Offset to apply, relative to @p dir.
     * @param dir Reference point of @p off.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it falls outside the window.
     */
    std::streambuf::pos_type seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir,
                                     std::ios_base::openmode which) override;

    /**
     * @brief Moves the read and/or write position to an absolute offset.
     *
     * @param pos Absolute offset to move to.
     * @param which Direction to move, read and/or write.
     * @return The new position, or pos_type(-1) when it falls outside the window.
     */
    std::streambuf::pos_type seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which) override;

  private:
    [[nodiscard]] std::size_t readPosition() const noexcept;
    [[nodiscard]] std::size_t writePosition() const noexcept;
    void setPointers(std::size_t read_off, std::size_t write_off) noexcept;
    void advanceGet(std::size_t count) noexcept;
    void resetState() noexcept;

    static constexpr int s_maxBump = std::numeric_limits<int>::max();

    // Borrowed window: never freed, must outlive the buffer.
    const std::byte* begin_ = nullptr;
    std::size_t      capacity_ = 0;
    std::size_t      size_ = 0;
    std::size_t      put_limit_ = 0;
    std::size_t      put_off_ = 0;
};

/**
 * @brief A read-only input stream over a byte window the caller owns.
 *
 * The content is not copied: the stream reads straight from the span it was given, which must outlive it.
 * Formatted input works as over any other istream, and reading past the window reports end of file.
 */
class V_CORE_API InputSpanStream : public std::istream
{
  public:
    /**
     * @brief Constructs an input stream over existing content.
     *
     * @param content Bytes to read, borrowed and never freed.
     */
    explicit InputSpanStream(std::span<const std::byte> content) noexcept;

    /**
     * @brief Constructs an input stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    InputSpanStream(InputSpanStream&& other) noexcept;

    /**
     * @brief Takes over another input stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    InputSpanStream& operator=(InputSpanStream&& other) noexcept;

    InputSpanStream(const InputSpanStream&) = delete;
    InputSpanStream& operator=(const InputSpanStream&) = delete;

    /**
     * @brief Returns the start of the borrowed content.
     *
     * @return Pointer into the caller's memory, or nullptr when the content is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns how many bytes can be read.
     *
     * @return Number of readable bytes.
     */
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    SpanStreamBuf buf_;
};

/**
 * @brief An output stream over a byte window the caller owns.
 *
 * Writes land in the span the stream was given, which must outlive it.
 * Once the window is full the stream reports the overflow through `badbit` instead of growing.
 */
class V_CORE_API OutputSpanStream : public std::ostream
{
  public:
    /**
     * @brief Constructs an output stream over a writable window.
     *
     * @param window Bytes to write into, borrowed and never freed.
     */
    explicit OutputSpanStream(std::span<std::byte> window) noexcept;

    /**
     * @brief Constructs an output stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    OutputSpanStream(OutputSpanStream&& other) noexcept;

    /**
     * @brief Takes over another output stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    OutputSpanStream& operator=(OutputSpanStream&& other) noexcept;

    OutputSpanStream(const OutputSpanStream&) = delete;
    OutputSpanStream& operator=(const OutputSpanStream&) = delete;

    /**
     * @brief Returns the start of the borrowed window.
     *
     * @return Pointer into the caller's memory, or nullptr when the window is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns how many bytes were written.
     *
     * @return Length of the written prefix, never more than `capacity()`.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns the window size.
     *
     * @return Number of bytes the window can hold.
     */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /**
     * @brief Rewinds to the start of the window so it can be filled again.
     */
    void clearData() noexcept;

  private:
    SpanStreamBuf buf_;
};

/**
 * @brief A readable and writable stream over a byte window the caller owns.
 *
 * Reads see the written prefix and writes land in the same borrowed window, which must outlive the stream.
 * Writing past the window reports the overflow through `badbit` instead of growing.
 */
class V_CORE_API SpanStream : public std::iostream
{
  public:
    /**
     * @brief Constructs a stream over a writable window.
     *
     * @param window Bytes to write into, borrowed and never freed.
     */
    explicit SpanStream(std::span<std::byte> window) noexcept;

    /**
     * @brief Constructs a stream by taking over another one.
     *
     * @param other Stream to move from, left valid and empty.
     */
    SpanStream(SpanStream&& other) noexcept;

    /**
     * @brief Takes over another stream.
     *
     * @param other Stream to move from, left valid and empty.
     * @return This stream.
     */
    SpanStream& operator=(SpanStream&& other) noexcept;

    SpanStream(const SpanStream&) = delete;
    SpanStream& operator=(const SpanStream&) = delete;

    /**
     * @brief Returns the start of the borrowed window.
     *
     * @return Pointer into the caller's memory, or nullptr when the window is empty.
     */
    [[nodiscard]] const std::byte* data() const noexcept;

    /**
     * @brief Returns how many bytes can be read.
     *
     * @return Length of the written prefix, never more than `capacity()`.
     */
    [[nodiscard]] std::size_t size() const noexcept;

    /**
     * @brief Returns the window size.
     *
     * @return Number of bytes the window can hold.
     */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /**
     * @brief Rewinds to the start of the window so it can be filled again.
     */
    void clearData() noexcept;

  private:
    SpanStreamBuf buf_;
};

V_CORE_NS_END
