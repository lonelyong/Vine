#include <vine/io/DataSourceStream.hpp>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <streambuf>

VN_IO_NS_BEGIN

DataSourceStreamBuf::DataSourceStreamBuf(DataSource& source) noexcept
  : source_(&source)
{
    // A stream describes one piece of content from its beginning, so the source is rewound here. A source that cannot
    // restart leaves the stream empty and reports why through error().
    source_->rewind();
    if (source_->error() != IoError::Ok) {
        error_ = source_->error();
    }
}

DataSourceStreamBuf::~DataSourceStreamBuf() = default;

std::streambuf::int_type DataSourceStreamBuf::underflow()
{
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    if (error_ != IoError::Ok) {
        return traits_type::eof();
    }

    const std::size_t got = source_->read(std::span<std::byte>(&current_, 1));
    if (got == 0) {
        if (source_->error() != IoError::Ok) {
            error_ = source_->error();
        }
        return traits_type::eof();
    }

    position_ += 1;
    char* const first = reinterpret_cast<char*>(&current_);
    setg(first, first, first + 1);
    return traits_type::to_int_type(*first);
}

std::streamsize DataSourceStreamBuf::xsgetn(char* out, std::streamsize count)
{
    if (count <= 0) {
        return 0;
    }
    if (error_ != IoError::Ok) {
        return 0;
    }

    std::streamsize done = 0;
    if (gptr() < egptr()) {
        // A byte underflow() fetched and the reader has not taken yet: it belongs to this read as well.
        *out = *gptr();
        gbump(1);
        done = 1;
        if (done == count) {
            return done;
        }
    }

    const std::size_t got = source_->read(
        std::span<std::byte>(reinterpret_cast<std::byte*>(out + done), static_cast<std::size_t>(count - done)));
    if (got == 0 && source_->error() != IoError::Ok) {
        error_ = source_->error();
    }
    position_ += got;
    return done + static_cast<std::streamsize>(got);
}

std::streambuf::pos_type DataSourceStreamBuf::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
{
    if ((which & std::ios_base::in) == 0) {
        return pos_type(off_type(-1));
    }

    if (dir == std::ios_base::beg && off == 0) {
        source_->rewind();
        if (source_->error() != IoError::Ok) {
            error_ = source_->error();
            return pos_type(off_type(-1)); // a source that cannot restart cannot be seeked
        }
        setg(nullptr, nullptr, nullptr);
        position_ = 0;
        error_    = IoError::Ok;
        return pos_type(0);
    }

    if (dir == std::ios_base::cur && off == 0) {
        // The position of the next byte the reader will get: what the source produced minus what is still staged here.
        const auto staged = static_cast<std::uint64_t>(egptr() - gptr());
        return pos_type(static_cast<off_type>(position_ - staged));
    }

    return pos_type(off_type(-1)); // a source states a size and a restart, not arbitrary positions
}

std::streambuf::pos_type DataSourceStreamBuf::seekpos(pos_type pos, std::ios_base::openmode which)
{
    return seekoff(static_cast<off_type>(pos), std::ios_base::beg, which);
}

std::uint64_t DataSourceStreamBuf::size() const noexcept
{
    return source_->size();
}

IoError DataSourceStreamBuf::error() const noexcept
{
    // The source is the authority on its own state: a failure it has already recorded is reported even when this
    // stream never saw read() return 0 for it (a short read leaves the stream at eof without asking again).
    if (error_ != IoError::Ok) {
        return error_;
    }
    return source_->error();
}

DataSourceStream::DataSourceStream(DataSource& source)
  : std::istream(nullptr)
  , buf_(source)
{
    init(&buf_);
}

DataSourceStream::~DataSourceStream() = default;

std::uint64_t DataSourceStream::size() const noexcept
{
    return buf_.size();
}

IoError DataSourceStream::error() const
{
    return buf_.error();
}

VN_IO_NS_END
