#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <iterator>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <vector>

#include <vine/io/Adapters.hpp>
#include <vine/io/DataSourceStream.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/ZipArchive.hpp>

#include "VfsTestSupport.hpp"

using vn::io::DataSource;
using vn::io::DataSourceStream;
using vn::io::IoError;
using vn::io::IstreamSource;
using vn::io::ZipArchive;
using vfstest::bytesOf;

namespace
{

/**
 * @brief A stream buffer that offers no seek, the way a pipe or socket does.
 */
class PipeBuf : public std::streambuf
{
  public:
    /**
     * @brief Feeds the given text, one read at a time.
     *
     * @param text The bytes this buffer serves.
     */
    explicit PipeBuf(std::string text) : text_(std::move(text)) {}

  protected:
    int_type underflow() override
    {
        if (pos_ >= text_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(text_[pos_]);
    }

    std::streamsize xsgetn(char* out, std::streamsize count) override
    {
        const std::streamsize available = static_cast<std::streamsize>(text_.size() - pos_);
        const std::streamsize take      = std::min(count, available);
        std::memcpy(out, text_.data() + pos_, static_cast<std::size_t>(take));
        pos_ += static_cast<std::size_t>(take);
        return take;
    }

  private:
    std::string text_;
    std::size_t pos_{ 0 };
};

/**
 * @brief A source that promises ten bytes, delivers four, and says that it failed.
 */
class FailingSource final : public DataSource
{
  public:
    std::uint64_t size() const override { return 10; }

    void rewind() override { produced_ = 0; }

    std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty() || produced_ >= kDelivered) {
            return 0; // the four bytes are everything it can produce of the ten it promised
        }
        const std::size_t got = std::min<std::size_t>(kDelivered - produced_, out.size());
        std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(got), std::byte{ 0x41 });
        produced_ += got;
        return got;
    }

    /// Once it has handed over its fourth byte it knows the remaining six will never come.
    IoError error() const override { return produced_ >= kDelivered ? IoError::IoFailure : IoError::Ok; }

  private:
    static constexpr std::size_t kDelivered = 4;
    std::size_t                  produced_{ 0 };
};

/**
 * @brief Reads a stream to its end.
 *
 * @param in The stream to read.
 * @return Every byte it produced, in order.
 */
std::string readAll(std::istream& in)
{
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(DataSourceStreamTest, ReadsAnEntryThroughAStream)
{
    std::string payload(300 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 17 + 3) % 251); // a NUL byte included on purpose
    }

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"blob.bin", bytesOf(payload)), IoError::Ok);

    auto entry = archive.openRead(u8"blob.bin");
    ASSERT_TRUE(entry.ok());

    DataSourceStream in(**entry);
    EXPECT_EQ(in.size(), payload.size());

    // A bulk read, on purpose: it is what a parser does, and it goes through one source call per chunk.
    std::string got(payload.size(), '\0');
    in.read(got.data(), static_cast<std::streamsize>(got.size()));
    EXPECT_EQ(got, payload);
    EXPECT_EQ(in.error(), IoError::Ok);
}

TEST(DataSourceStreamTest, SeeksBackToTheStart)
{
    const std::string payload = "0123456789";

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"text.bin", bytesOf(payload)), IoError::Ok);
    auto entry = archive.openRead(u8"text.bin");
    ASSERT_TRUE(entry.ok());

    DataSourceStream in(**entry);
    std::array<char, 4> head{};
    in.read(head.data(), 4);
    ASSERT_EQ(std::string(head.data(), 4), "0123");
    EXPECT_EQ(in.tellg(), std::streampos(4));

    in.seekg(0);
    ASSERT_TRUE(in.good()) << "a source states a size and a restart, so going back to the start works";
    EXPECT_EQ(readAll(in), payload);
    EXPECT_EQ(in.tellg(), std::streampos(payload.size()));
}

TEST(DataSourceStreamTest, HandsAStagedByteToTheNextRead)
{
    const std::string payload = "abcdefghij";

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"text.bin", bytesOf(payload)), IoError::Ok);
    auto entry = archive.openRead(u8"text.bin");
    ASSERT_TRUE(entry.ok());

    DataSourceStream in(**entry);
    const int        first = in.peek(); // stages one byte without consuming it
    ASSERT_EQ(first, 'a');

    std::string all(payload.size(), '\0');
    in.read(all.data(), static_cast<std::streamsize>(all.size()));
    EXPECT_EQ(all, payload) << "a byte the stream already fetched belongs to the next read as well";
}

TEST(DataSourceStreamTest, ReadsTextLineByLine)
{
    const std::string payload = "first\nsecond\nthird\n";

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"list.txt", bytesOf(payload)), IoError::Ok);
    auto entry = archive.openRead(u8"list.txt");
    ASSERT_TRUE(entry.ok());

    DataSourceStream in(**entry);
    std::string      line;
    ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
    EXPECT_EQ(line, "first");
    ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
    EXPECT_EQ(line, "second");
    ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
    EXPECT_EQ(line, "third");
}

TEST(DataSourceStreamTest, ReportsAStreamFailureBehindTheEnd)
{
    FailingSource source;
    {
        DataSourceStream in(source);
        EXPECT_EQ(in.size(), 10u);

        // A read, not an iterator: only a stream operation turns a short read into eofbit plus a gcount.
        std::string got(16, '\0');
        in.read(got.data(), static_cast<std::streamsize>(got.size()));
        EXPECT_EQ(in.gcount(), 4) << "the source delivered four of the ten bytes it promised";
        EXPECT_TRUE(in.eof());
        EXPECT_EQ(in.error(), IoError::IoFailure) << "a std::istream cannot carry the reason, so it is asked for here";
    }

    // A clean source reports a clean end instead, which is what tells the two apart.
    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"text.bin", bytesOf("AAAA")), IoError::Ok);
    auto entry = archive.openRead(u8"text.bin");
    ASSERT_TRUE(entry.ok());
    DataSourceStream clean(**entry);
    std::string      whole(16, '\0');
    clean.read(whole.data(), static_cast<std::streamsize>(whole.size()));
    EXPECT_EQ(clean.gcount(), 4) << "a clean source ends where its content ends";
    EXPECT_TRUE(clean.eof());
    EXPECT_EQ(clean.error(), IoError::Ok);
}

TEST(DataSourceStreamTest, RefusesToSeekASourceThatCannotRestart)
{
    PipeBuf      buffer("a pipe cannot restart");
    std::istream pipe(&buffer);

    IstreamSource source(pipe, 22);
    ASSERT_EQ(source.error(), IoError::Unsupported);

    DataSourceStream in(source);
    EXPECT_EQ(in.error(), IoError::Unsupported) << "the source could not be rewound for the stream to start";
    EXPECT_EQ(readAll(in), "") << "a stream over a source that cannot deliver reads nothing";

    in.seekg(0);
    EXPECT_TRUE(in.fail()) << "seeking a source that cannot restart fails instead of reading nothing quietly";
}
