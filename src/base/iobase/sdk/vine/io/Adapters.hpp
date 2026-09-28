#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <span>
#include <type_traits>

#include <vine/Buffer.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/io_global.hpp>
#include <vine/intrusive_ptr.hpp>

VN_IO_NS_BEGIN

/**
 * @brief An element-wise conversion: In elements come in, Out elements go out.
 *
 * The unit both adapters below take, because the element types have to be nameable somewhere and a lambda cannot
 * carry them into a constructor's template arguments. Naming them once here buys a typed conversion that also comes
 * with a safe spelling for it: the callable may take `auto` parameters, since the types are already fixed by then.
 *
 * @tparam In Element type read; must be trivially copyable, since it is handled as bytes.
 * @tparam Out Element type produced; must be trivially copyable, since it is handed over as bytes.
 */
template <typename In, typename Out>
struct Conversion
{
    /// Elements in, elements out: both spans always hold the same number of elements.
    std::function<void(std::span<const In>, std::span<Out>)> convert;
};

/**
 * @brief A content source over a seekable input stream, converting as it is pulled.
 *
 * The adapter that turns "something with bytes in it" into a DataSource: a file (std::ifstream), a network stream, or a
 * view over memory (vn::InputSpanStream). Nothing is copied up front - the stream is read when the save pulls, one chunk
 * per call - so a huge file or a huge in-memory object reaches a tree without ever being held as a byte block.
 *
 * CONVERTING AS IT IS PULLED. The typed constructor takes elements in and elements out: it reads whole In elements,
 * hands them to the caller's function, and produces Out elements, so data that has to be converted (a double array
 * stored as float, say) is converted on the way rather than being materialized twice. The element sizes drive the pull
 * math and every element boundary is handled here, so the caller's function only ever sees whole elements.
 *
 * LIFETIME. The stream is borrowed and has to stay alive until the save finishes; the same rule as for Fragment and for
 * any source, and it is what makes the borrow-free-of-copies possible. A stream whose content grows or moves while the
 * save runs is a broken contract, not a supported case.
 *
 * SEEKING. rewind() seeks to the start, so the stream has to be seekable; a stream that cannot seek cannot be pulled
 * twice, and the only way to source it is to spool it somewhere seekable first.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API IstreamSource final : public DataSource
{
  public:
    /**
     * @brief Sources the next @p size bytes of a stream, unchanged.
     *
     * @param in The stream to read; it must be seekable and must outlive the save.
     * @param size Bytes to produce, or kUnknownSize to measure the stream instead (still requires seek).
     */
    IstreamSource(std::istream& in, std::uint64_t size);

    /**
     * @brief Sources @p count elements of a stream, converting each one as it is pulled.
     *
     * @tparam In Element type the stream holds.
     * @tparam Out Element type produced.
     * @param in The stream to read; it must be seekable and must outlive the save.
     * @param count Elements to produce, or kUnknownSize to measure the stream instead (still requires seek).
     * @param conversion How one block is turned into the other; a block moves whole elements, never a partial one.
     */
    template <typename In, typename Out>
    IstreamSource(std::istream& in, std::uint64_t count, Conversion<In, Out> conversion)
      : IstreamSource(in, count, sizeof(In), sizeof(Out), wrap<In, Out>(std::move(conversion)))
    {
    }

    /**
     * @brief Destroys the source.
     */
    ~IstreamSource() override;

    IstreamSource(const IstreamSource&) = delete;
    IstreamSource& operator=(const IstreamSource&) = delete;
    IstreamSource(IstreamSource&&) noexcept;
    IstreamSource& operator=(IstreamSource&&) noexcept;

    /**
     * @brief Reports how many bytes this source produces.
     *
     * @return The byte count after conversion (kUnknownSize was measured in the constructor).
     */
    [[nodiscard]] std::uint64_t size() const noexcept override;

    /**
     * @brief Seeks the stream back to its first byte and forgets the conversion state.
     */
    void rewind() override;

    /**
     * @brief Reads and converts the next chunk.
     *
     * @param out Buffer to fill; a converting source produces whole elements only, so a caller
     *            offering less than one element gets no progress from that call.
     * @return The number of bytes written, or 0 at the end of the content.
     */
    [[nodiscard]] std::size_t read(std::span<std::byte> out) override;

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * @return IoError::Ok while the content reads cleanly, IoError::IoFailure when the stream failed or
     *         ended short of the length it was sourced with, IoError::Unsupported when the length could
     *         not be measured (a stream that does not seek), IoError::InvalidData when the stream does not
     *         hold a whole number of elements.
     */
    [[nodiscard]] IoError error() const override;

  private:
    struct Impl;

    /**
     * @brief Wraps a typed conversion into the byte-level one the implementation pulls through.
     *
     * @tparam In Element type read.
     * @tparam Out Element type produced.
     * @param conversion The typed conversion to adapt.
     * @return The same conversion, taking and producing bytes.
     */
    template <typename In, typename Out>
    [[nodiscard]] static std::function<void(std::span<const std::byte>, std::span<std::byte>)> wrap(
        Conversion<In, Out> conversion)
    {
        static_assert(std::is_trivially_copyable_v<In>, "In is read as bytes, so it has to be trivially copyable");
        static_assert(std::is_trivially_copyable_v<Out>, "Out is handed over as bytes, so it has to be trivially copyable");
        return [convert = std::move(conversion.convert)](std::span<const std::byte> raw_in, std::span<std::byte> raw_out) {
            convert(std::span<const In>(reinterpret_cast<const In*>(raw_in.data()), raw_in.size() / sizeof(In)),
                    std::span<Out>(reinterpret_cast<Out*>(raw_out.data()), raw_out.size() / sizeof(Out)));
        };
    }

    /**
     * @brief The seam the typed constructor delegates to; element sizes drive the pull math.
     *
     * @param in The stream to read.
     * @param count Elements to produce, or kUnknownSize to measure the stream.
     * @param in_element Bytes of one element read.
     * @param out_element Bytes of one element produced.
     * @param convert Type-erased block conversion, or null for a plain byte copy.
     */
    IstreamSource(std::istream& in, std::uint64_t count, std::size_t in_element, std::size_t out_element,
                  std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert);

    std::unique_ptr<Impl> impl;
};

