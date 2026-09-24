#include <vine/MemoryStream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

VN_CORE_NS_BEGIN

MemoryStreamBuf::MemoryStreamBuf() = default;

MemoryStreamBuf::MemoryStreamBuf(std::span<const std::byte> src)
{
    reset(src);
}

MemoryStreamBuf::MemoryStreamBuf(std::vector<std::byte>&& src)
  : buffer_(std::move(src))
  , size_(buffer_.size())
{
    setPointers(0, size_);
}

MemoryStreamBuf::MemoryStreamBuf(MemoryStreamBuf&& other) noexcept
  : buffer_(std::move(other.buffer_))
  , size_(other.size_)
{
    setPointers(0, size_);
    other.resetState();
}

MemoryStreamBuf& MemoryStreamBuf::operator=(MemoryStreamBuf&& other) noexcept
{
    if (this != &other) {
        buffer_ = std::move(other.buffer_);
        size_   = other.size_;
        setPointers(0, size_);
        other.resetState();
    }
    return *this;
}

MemoryStreamBuf::~MemoryStreamBuf() = default;

const std::byte* MemoryStreamBuf::data() const noexcept
{
    return buffer_.data();
}

std::size_t MemoryStreamBuf::size() const noexcept
{
    const std::size_t written = writePosition();
    return written > size_ ? written : size_;
}

std::vector<std::byte> MemoryStreamBuf::release()
{
    // Hand the storage over instead of copying it out. resize() can only shrink
    // here, because the logical size never exceeds the capacity, so the address
    // the caller may already hold stays the same.
    buffer_.resize(size());
    std::vector<std::byte> result = std::exchange(buffer_, std::vector<std::byte>{});
    resetState();
    return result;
}

void MemoryStreamBuf::clearData() noexcept
{
    size_ = 0;
    setPointers(0, 0);
}

void MemoryStreamBuf::reset(std::span<const std::byte> src)
{
    buffer_.clear();
    size_ = 0;
    if (!src.empty()) {
        buffer_.assign(src.begin(), src.end());
        size_ = src.size();
    }
    // The storage was just replaced, so every cached pointer is stale and
    // must be rebuilt from scratch rather than revalidated.
    setPointers(0, size_);
}

std::streambuf::int_type MemoryStreamBuf::underflow()
{
    syncGetArea();
    if (gptr() != nullptr && gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    return traits_type::eof();
}

std::streambuf::int_type MemoryStreamBuf::overflow(std::streambuf::int_type ch)
{
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
        return traits_type::eof();
    }

    const std::size_t pos = writePosition();
    grow(pos + 1);
    buffer_.data()[pos] = static_cast<std::byte>(ch);
    if (pos + 1 > size_) {
        size_ = pos + 1;
    }
    setPointers(readPosition(), pos + 1);
    return traits_type::not_eof(ch);
}

std::streamsize MemoryStreamBuf::xsputn(const char* s, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }

    const std::size_t pos = writePosition();
    const auto        n   = static_cast<std::size_t>(count);
    grow(pos + n);
    std::memcpy(buffer_.data() + pos, s, n);
    if (pos + n > size_) {
        size_ = pos + n;
    }
    setPointers(readPosition(), pos + n);
    return count;
}

std::streamsize MemoryStreamBuf::xsgetn(char* s, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }
    // The end of the get area is refreshed lazily, because put-area writes
    // move the put pointer without going through overflow()/xsputn().
    syncGetArea();
    if (gptr() == nullptr || egptr() == nullptr) {
        return 0;
    }

    const auto avail = static_cast<std::size_t>(egptr() - gptr());
    const auto n     = std::min(avail, static_cast<std::size_t>(count));
    if (n > 0) {
        std::memcpy(s, gptr(), n);
        advanceGet(n);
    }
    return static_cast<std::streamsize>(n);
}

