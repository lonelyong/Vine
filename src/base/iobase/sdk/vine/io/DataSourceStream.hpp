#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <streambuf>

#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/io_global.hpp>

VN_IO_NS_BEGIN

/**
 * @brief A stream buffer over a content source: what the source produces, the stream reads.
 *
 * The fourth member of the memory-stream family (core has the span, the growing buffer and the chunked one): this one
 * covers bytes that a DataSource produces rather than bytes that already sit somewhere. That is what lets anything
 * written against std::istream - a parser, assimp's reader, a test - consume a tree entry (VfsEntrySource) or a
 * generated model (meshio's BinStlSource) without an intermediate buffer and without a temporary file.
 *
 * IT STARTS AT THE SOURCE'S FIRST BYTE: the constructor rewinds, because a stream describes one piece of content from
 * its beginning. A source that cannot restart leaves the stream empty and says so through error().
 *
 * SEEKING IS A REWIND. A source states a size and a restart, not arbitrary positions: seekg(0) works, anything else
 * fails. That is also where a source that cannot restart shows up - seekg fails instead of silently reading nothing.
 *
 * A FAILURE BEHIND THE END. std::istream has no slot for "why the bytes stopped", so a source that broke reads as an
 * early end of the stream. A caller that has to tell a clean end from a broken one asks DataSourceStream::error() once
 * the stream ran dry.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API DataSourceStreamBuf final : public std::streambuf
{
  public:
    /**
     * @brief Buffers the content of a source, starting at its first byte.
     *
     * @param source The source to read; it must outlive this buffer.
     */
    explicit DataSourceStreamBuf(DataSource& source) noexcept;

    /**
     * @brief Destroys the buffer, leaving the source as it is.
     */
    ~DataSourceStreamBuf() override;

    DataSourceStreamBuf(const DataSourceStreamBuf&) = delete;
    DataSourceStreamBuf& operator=(const DataSourceStreamBuf&) = delete;

    /**
     * @brief Reports the length of the content.
     *
     * @return The source's own byte count, or kUnknownSize when the source cannot state one.
     */
    [[nodiscard]] std::uint64_t size() const noexcept;

    /**
     * @brief Reports why the content stopped, when it did not simply end.
     *
     * The source's own answer, not the stream's: a std::istream has nowhere to carry it, and a source may
     * already know that the rest is missing before anything asks it for more.
     *
     * @return IoError::Ok while the source delivers what it promised, or the source's own failure.
     */
    [[nodiscard]] IoError error() const noexcept;

  protected:
    int_type        underflow() override;
    std::streamsize xsgetn(char* out, std::streamsize count) override;
    pos_type        seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override;
    pos_type        seekpos(pos_type pos, std::ios_base::openmode which) override;

  private:
    DataSource*   source_;
    std::uint64_t position_{ 0 };   ///< Bytes the source produced; the byte in the get area counts as produced.
    std::byte     current_{};       ///< The one-byte get area underflow() publishes.
    IoError       error_{ IoError::Ok };
};

/**
 * @brief A content source as a std::istream.
 *
 * The adapter between the two vocabularies this library speaks: a tree hands out a DataSource (openRead), and the
 * readers and parsers out there take a std::istream. Reading to the end is what a parser does; seeking back to the
 * start is what a second pass does.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API DataSourceStream final : public std::istream
{
  public:
    /**
     * @brief Reads a source as a stream, starting at its first byte.
     *
     * @param source The source to read; it must outlive this stream.
     */
    explicit DataSourceStream(DataSource& source);

    /**
     * @brief Destroys the stream.
     */
    ~DataSourceStream() override;

    DataSourceStream(const DataSourceStream&) = delete;
    DataSourceStream& operator=(const DataSourceStream&) = delete;

    /**
     * @brief Reports the length of the content.
     *
     * @return The source's own byte count, or kUnknownSize when the source cannot state one.
     */
    [[nodiscard]] std::uint64_t size() const noexcept;

    /**
     * @brief Reports why the content stopped, when it did not simply end.
     *
     * @return IoError::Ok while the source delivers what it promised, or the source's own failure - which is the
     *         only way to tell a broken source from a clean end of the stream.
     */
    [[nodiscard]] IoError error() const;

  private:
    DataSourceStreamBuf buf_;
};

VN_IO_NS_END