/**
 * @brief A content sink over an output stream, converting as it is pushed.
 *
 * The read-side mirror of IstreamSource: a backend pushes entry content into it, one chunk per call, and the bytes land
 * in the stream as they arrive - a copy to a file, a hash, a parser or another tree's writer never holds the entry.
 * Handy together with the memory streams of core (vn::OutputSpanStream for a fixed window, the growing ones otherwise).
 *
 * CONVERTING AS IT IS PUSHED. The typed constructor takes In elements in and writes Out elements, so reading a double
 * array as float costs one conversion and no intermediate buffer. Chunk boundaries do NOT respect element boundaries,
 * so the leftover bytes of a split element are held here and completed by the next chunk.
 *
 * finalize() is the end of the transfer: it flushes the stream and reports content that ended inside an element. A
 * Vfs::read(path, sink) call cannot do that itself, so the caller that owns the concrete sink calls it.
 *
 * LIFETIME. The stream is borrowed and has to stay alive while the sink is used.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects share, are synchronized by the caller;
 * objects that share nothing (buffer, handle or storage) may be used concurrently.
 */
class VN_IOBASE_API OstreamSink final : public DataSink
{
  public:
    /**
     * @brief Writes every chunk to a stream, unchanged.
     *
     * @param out The stream to write; it must outlive the sink.
     */
    explicit OstreamSink(std::ostream& out);

    /**
     * @brief Writes every chunk to a stream, converting the elements first.
     *
     * @tparam In Element type the chunks hold.
     * @tparam Out Element type written.
     * @param out The stream to write; it must outlive the sink.
     * @param conversion How one block is turned into the other; a block that ends inside an element is
     *        completed by the next chunk, so no element is ever dropped.
     */
    template <typename In, typename Out>
    OstreamSink(std::ostream& out, Conversion<In, Out> conversion)
      : OstreamSink(out, sizeof(In), sizeof(Out), wrap<In, Out>(std::move(conversion)))
    {
    }

    /**
     * @brief Destroys the sink.
     */
    ~OstreamSink() override;

    OstreamSink(const OstreamSink&) = delete;
    OstreamSink& operator=(const OstreamSink&) = delete;
    OstreamSink(OstreamSink&&) noexcept;
    OstreamSink& operator=(OstreamSink&&) noexcept;

    /**
     * @brief Consumes one chunk.
     *
     * @param bytes The chunk; only valid for the duration of the call.
     * @return IoError::Ok on success, IoError::IoFailure when the stream refused the bytes.
     */
    [[nodiscard]] IoError write(std::span<const std::byte> bytes) override;

    /**
     * @brief Ends the transfer: flushes the stream and reports content that ended inside an element.
     *
     * @return IoError::Ok on success, IoError::InvalidData when the chunks ended inside an element of a
     *         converting sink, IoError::IoFailure when flushing the stream failed.
     */
    [[nodiscard]] IoError finalize();

  private:
    struct Impl;