std::streambuf::pos_type MemoryStreamBuf::seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
{
    pos_type result{ off_type(-1) };

    // Direct put-area writes are folded into the logical size before any
    // position is derived from it.
    const std::size_t len = size();
    size_                 = len;

    if (which & std::ios_base::in) {
        if (buffer_.empty()) {
            // An empty buffer has exactly one valid position: the start.
            if (off == 0 && len == 0) {
                result = pos_type(0);
            }
        }
        else {
            char* base   = reinterpret_cast<char*>(buffer_.data());
            char* target = nullptr;
            switch (dir) {
            case std::ios_base::beg: target = base + off; break;
            case std::ios_base::cur: target = gptr() + off; break;
            case std::ios_base::end: target = base + len + off; break;
            default: return result;
            }
            if (target >= base && target <= base + len) {
                setg(base, target, base + len);
                result = pos_type(target - base);
            }
        }
    }

    if (which & std::ios_base::out) {
        // grow() may reallocate, so every offset is taken as a value first.
        const std::size_t    read_off  = readPosition();
        const auto           end_off   = static_cast<std::ptrdiff_t>(len);
        const std::ptrdiff_t write_off = pptr() != nullptr ? pptr() - reinterpret_cast<char*>(buffer_.data()) : end_off;

        std::ptrdiff_t target = 0;
        switch (dir) {
        case std::ios_base::beg: target = off; break;
        case std::ios_base::cur: target = write_off + off; break;
        case std::ios_base::end: target = end_off + off; break;
        default: return result;
        }
        if (target < 0) {
            return result;
        }

        const auto pos = static_cast<std::size_t>(target);
        grow(pos);
        if (pos > size_) {
            // Seeking past the logical end opens a hole; report it as zeros.
            std::memset(reinterpret_cast<char*>(buffer_.data()) + size_, 0, pos - size_);
            size_ = pos;
        }
        setPointers(read_off, pos);
        if (!(which & std::ios_base::in)) {
            result = pos_type(pos);
        }
    }

    return result;
}

