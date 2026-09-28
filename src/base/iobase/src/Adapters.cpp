#include <vine/io/Adapters.hpp>

#include <algorithm>
#include <cstring>
#include <ios>
#include <utility>
#include <vector>

VN_IO_NS_BEGIN

namespace
{

/**
 * @brief Elements one pull or push moves before returning: a chunk of work, never a whole entry.
 */
constexpr std::size_t kElementsPerChunk = 4096;

/**
 * @brief Reads exactly @p count bytes, or as many as the stream delivers.
 *
 * A stream that stops before the count is an error rather than an end: the source promised a length, so a short read
 * means the content is not what it was described as.
 *
 * @param in The stream to read.
 * @param out Buffer to fill.
 * @param count Bytes to read.
 * @param error Receives IoError::IoFailure when the stream stopped short.
 * @return The number of bytes read.
 */
std::size_t readExact(std::istream& in, std::byte* out, std::size_t count, IoError& error)
{
    std::size_t done = 0;
    while (done < count) {
        in.read(reinterpret_cast<char*>(out) + done, static_cast<std::streamsize>(count - done));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            error = IoError::IoFailure;
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

/**
 * @brief Turns a possibly kUnknownSize element count into the stream's own count.
 *
 * Measuring needs a seekable stream, and the element size has to divide the length: content that holds half an element
 * cannot be read as elements at all.
 *
 * @param in The stream to measure; its position is left at the start.
 * @param count Elements the caller stated, or kUnknownSize to measure.
 * @param in_element Bytes of one element.
 * @param error Receives the reason when the count cannot be settled.
 * @return The element count, or 0 when it cannot be settled (see @p error).
 */
std::uint64_t measureElements(std::istream& in, std::uint64_t count, std::size_t in_element, IoError& error)
{
    // A source is pulled again for every save (rewind()), so a stream that cannot seek cannot be one.
    in.clear();
    in.seekg(0);
    if (!in) {
        error = IoError::Unsupported;
        return 0;
    }
    if (count != kUnknownSize) {
        return count;
    }

    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) {
        error = IoError::Unsupported; // a stream that cannot be measured cannot state a length
        return 0;
    }
    in.clear();
    in.seekg(0);
    if (!in) {
        error = IoError::IoFailure;
        return 0;
    }

    const auto bytes = static_cast<std::uint64_t>(end);
    if (bytes % in_element != 0) {
        error = IoError::InvalidData; // the content is not a whole number of elements
        return 0;
    }
    return bytes / in_element;
}

} // namespace

struct IstreamSource::Impl
{
    std::istream* in{ nullptr };
    std::uint64_t size{ 0 };     ///< Bytes this source produces, after conversion.
    std::uint64_t produced{ 0 }; ///< Bytes produced since the last rewind().
    std::size_t   in_element{ 1 };
    std::size_t   out_element{ 1 };
    IoError       setup_error{ IoError::Ok }; ///< What the constructor settled: a bad setup survives every rewind().
    IoError       error{ IoError::Ok };
    std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert;
    std::vector<std::byte> scratch;     ///< Input side of one converting chunk.
    std::vector<std::byte> pending;     ///< One element's output the caller's buffer could not take whole.
    std::size_t            pending_off{ 0 };
};

IstreamSource::IstreamSource(std::istream& in, std::uint64_t size)
  : IstreamSource(in, size, 1, 1, nullptr)
{
}

IstreamSource::IstreamSource(std::istream& in, std::uint64_t count, std::size_t in_element, std::size_t out_element,
                             std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert)
  : impl(std::make_unique<Impl>())
{
    impl->in          = &in;
    impl->in_element  = in_element == 0 ? 1 : in_element;
    impl->out_element = out_element == 0 ? 1 : out_element;
    impl->convert     = std::move(convert);

    const std::uint64_t elements = measureElements(in, count, impl->in_element, impl->setup_error);
    impl->error                  = impl->setup_error;
    if (impl->setup_error != IoError::Ok) {
        return; // the length could not be settled: size stays 0 and error() says why
    }
    if (elements > kUnknownSize / impl->out_element) {
        impl->setup_error = IoError::InvalidData; // a length no storage could hold
        impl->error       = impl->setup_error;
        return;
    }
    impl->size = elements * impl->out_element;
}

IstreamSource::~IstreamSource() = default;
IstreamSource::IstreamSource(IstreamSource&&) noexcept = default;
IstreamSource& IstreamSource::operator=(IstreamSource&&) noexcept = default;

std::uint64_t IstreamSource::size() const noexcept
{
    return impl->size;
}

void IstreamSource::rewind()
{
    impl->produced    = 0;
    impl->pending_off = 0;
    impl->pending.clear();
    impl->error = impl->setup_error;

    impl->in->clear();
    impl->in->seekg(0);
    if (!*impl->in && impl->error == IoError::Ok) {
        impl->error = IoError::IoFailure; // a stream that cannot seek cannot be pulled twice
    }
}

std::size_t IstreamSource::read(std::span<std::byte> out)
{
    if (out.empty() || impl->error != IoError::Ok) {
        return 0;
    }

    Impl&       s       = *impl;
    std::size_t written = 0;

    // Whatever is left of an element the previous call could not take whole.
    if (s.pending_off < s.pending.size()) {
        const std::size_t take = std::min(out.size(), s.pending.size() - s.pending_off);
        std::memcpy(out.data(), s.pending.data() + s.pending_off, take);
        s.pending_off += take;
        written += take;
        if (s.pending_off < s.pending.size() || written == out.size()) {
            return written;
        }
        s.pending.clear();
        s.pending_off = 0;
    }

    // A plain copy: the caller's own buffer is what the stream fills.
    if (s.convert == nullptr) {
        const std::uint64_t left = s.size - s.produced;
        const std::size_t   want = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - written, left));
        if (want == 0) {
            return written;
        }
        const std::size_t got = readExact(*s.in, out.data() + written, want, s.error);
        s.produced += got;
        return written + got;
    }

    // Converted: whole input elements in, whole output elements out.
    while (written < out.size() && s.produced < s.size) {
        const std::size_t room     = out.size() - written;
        std::size_t       elements = room / s.out_element;
        if (elements == 0) {
            // Room for part of one element only: convert a single element aside, hand over
            // what fits, and keep the rest for the next call.
            s.scratch.resize(s.in_element);
            if (readExact(*s.in, s.scratch.data(), s.in_element, s.error) < s.in_element) {
                return written;
            }
            s.pending.resize(s.out_element);
            s.convert(std::span<const std::byte>(s.scratch), std::span<std::byte>(s.pending));
            s.produced += s.out_element;
            const std::size_t take = std::min(room, s.pending.size());
            std::memcpy(out.data() + written, s.pending.data(), take);
            s.pending_off = take;
            return written + take;
        }

        const std::uint64_t left_elements = (s.size - s.produced) / s.out_element;
        elements = static_cast<std::size_t>(std::min<std::uint64_t>(elements, left_elements));
        elements = std::min(elements, kElementsPerChunk);
        const std::size_t in_bytes = elements * s.in_element;
        s.scratch.resize(in_bytes);
        if (readExact(*s.in, s.scratch.data(), in_bytes, s.error) < in_bytes) {
            return written;
        }
        s.convert(std::span<const std::byte>(s.scratch),
                  std::span<std::byte>(out.data() + written, elements * s.out_element));
        s.produced += elements * s.out_element;
        written += elements * s.out_element;
    }
    return written;
}