    /**
     * @brief Wraps a typed conversion into the byte-level one the implementation pushes through.
     *
     * @tparam In Element type received.
     * @tparam Out Element type written.
     * @param conversion The typed conversion to adapt.
     * @return The same conversion, taking and producing bytes.
     */
    template <typename In, typename Out>
    [[nodiscard]] static std::function<void(std::span<const std::byte>, std::span<std::byte>)> wrap(
        Conversion<In, Out> conversion)
    {
        static_assert(std::is_trivially_copyable_v<In>, "In arrives as bytes, so it has to be trivially copyable");
        static_assert(std::is_trivially_copyable_v<Out>, "Out is handed over as bytes, so it has to be trivially copyable");
        return [convert = std::move(conversion.convert)](std::span<const std::byte> raw_in, std::span<std::byte> raw_out) {
            convert(std::span<const In>(reinterpret_cast<const In*>(raw_in.data()), raw_in.size() / sizeof(In)),
                    std::span<Out>(reinterpret_cast<Out*>(raw_out.data()), raw_out.size() / sizeof(Out)));
        };
    }

    /**
     * @brief The seam the typed constructor delegates to; element sizes drive the conversion math.
     *
     * @param out The stream to write.
     * @param in_element Bytes of one element received.
     * @param out_element Bytes of one element written.
     * @param convert Type-erased block conversion, or null for a plain byte copy.
     */
    OstreamSink(std::ostream& out, std::size_t in_element, std::size_t out_element,
                std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert);

    std::unique_ptr<Impl> impl;
};

/**
 * @brief A core buffer as a content source.
 *
 * The bytes are the buffer's own elements, handed out as they are pulled, so data that already lives in a buffer reaches
 * a package without being copied first. The source HOLDS the buffer: content that a backend pulls later (a ZIP records
 * the source and reads it while it is written) therefore needs nothing kept alive on the caller's side - which is the
 * difference between this and handing over borrowed memory.
 *
 * A buffer that was edited after it was handed over is refused (`IoError::InvalidData`), the way every borrowed source in
 * this library refuses one: the content it promised is no longer what it would produce. Replacing the buffer
 * (`setPositions` and the like) is not an edit - this source keeps reading the storage it was given.
 *
 * Threading: no method is thread-safe and there is no internal locking - a shared object, and storage that two objects
 * share, are synchronized by the caller; two sources over the same buffer may be pulled concurrently, since reading one
 * does not touch the other's cursor.
 *
 * @tparam T The element type; it has to be a plain type, since the bytes are the buffer's own layout.
 */
template <typename T>
class BufferSliceSource final : public DataSource
{
  public:
    /**
     * @brief Sources a whole buffer.
     *
     * @param buffer The buffer to write; the source holds it. A null buffer is reported through error() like every other
     *        defect of a source, so the caller sees it where all of them surface instead of crashing here.
     */
    explicit BufferSliceSource(vn::intrusive_ptr<const vn::Buffer<T>> buffer) : buffer_(std::move(buffer))
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "the bytes of a buffer are its elements, so it has to be a plain type");
        if (buffer_ == nullptr) {
            error_ = IoError::InvalidData;
            return;
        }
        revision_ = buffer_->revision();
    }

    /**
     * @brief Reports the length of the content.
     *
     * @return The buffer's byte count, known exactly, or 0 when the buffer was null.
     */
    [[nodiscard]] std::uint64_t size() const noexcept override
    {
        return buffer_ == nullptr ? 0 : buffer_->size() * sizeof(T);
    }

    /**
     * @brief Restarts the content from the buffer's first element.
     */
    void rewind() override { cursor_ = 0; }

    /**
     * @brief Writes the next chunk of the buffer's bytes.
     *
     * @param out Buffer to fill.
     * @return The number of bytes written, or 0 at the end of the content (see error()).
     */
    [[nodiscard]] std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty() || buffer_ == nullptr || error_ != IoError::Ok) {
            return 0;
        }
        if (buffer_->revision() != revision_) {
            error_ = IoError::InvalidData; // the buffer was edited after it was handed over
            return 0;
        }

        const std::size_t total = static_cast<std::size_t>(size());
        if (cursor_ >= total) {
            return 0;
        }
        const std::size_t take = std::min(out.size(), total - cursor_);
        std::memcpy(out.data(), reinterpret_cast<const std::byte*>(buffer_->data()) + cursor_, take);
        cursor_ += take;
        return take;
    }

    /**
     * @brief Reports a failure that read() cannot express.
     *
     * @return IoError::Ok while the buffer writes cleanly, IoError::InvalidData when it was null or was edited after it
     *         was handed over.
     */
    [[nodiscard]] IoError error() const override { return error_; }

  private:
    vn::intrusive_ptr<const vn::Buffer<T>> buffer_;
    std::uint64_t                          revision_{ 0 };
    std::size_t                            cursor_{ 0 };
    IoError                                error_{ IoError::Ok };
};

VN_IO_NS_END