std::streambuf::pos_type MemoryStreamBuf::seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which)
{
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

/**
 * @brief Returns the offset the next read happens at.
 *
 * @return Offset from the start of the data.
 */
std::size_t MemoryStreamBuf::readPosition() const noexcept
{
    if (gptr() == nullptr || eback() == nullptr) {
        return size_;
    }
    return static_cast<std::size_t>(gptr() - eback());
}

/**
 * @brief Returns the offset the next write happens at.
 *
 * @return Offset from the start of the data.
 */
std::size_t MemoryStreamBuf::writePosition() const noexcept
{
    if (pptr() != nullptr && pbase() != nullptr) {
        return static_cast<std::size_t>(pptr() - pbase());
    }
    return size_;
}

/**
 * @brief Extends the get area to cover everything written so far.
 *
 * The put area spans the whole capacity, so a write made directly through it, as `sputc()` does, leaves `egptr()` where it was; the end is refreshed here, just
 * before a read, rather than on every write.
 */
void MemoryStreamBuf::syncGetArea() noexcept
{
    if (gptr() == nullptr || buffer_.empty()) {
        return;
    }
    char* base = reinterpret_cast<char*>(buffer_.data());
    char* end  = base + size();
    if (end != egptr()) {
        setg(base, gptr(), end);
    }
}

/**
 * @brief Grows the physical capacity to hold at least the given size.
 *
 * @param needed Bytes of capacity required from the start of the data.
 */
void MemoryStreamBuf::grow(std::size_t needed)
{
    if (needed <= buffer_.size()) {
        return;
    }

    std::size_t new_cap = buffer_.size() == 0 ? s_minCapacity : buffer_.size();
    while (new_cap < needed) {
        new_cap *= 2;
    }

    // resize() may move the storage, so the offsets are captured as values.
    const std::size_t read_off  = readPosition();
    const std::size_t write_off = writePosition();
    if (write_off > size_) {
        // Fold in put-area writes that never reached overflow()/xsputn().
        size_ = write_off;
    }

    buffer_.resize(new_cap);
    setPointers(read_off, write_off);
}

/**
 * @brief Publishes fresh get and put areas over the current storage.
 *
 * Must be used whenever the storage moved, because it rebuilds every cached pointer instead of revalidating one.
 *
 * @param read_off Offset the next read starts at.
 * @param write_off Offset the next write starts at.
 */
void MemoryStreamBuf::setPointers(std::size_t read_off, std::size_t write_off) noexcept
{
    if (buffer_.empty()) {
        setg(nullptr, nullptr, nullptr);
        setp(nullptr, nullptr);
        return;
    }

    char* base = reinterpret_cast<char*>(buffer_.data());
    if (read_off > size_) {
        read_off = size_;
    }
    if (write_off > buffer_.size()) {
        write_off = buffer_.size();
    }
    setg(base, base + read_off, base + size_);
    setp(base, base + buffer_.size());
    advancePut(write_off);
}

/**
 * @brief Advances the put pointer by the given offset.
 *
 * Must be called with `pptr() == pbase()`, which is the state `setp()` leaves behind.
 *
 * @param offset Offset from the start of the put area.
 */
void MemoryStreamBuf::advancePut(std::size_t offset) noexcept
{
    // pbump() takes an int, so offsets beyond INT_MAX have to be stepped.
    while (offset > static_cast<std::size_t>(s_maxBump)) {
        pbump(s_maxBump);
        offset -= static_cast<std::size_t>(s_maxBump);
    }
    pbump(static_cast<int>(offset));
}

/**
 * @brief Advances the get pointer by the given count.
 *
 * @param count Number of bytes just consumed.
 */
void MemoryStreamBuf::advanceGet(std::size_t count) noexcept
{
    // gbump() takes an int, so counts beyond INT_MAX have to be stepped.
    while (count > static_cast<std::size_t>(s_maxBump)) {
        gbump(s_maxBump);
        count -= static_cast<std::size_t>(s_maxBump);
    }
    gbump(static_cast<int>(count));
}

/**
 * @brief Drops all state, leaving an empty buffer behind.
 */
void MemoryStreamBuf::resetState() noexcept
{
    buffer_.clear();
    size_ = 0;
    setg(nullptr, nullptr, nullptr);
    setp(nullptr, nullptr);
}

InputMemoryStream::InputMemoryStream()
  : std::istream(&buf_)
{}

InputMemoryStream::InputMemoryStream(std::span<const std::byte> data)
  : std::istream(&buf_)
  , buf_(data)
{}

InputMemoryStream::InputMemoryStream(std::vector<std::byte>&& data)
  : std::istream(&buf_)
  , buf_(std::move(data))
{}

InputMemoryStream::InputMemoryStream(InputMemoryStream&& other) noexcept
  : std::istream(&buf_)
  , buf_(std::move(other.buf_))
{}

InputMemoryStream& InputMemoryStream::operator=(InputMemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* InputMemoryStream::data() const noexcept
{
    return buf_.data();
}

std::size_t InputMemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<std::byte> InputMemoryStream::release()
{
    return buf_.release();
}

OutputMemoryStream::OutputMemoryStream()
  : std::ostream(&buf_)
{}

OutputMemoryStream::OutputMemoryStream(OutputMemoryStream&& other) noexcept
  : std::ostream(&buf_)
  , buf_(std::move(other.buf_))
{}

OutputMemoryStream& OutputMemoryStream::operator=(OutputMemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* OutputMemoryStream::data() const noexcept
{
    return buf_.data();
}

std::size_t OutputMemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<std::byte> OutputMemoryStream::release()
{
    return buf_.release();
}

void OutputMemoryStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

MemoryStream::MemoryStream()
  : std::iostream(&buf_)
{}

MemoryStream::MemoryStream(std::span<const std::byte> data)
  : std::iostream(&buf_)
  , buf_(data)
{}

MemoryStream::MemoryStream(std::vector<std::byte>&& data)
  : std::iostream(&buf_)
  , buf_(std::move(data))
{}

MemoryStream::MemoryStream(MemoryStream&& other) noexcept
  : std::iostream(&buf_)
  , buf_(std::move(other.buf_))
{}

MemoryStream& MemoryStream::operator=(MemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* MemoryStream::data() const noexcept
{
    return buf_.data();
}

std::size_t MemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<std::byte> MemoryStream::release()
{
    return buf_.release();
}

void MemoryStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

void MemoryStream::reset(std::span<const std::byte> data)
{
    buf_.reset(data);
    this->std::ios::clear();
}

ChunkedMemoryStreamBuf::ChunkedMemoryStreamBuf(std::size_t chunk_size)
  : chunk_size_(chunk_size == 0 ? s_defaultChunkSize : chunk_size)
{
    // The put area is never published: every write goes through
    // overflow()/xsputn(), which append to the chunk chain directly.
    setp(nullptr, nullptr);
}

ChunkedMemoryStreamBuf::ChunkedMemoryStreamBuf(std::span<const std::byte> src, std::size_t chunk_size)
  : chunk_size_(chunk_size == 0 ? s_defaultChunkSize : chunk_size)
{
    setp(nullptr, nullptr);
    appendRaw(src.data(), src.size());
}

ChunkedMemoryStreamBuf::ChunkedMemoryStreamBuf(ChunkedMemoryStreamBuf&& other) noexcept
  : chunk_size_(other.chunk_size_)
  , chunks_(std::move(other.chunks_))
  , read_chunk_idx_(other.read_chunk_idx_)
  , active_chunk_start_(other.active_chunk_start_)
  , read_pos_(other.readPosition())
  , coalesced_(std::move(other.coalesced_))
  , coalesced_valid_(other.coalesced_valid_)
  , size_(other.size_)
{
    setp(nullptr, nullptr);
    // The chunks keep their storage, so resuming the reader is a pointer
    // publication at the position captured above.
    if (other.gptr() != nullptr) {
        republishGetArea();
    }
    other.resetState();
}

ChunkedMemoryStreamBuf& ChunkedMemoryStreamBuf::operator=(ChunkedMemoryStreamBuf&& other) noexcept
{
    if (this != &other) {
        chunk_size_         = other.chunk_size_;
        chunks_             = std::move(other.chunks_);
        read_chunk_idx_     = other.read_chunk_idx_;
        active_chunk_start_ = other.active_chunk_start_;
        read_pos_           = other.readPosition();
        coalesced_          = std::move(other.coalesced_);
        coalesced_valid_    = other.coalesced_valid_;
        size_               = other.size_;

        setp(nullptr, nullptr);
        detachGetArea();
        if (other.gptr() != nullptr) {
            republishGetArea();
        }
        other.resetState();
    }
    return *this;
}

ChunkedMemoryStreamBuf::~ChunkedMemoryStreamBuf() = default;

const std::byte* ChunkedMemoryStreamBuf::data() const
{
    ensureCoalesced();
    return coalesced_.data();
}

std::size_t ChunkedMemoryStreamBuf::size() const noexcept
{
    return size_;
}

std::vector<ChunkedMemoryStreamBuf::BufferPart> ChunkedMemoryStreamBuf::chunks() const
{
    std::vector<BufferPart> parts;
    parts.reserve(chunks_.size());
    std::size_t remaining = size_;
    for (const auto& chunk : chunks_) {
        if (remaining == 0) {
            break;
        }
        const std::size_t n = std::min(chunk.size(), remaining);
        parts.push_back({ reinterpret_cast<const std::byte*>(chunk.data()), n });
        remaining -= n;
    }
    return parts;
}

std::vector<std::byte> ChunkedMemoryStreamBuf::release()
{
    std::vector<std::byte> result(size_);
    std::size_t            offset    = 0;
    std::size_t            remaining = size_;
    for (const auto& chunk : chunks_) {
        if (remaining == 0) {
            break;
        }
        const std::size_t n = std::min(chunk.size(), remaining);
        std::memcpy(result.data() + offset, chunk.data(), n);
        offset += n;
        remaining -= n;
    }
    clearData();
    return result;
}

void ChunkedMemoryStreamBuf::clearData() noexcept
{
    chunks_.clear();
    coalesced_.clear();
    coalesced_valid_ = false;
    size_            = 0;
    resetState();
}

std::streambuf::int_type ChunkedMemoryStreamBuf::underflow()
{
    if (gptr() != nullptr && gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    if (!activateGetArea()) {
        return traits_type::eof();
    }
    return traits_type::to_int_type(*gptr());
}

std::streambuf::int_type ChunkedMemoryStreamBuf::uflow()
{
    if (gptr() == nullptr || gptr() >= egptr()) {
        if (!activateGetArea()) {
            return traits_type::eof();
        }
    }
    const int_type ch = traits_type::to_int_type(*gptr());
    gbump(1);
    return ch;
}

std::streambuf::int_type ChunkedMemoryStreamBuf::overflow(std::streambuf::int_type ch)
{
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
        return traits_type::eof();
    }
    const std::byte byte = static_cast<std::byte>(ch);
    appendRaw(&byte, 1);
    return traits_type::not_eof(ch);
}

std::streamsize ChunkedMemoryStreamBuf::xsputn(const char* s, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }
    appendRaw(reinterpret_cast<const std::byte*>(s), static_cast<std::size_t>(count));
    return count;
}

std::streamsize ChunkedMemoryStreamBuf::xsgetn(char* s, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }

    std::streamsize total = 0;
    while (total < count) {
        if (gptr() == nullptr || gptr() >= egptr()) {
            if (!activateGetArea()) {
                break;
            }
        }
        const auto avail = static_cast<std::size_t>(egptr() - gptr());
        const auto n     = std::min(avail, static_cast<std::size_t>(count - total));
        std::memcpy(s + total, gptr(), n);
        advanceGet(n);
        total += static_cast<std::streamsize>(n);
    }
    return total;
}

std::streambuf::pos_type ChunkedMemoryStreamBuf::seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
{
    if ((which & std::ios_base::out) && !(which & std::ios_base::in)) {
        return pos_type(off_type(-1));
    }

    const auto cur = static_cast<std::ptrdiff_t>(readPosition());
    const auto end = static_cast<std::ptrdiff_t>(size_);

    std::ptrdiff_t target = 0;
    switch (dir) {
    case std::ios_base::beg: target = off; break;
    case std::ios_base::cur: target = cur + off; break;
    case std::ios_base::end: target = end + off; break;
    default: return pos_type(off_type(-1));
    }
    if (target < 0 || static_cast<std::size_t>(target) > size_) {
        return pos_type(off_type(-1));
    }

    // The get area is a cache of read_pos_, never its source: drop it first
    // so the new offset is what the next read resumes from.
    detachGetArea();
    read_pos_ = static_cast<std::size_t>(target);
    republishGetArea();
    return pos_type(read_pos_);
}

std::streambuf::pos_type ChunkedMemoryStreamBuf::seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which)
{
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

/**
 * @brief Appends bytes to the chunk chain.
 *
 * @param src Bytes to append.
 * @param len Number of bytes to append.
 */
void ChunkedMemoryStreamBuf::appendRaw(const std::byte* src, std::size_t len)
{
    if (len == 0) {
        return;
    }

    // Appending can reallocate the chunk that currently backs the get area,
    // which would leave gptr()/egptr() dangling. The read position is
    // therefore taken as a value and the area is rebuilt afterwards.
    const bool        reading  = gptr() != nullptr;
    const std::size_t read_pos = readPosition();
    detachGetArea();
    read_pos_ = read_pos;

    const char* p         = reinterpret_cast<const char*>(src);
    std::size_t remaining = len;
    while (remaining > 0) {
        if (chunks_.empty() || chunks_.back().size() >= chunk_size_) {
            chunks_.emplace_back();
            chunks_.back().reserve(chunk_size_);
        }
        auto&             last  = chunks_.back();
        const std::size_t space = chunk_size_ - last.size();
        const std::size_t n     = std::min(remaining, space);
        last.insert(last.end(), p, p + n);
        p += n;
        remaining -= n;
    }

    size_ += len;
    coalesced_valid_ = false;

    if (reading) {
        republishGetArea();
    }
}

/**
 * @brief Returns the offset the next read happens at.
 *
 * @return Offset from the start of the content.
 */
std::size_t ChunkedMemoryStreamBuf::readPosition() const noexcept
{
    if (gptr() == nullptr || eback() == nullptr) {
        return read_pos_;
    }
    return active_chunk_start_ + static_cast<std::size_t>(gptr() - eback());
}

/**
 * @brief Points the get area at the chunk holding the read position.
 *
 * @return true when a byte is available, false at the end of the content.
 */
bool ChunkedMemoryStreamBuf::activateGetArea() noexcept
{
    const std::size_t pos = readPosition();
    read_pos_             = pos;
    if (pos >= size_) {
        detachGetArea();
        return false;
    }

    // Reusing the current chunk keeps sequential reads at one publish per
    // chunk instead of one per byte.
    if (read_chunk_idx_ < chunks_.size()) {
        const auto& current = chunks_[read_chunk_idx_];
        if (pos >= active_chunk_start_ && pos < active_chunk_start_ + current.size()) {
            publishGetArea(current, pos - active_chunk_start_);
            return true;
        }
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
        const std::size_t next = start + chunks_[i].size();
        if (pos < next) {
            read_chunk_idx_     = i;
            active_chunk_start_ = start;
            publishGetArea(chunks_[i], pos - start);
            return true;
        }
        start = next;
    }

    detachGetArea();
    return false;
}

/**
 * @brief Rebuilds the get area after the chunk storage may have moved.
 */
void ChunkedMemoryStreamBuf::republishGetArea() noexcept
{
    if (read_pos_ >= size_) {
        detachGetArea();
        return;
    }
    activateGetArea();
}

/**
 * @brief Withdraws the get area, leaving `read_pos_` authoritative.
 */
void ChunkedMemoryStreamBuf::detachGetArea() noexcept
{
    setg(nullptr, nullptr, nullptr);
}

/**
 * @brief Publishes one chunk as the get area.
 *
 * @param chunk Chunk to expose.
 * @param in_chunk_off Offset inside the chunk the next read starts at.
 */
void ChunkedMemoryStreamBuf::publishGetArea(const std::vector<char>& chunk, std::size_t in_chunk_off) noexcept
{
    char* base = const_cast<char*>(chunk.data());
    setg(base, base + in_chunk_off, base + chunk.size());
}

/**
 * @brief Advances the get pointer by the given count.
 *
 * @param count Number of bytes just consumed.
 */
void ChunkedMemoryStreamBuf::advanceGet(std::size_t count) noexcept
{
    // gbump() takes an int, so counts beyond INT_MAX have to be stepped.
    while (count > static_cast<std::size_t>(s_maxBump)) {
        gbump(s_maxBump);
        count -= static_cast<std::size_t>(s_maxBump);
    }
    gbump(static_cast<int>(count));
}

/**
 * @brief Copies the chunks into one contiguous block.
 */
void ChunkedMemoryStreamBuf::ensureCoalesced() const
{
    if (coalesced_valid_) {
        return;
    }

    coalesced_.resize(size_);
    std::size_t offset    = 0;
    std::size_t remaining = size_;
    for (const auto& chunk : chunks_) {
        if (remaining == 0) {
            break;
        }
        const std::size_t n = std::min(chunk.size(), remaining);
        std::memcpy(coalesced_.data() + offset, chunk.data(), n);
        offset += n;
        remaining -= n;
    }
    coalesced_valid_ = true;
}

/**
 * @brief Drops all state, leaving an empty buffer behind.
 */
void ChunkedMemoryStreamBuf::resetState() noexcept
{
    chunks_.clear();
    coalesced_.clear();
    coalesced_valid_    = false;
    size_               = 0;
    read_chunk_idx_     = 0;
    active_chunk_start_ = 0;
    read_pos_           = 0;
    detachGetArea();
    setp(nullptr, nullptr);
}

InputChunkedMemoryStream::InputChunkedMemoryStream(std::span<const std::byte> data, std::size_t chunk_size)
  : std::istream(&buf_)
  , buf_(data, chunk_size)
{}

InputChunkedMemoryStream::InputChunkedMemoryStream(InputChunkedMemoryStream&& other) noexcept
  : std::istream(&buf_)
  , buf_(std::move(other.buf_))
{}

InputChunkedMemoryStream& InputChunkedMemoryStream::operator=(InputChunkedMemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* InputChunkedMemoryStream::data() const
{
    return buf_.data();
}

std::size_t InputChunkedMemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<ChunkedMemoryStreamBuf::BufferPart> InputChunkedMemoryStream::chunks() const
{
    return buf_.chunks();
}

std::vector<std::byte> InputChunkedMemoryStream::release()
{
    return buf_.release();
}

OutputChunkedMemoryStream::OutputChunkedMemoryStream(std::size_t chunk_size)
  : std::ostream(&buf_)
  , buf_(chunk_size)
{}

OutputChunkedMemoryStream::OutputChunkedMemoryStream(OutputChunkedMemoryStream&& other) noexcept
  : std::ostream(&buf_)
  , buf_(std::move(other.buf_))
{}

OutputChunkedMemoryStream& OutputChunkedMemoryStream::operator=(OutputChunkedMemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* OutputChunkedMemoryStream::data() const
{
    return buf_.data();
}

std::size_t OutputChunkedMemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<ChunkedMemoryStreamBuf::BufferPart> OutputChunkedMemoryStream::chunks() const
{
    return buf_.chunks();
}

std::vector<std::byte> OutputChunkedMemoryStream::release()
{
    return buf_.release();
}

void OutputChunkedMemoryStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

ChunkedMemoryStream::ChunkedMemoryStream(std::size_t chunk_size)
  : std::iostream(&buf_)
  , buf_(chunk_size)
{}

ChunkedMemoryStream::ChunkedMemoryStream(std::span<const std::byte> data, std::size_t chunk_size)
  : std::iostream(&buf_)
  , buf_(data, chunk_size)
{}

ChunkedMemoryStream::ChunkedMemoryStream(ChunkedMemoryStream&& other) noexcept
  : std::iostream(&buf_)
  , buf_(std::move(other.buf_))
{}

ChunkedMemoryStream& ChunkedMemoryStream::operator=(ChunkedMemoryStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* ChunkedMemoryStream::data() const
{
    return buf_.data();
}

std::size_t ChunkedMemoryStream::size() const noexcept
{
    return buf_.size();
}

std::vector<ChunkedMemoryStreamBuf::BufferPart> ChunkedMemoryStream::chunks() const
{
    return buf_.chunks();
}

std::vector<std::byte> ChunkedMemoryStream::release()
{
    return buf_.release();
}

void ChunkedMemoryStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

SpanStreamBuf::SpanStreamBuf() noexcept
{
    // The put area is never published: writes go through overflow()/xsputn(),
    // which keeps the written length exact and lets seekp() stay position-only.
    setp(nullptr, nullptr);
    setPointers(0, 0);
}

SpanStreamBuf::SpanStreamBuf(std::span<std::byte> window) noexcept
  : begin_(window.data())
  , capacity_(window.size())
  , put_limit_(window.size())
{
    // The put area is never published: writes go through overflow()/xsputn(),
    // which keeps the written length exact and lets seekp() stay position-only.
    setp(nullptr, nullptr);
    setPointers(0, 0);
}

SpanStreamBuf SpanStreamBuf::readOnly(std::span<const std::byte> content) noexcept
{
    SpanStreamBuf buf;
    buf.begin_    = content.data();
    buf.capacity_ = content.size();
    buf.size_     = content.size();
    // put_limit_ stays 0: a read-only view has no writable room at all, which is
    // what makes every write fail in overflow()/xsputn().
    buf.setPointers(0, 0);
    return buf;
}

SpanStreamBuf::SpanStreamBuf(SpanStreamBuf&& other) noexcept
  : begin_(other.begin_)
  , capacity_(other.capacity_)
  , size_(other.size_)
  , put_limit_(other.put_limit_)
{
    setp(nullptr, nullptr);
    setPointers(other.readPosition(), other.writePosition());
    other.resetState();
}

SpanStreamBuf& SpanStreamBuf::operator=(SpanStreamBuf&& other) noexcept
{
    if (this != &other) {
        const std::size_t read_off  = other.readPosition();
        const std::size_t write_off = other.writePosition();

        begin_     = other.begin_;
        capacity_  = other.capacity_;
        size_      = other.size_;
        put_limit_ = other.put_limit_;

        setPointers(read_off, write_off);
        other.resetState();
    }
    return *this;
}

SpanStreamBuf::~SpanStreamBuf() = default;

const std::byte* SpanStreamBuf::data() const noexcept
{
    return begin_;
}

std::size_t SpanStreamBuf::size() const noexcept
{
    return size_;
}

std::size_t SpanStreamBuf::capacity() const noexcept
{
    return capacity_;
}

void SpanStreamBuf::clearData() noexcept
{
    // A read-only view keeps its whole window readable; a writable one forgets
    // the written prefix so the window can be filled again.
    if (put_limit_ > 0) {
        size_ = 0;
    }
    setPointers(0, 0);
}

std::streambuf::int_type SpanStreamBuf::underflow()
{
    if (gptr() != nullptr && gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    return traits_type::eof();
}

std::streambuf::int_type SpanStreamBuf::overflow(std::streambuf::int_type ch)
{
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
        return traits_type::eof();
    }
    if (put_off_ >= put_limit_) {
        // No writable room left, either because the window is full or because
        // this is a read-only view: report the overflow instead of finding room.
        return traits_type::eof();
    }

    // Only reachable with writable room, where the window really is mutable; a
    // read-only view is spelled with a const pointer and never reaches here.
    const_cast<std::byte*>(begin_)[put_off_] = static_cast<std::byte>(ch);
    ++put_off_;
    if (put_off_ > size_) {
        size_ = put_off_;
    }
    setPointers(readPosition(), put_off_);
    return traits_type::not_eof(ch);
}

std::streamsize SpanStreamBuf::xsputn(const char* s, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }

    const std::size_t space = put_limit_ > put_off_ ? put_limit_ - put_off_ : 0;
    const auto        n     = std::min(space, static_cast<std::size_t>(count));
    if (n > 0) {
        std::memcpy(const_cast<std::byte*>(begin_) + put_off_, s, n);
        put_off_ += n;
        if (put_off_ > size_) {
            size_ = put_off_;
        }
        setPointers(readPosition(), put_off_);
    }
    return static_cast<std::streamsize>(n);
}

std::streamsize SpanStreamBuf::xsgetn(char* s, std::streamsize count)
{
    if (count <= 0 || gptr() == nullptr || egptr() == nullptr) {
        return 0;
    }

    const auto avail = static_cast<std::size_t>(egptr() - gptr());
    const auto n     = std::min(avail, static_cast<std::size_t>(count));
    if (n > 0) {
        std::memcpy(s, gptr(), n);
        advanceGet(n);
    }
    return static_cast<std::streamsize>(n);
}

std::streambuf::pos_type SpanStreamBuf::seekoff(std::streambuf::off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
{
    pos_type result{ off_type(-1) };

    if (which & std::ios_base::in) {
        if (capacity_ == 0) {
            // An empty window has exactly one valid position: the start.
            if (off == 0) {
                result = pos_type(0);
            }
        }
        else {
            // The get area is spelled with mutable pointers even for a read-only
            // view; nothing ever writes through them.
            char* base   = const_cast<char*>(reinterpret_cast<const char*>(begin_));
            char* target = nullptr;
            switch (dir) {
            case std::ios_base::beg: target = base + off; break;
            case std::ios_base::cur: target = gptr() + off; break;
            case std::ios_base::end: target = base + size_ + off; break;
            default: return result;
            }
            // Reads cannot reach past what was written.
            if (target >= base && target <= base + size_) {
                setg(base, target, base + size_);
                result = pos_type(target - base);
            }
        }
    }

    if (which & std::ios_base::out) {
        const std::size_t    read_off  = readPosition();
        const std::ptrdiff_t write_off = static_cast<std::ptrdiff_t>(put_off_);

        std::ptrdiff_t target = 0;
        switch (dir) {
        case std::ios_base::beg: target = off; break;
        case std::ios_base::cur: target = write_off + off; break;
        case std::ios_base::end: target = static_cast<std::ptrdiff_t>(size_) + off; break;
        default: return result;
        }
        // Writes may reach the end of the writable room, but no further.
        if (target < 0 || static_cast<std::size_t>(target) > put_limit_) {
            return result;
        }

        // Moving the write position writes nothing and leaves size() alone, so
        // the bytes that are jumped over keep whatever the caller left there.
        setPointers(read_off, static_cast<std::size_t>(target));
        if (!(which & std::ios_base::in)) {
            result = pos_type(target);
        }
    }

    return result;
}

std::streambuf::pos_type SpanStreamBuf::seekpos(std::streambuf::pos_type pos, std::ios_base::openmode which)
{
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

/**
 * @brief Returns the offset the next read happens at.
 *
 * @return Offset from the start of the window.
 */
std::size_t SpanStreamBuf::readPosition() const noexcept
{
    if (gptr() == nullptr || eback() == nullptr) {
        return size_;
    }
    return static_cast<std::size_t>(gptr() - eback());
}

/**
 * @brief Returns the offset the next write happens at.
 *
 * @return Offset from the start of the window.
 */
std::size_t SpanStreamBuf::writePosition() const noexcept
{
    return put_off_;
}

/**
 * @brief Publishes the get area and stores the write position.
 *
 * @param read_off Offset the next read starts at.
 * @param write_off Offset the next write starts at.
 */
void SpanStreamBuf::setPointers(std::size_t read_off, std::size_t write_off) noexcept
{
    put_off_ = write_off > put_limit_ ? put_limit_ : write_off;

    if (begin_ == nullptr || capacity_ == 0) {
        setg(nullptr, nullptr, nullptr);
        return;
    }

    // The streambuf get area is spelled with mutable pointers even for a
    // read-only view; nothing ever writes through them.
    char* base = const_cast<char*>(reinterpret_cast<const char*>(begin_));
    if (read_off > size_) {
        read_off = size_;
    }
    setg(base, base + read_off, base + size_);
}

/**
 * @brief Advances the get pointer by the given count.
 *
 * @param count Number of bytes just consumed.
 */
void SpanStreamBuf::advanceGet(std::size_t count) noexcept
{
    // gbump() takes an int, so counts beyond INT_MAX have to be stepped.
    while (count > static_cast<std::size_t>(s_maxBump)) {
        gbump(s_maxBump);
        count -= static_cast<std::size_t>(s_maxBump);
    }
    gbump(static_cast<int>(count));
}

/**
 * @brief Drops all state, leaving an empty buffer behind.
 */
void SpanStreamBuf::resetState() noexcept
{
    begin_     = nullptr;
    capacity_  = 0;
    size_      = 0;
    put_limit_ = 0;
    put_off_   = 0;
    setg(nullptr, nullptr, nullptr);
    setp(nullptr, nullptr);
}

InputSpanStream::InputSpanStream(std::span<const std::byte> content) noexcept
  : std::istream(&buf_)
  , buf_(SpanStreamBuf::readOnly(content))
{}

InputSpanStream::InputSpanStream(InputSpanStream&& other) noexcept
  : std::istream(&buf_)
  , buf_(std::move(other.buf_))
{}

InputSpanStream& InputSpanStream::operator=(InputSpanStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* InputSpanStream::data() const noexcept
{
    return buf_.data();
}

std::size_t InputSpanStream::size() const noexcept
{
    return buf_.size();
}

OutputSpanStream::OutputSpanStream(std::span<std::byte> window) noexcept
  : std::ostream(&buf_)
  , buf_(window)
{}

OutputSpanStream::OutputSpanStream(OutputSpanStream&& other) noexcept
  : std::ostream(&buf_)
  , buf_(std::move(other.buf_))
{}

OutputSpanStream& OutputSpanStream::operator=(OutputSpanStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* OutputSpanStream::data() const noexcept
{
    return buf_.data();
}

std::size_t OutputSpanStream::size() const noexcept
{
    return buf_.size();
}

std::size_t OutputSpanStream::capacity() const noexcept
{
    return buf_.capacity();
}

void OutputSpanStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

SpanStream::SpanStream(std::span<std::byte> window) noexcept
  : std::iostream(&buf_)
  , buf_(window)
{}

SpanStream::SpanStream(SpanStream&& other) noexcept
  : std::iostream(&buf_)
  , buf_(std::move(other.buf_))
{}

SpanStream& SpanStream::operator=(SpanStream&& other) noexcept
{
    if (this != &other) {
        this->std::ios::clear();
        buf_ = std::move(other.buf_);
        rdbuf(&buf_);
    }
    return *this;
}

const std::byte* SpanStream::data() const noexcept
{
    return buf_.data();
}

std::size_t SpanStream::size() const noexcept
{
    return buf_.size();
}

std::size_t SpanStream::capacity() const noexcept
{
    return buf_.capacity();
}

void SpanStream::clearData() noexcept
{
    buf_.clearData();
    this->std::ios::clear();
}

VN_CORE_NS_END