IoError IstreamSource::error() const
{
    return impl->error;
}

struct OstreamSink::Impl
{
    std::ostream* out{ nullptr };
    std::size_t   in_element{ 1 };
    std::size_t   out_element{ 1 };
    std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert;
    std::vector<std::byte> remainder; ///< Bytes of an element the last chunk did not complete.
    std::vector<std::byte> scratch;   ///< Output side of one converting chunk.
};

OstreamSink::OstreamSink(std::ostream& out)
  : OstreamSink(out, 1, 1, nullptr)
{
}

OstreamSink::OstreamSink(std::ostream& out, std::size_t in_element, std::size_t out_element,
                         std::function<void(std::span<const std::byte>, std::span<std::byte>)> convert)
  : impl(std::make_unique<Impl>())
{
    impl->out         = &out;
    impl->in_element  = in_element == 0 ? 1 : in_element;
    impl->out_element = out_element == 0 ? 1 : out_element;
    impl->convert     = std::move(convert);
    if (impl->convert != nullptr) {
        impl->remainder.reserve(impl->in_element);
    }
}

OstreamSink::~OstreamSink() = default;
OstreamSink::OstreamSink(OstreamSink&&) noexcept = default;
OstreamSink& OstreamSink::operator=(OstreamSink&&) noexcept = default;

IoError OstreamSink::write(std::span<const std::byte> bytes)
{
    Impl& s = *impl;

    // A plain copy: the chunk goes to the stream as it is.
    if (s.convert == nullptr) {
        if (!bytes.empty()) {
            s.out->write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        return *s.out ? IoError::Ok : IoError::IoFailure;
    }

    // Converted: the chunk is appended to whatever an earlier chunk left of an element, and
    // only whole elements are converted - a chunk boundary is no element boundary.
    s.remainder.insert(s.remainder.end(), bytes.begin(), bytes.end());
    std::size_t consumed = 0;
    while (s.remainder.size() - consumed >= s.in_element) {
        const std::size_t elements = std::min((s.remainder.size() - consumed) / s.in_element, kElementsPerChunk);
        s.scratch.resize(elements * s.out_element);
        s.convert(std::span<const std::byte>(s.remainder.data() + consumed, elements * s.in_element),
                  std::span<std::byte>(s.scratch));
        s.out->write(reinterpret_cast<const char*>(s.scratch.data()), static_cast<std::streamsize>(s.scratch.size()));
        if (!*s.out) {
            return IoError::IoFailure;
        }
        consumed += elements * s.in_element;
    }
    if (consumed != 0) {
        s.remainder.erase(s.remainder.begin(), s.remainder.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return IoError::Ok;
}

IoError OstreamSink::finalize()
{
    Impl& s = *impl;
    if (s.convert != nullptr && !s.remainder.empty()) {
        return IoError::InvalidData; // the content ended inside an element
    }
    s.out->flush();
    return *s.out ? IoError::Ok : IoError::IoFailure;
}

VN_IO_NS_END
