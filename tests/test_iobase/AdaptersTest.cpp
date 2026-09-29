#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <vine/MemoryStream.hpp>
#include <vine/io/Adapters.hpp>
#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/ZipArchive.hpp>

#include "VfsTestSupport.hpp"

using vn::io::BufferSliceSource;
using vn::io::DataSource;
using vn::io::DirectoryVfs;
using vn::io::Fragment;
using vn::io::IoError;
using vn::io::IstreamSource;
using vn::io::kUnknownSize;
using vn::io::OstreamSink;
using vn::io::VfsEntrySource;
using vn::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::TempDir;

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
    /**
     * @brief Serves one byte without a get area.
     *
     * @return The next byte, or eof.
     */
    int_type underflow() override
    {
        if (pos_ >= text_.size()) {
            return traits_type::eof();
        }
        return traits_type::to_int_type(text_[pos_]);
    }

    /**
     * @brief Serves a block, which is what std::istream::read goes through.
     *
     * @param out Buffer to fill.
     * @param count Bytes to serve.
     * @return The number of bytes served, short at the end.
     */
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
 * @brief A source that stops after four bytes and says whether that was a failure.
 */
class ShortSource final : public DataSource
{
  public:
    /**
     * @brief Promises @p promised bytes and delivers four of them.
     *
     * @param promised Length this source claims, so a consumer can see it fall short.
     * @param reason What error() answers once it stopped, IoError::Ok for a silent short stop.
     */
    ShortSource(std::uint64_t promised, IoError reason) : promised_(promised), reason_(reason) {}

    std::uint64_t size() const override { return promised_; }

    void rewind() override
    {
        produced_ = 0;
        stopped_  = false;
    }

    std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty()) {
            return 0;
        }
        if (produced_ >= kDelivered) {
            stopped_ = true;
            return 0;
        }
        const std::size_t got = std::min<std::size_t>(kDelivered - produced_, out.size());
        produced_ += got;
        return got;
    }

    IoError error() const override { return stopped_ ? reason_ : IoError::Ok; }

  private:
    static constexpr std::size_t kDelivered = 4;
    std::uint64_t                promised_{ 0 };
    IoError                      reason_{ IoError::Ok };
    std::size_t                  produced_{ 0 };
    bool                         stopped_{ false };
};

/**
 * @brief A source that produces a payload but refuses to say how long it is.
 *
 * It reports kUnknownSize, so the only way to find the end is to pull until it returns nothing - and with
 * @p broken_with set it does exactly that halfway through, that time with a reason attached.
 */
class UnmeasuredSource final : public DataSource
{
  public:
    /**
     * @brief Builds a source over the given payload.
     *
     * @param payload The bytes to produce.
     * @param broken_with The reason to report after half the payload, or IoError::Ok to deliver all of it.
     */
    explicit UnmeasuredSource(std::string payload, IoError broken_with = IoError::Ok)
        : payload_(std::move(payload)), broken_with_(broken_with)
    {
    }

    std::uint64_t size() const override { return kUnknownSize; }

    void rewind() override
    {
        position_ = 0;
        stopped_  = false;
    }

    std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty()) {
            return 0;
        }
        const std::size_t limit = broken_with_ == IoError::Ok ? payload_.size() : payload_.size() / 2;
        if (position_ >= limit) {
            stopped_ = true;
            return 0; // the only way a source without a stated length can report the end
        }
        const std::size_t got = std::min(out.size(), limit - position_);
        std::memcpy(out.data(), payload_.data() + position_, got);
        position_ += got;
        return got;
    }

    IoError error() const override { return stopped_ ? broken_with_ : IoError::Ok; }

  private:
    std::string payload_;
    IoError     broken_with_{ IoError::Ok };
    std::size_t position_{ 0 };
    bool        stopped_{ false };
};

/**
 * @brief A source that reports how much of its content is already on disk while it is pulled.
 *
 * A backend that materializes the content first leaves nothing on disk until the pull is over; one that streams grows
 * the file as the bytes are produced. Recording the largest size seen during the pull is what tells the two apart.
 */
class WatchingSource final : public DataSource
{
  public:
    /**
     * @brief Builds a source that watches a directory while it produces its payload.
     *
     * @param directory The directory to watch for content appearing.
     * @param payload The bytes to produce.
     */
    WatchingSource(std::filesystem::path directory, std::string payload)
        : directory_(std::move(directory)), payload_(std::move(payload))
    {
    }

    std::uint64_t size() const override { return kUnknownSize; }

    void rewind() override { position_ = 0; }

    std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty() || position_ >= payload_.size()) {
            return 0;
        }

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
            const std::uintmax_t size = entry.is_regular_file(ec) ? std::filesystem::file_size(entry.path(), ec) : 0;
            largest_on_disk_          = std::max(largest_on_disk_, static_cast<std::uint64_t>(size));
        }

        const std::size_t got = std::min(out.size(), payload_.size() - position_);
        std::memcpy(out.data(), payload_.data() + position_, got);
        position_ += got;
        return got;
    }

    /**
     * @brief Returns the largest amount of content seen on disk while pulling.
     *
     * @return That size in bytes, 0 when nothing was ever there.
     */
    [[nodiscard]] std::uint64_t largestOnDisk() const noexcept { return largest_on_disk_; }

  private:
    std::filesystem::path directory_;
    std::string           payload_;
    std::size_t           position_{ 0 };
    std::uint64_t         largest_on_disk_{ 0 };
};

/**
 * @brief Reads a reader to its end, the way its callers do.
 *
 * @param source The reader to consume.
 * @return Every byte it produced, in order.
 */
std::vector<unsigned char> drain(VfsEntrySource& source)
{
    std::vector<unsigned char>  out;
    std::array<std::byte, 1024> buffer{};
    for (std::size_t got = 0; (got = source.read(buffer)) != 0;) {
        out.insert(out.end(), reinterpret_cast<const unsigned char*>(buffer.data()),
                   reinterpret_cast<const unsigned char*>(buffer.data()) + got);
    }
    return out;
}

/**
 * @brief Builds a package with one entry and then wrecks that entry's data area.
 *
 * The trailing central directory stays intact, so the archive still opens and lists - only the content is damaged.
 *
 * @param target File to write the damaged package to.
 * @param damaged The content the entry was built from.
 */
void writeDamagedPackage(const std::filesystem::path& target, std::span<const unsigned char> damaged)
{
    {
        ZipArchive archive;
        EXPECT_EQ(archive.addFile(u8"mesh.bin", damaged), IoError::Ok);
        EXPECT_EQ(archive.saveAs(target), IoError::Ok);
    }
    std::fstream file(target, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.good());
    file.seekg(0, std::ios::end);
    const std::streamoff size = static_cast<std::streamoff>(file.tellg());
    ASSERT_GT(size, 256);
    const std::vector<char> garbage(64, static_cast<char>(0xAA));
    file.seekp(size / 2);
    file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    ASSERT_TRUE(file.good());
}

} // namespace

TEST(AdaptersTest, IstreamSourcesPullsTheStreamIntoThePackage)
{
    const TempDir dir;

    std::string payload;
    payload.reserve(300 * 1024);
    for (std::size_t i = 0; i < 300 * 1024; ++i) {
        payload.push_back(static_cast<char>((i * 31 + 7) % 251));
    }

    std::istringstream in(payload, std::ios::binary);
    auto               source = std::make_shared<IstreamSource>(in, payload.size());
    EXPECT_EQ(source->size(), payload.size());

    // One source, two packages: every pull restarts it (rewind()), which is what makes one
    // source able to feed more than one save.
    for (const char* name : { "first.zip", "second.zip" }) {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"blob.bin", source), IoError::Ok);
        ASSERT_EQ(archive.saveAs(dir.path() / name), IoError::Ok);

        const auto opened = ZipArchive::open(dir.path() / name, ZipArchive::OpenMode::ReadOnly);
        ASSERT_TRUE(opened.ok());
        const auto bytes = opened->read(u8"blob.bin");
        ASSERT_TRUE(bytes.ok());
        ASSERT_EQ(bytes->size(), payload.size());
        EXPECT_EQ(std::memcmp(bytes->data(), payload.data(), payload.size()), 0) << name;
    }
}

TEST(AdaptersTest, IstreamSourceMeasuresAStreamOfUnknownLength)
{
    const std::string payload = "measured content, not a byte more";
    std::istringstream in(payload, std::ios::binary);

    auto source = std::make_shared<IstreamSource>(in, kUnknownSize);
    EXPECT_EQ(source->error(), IoError::Ok);
    EXPECT_EQ(source->size(), payload.size());

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"text.bin", source), IoError::Ok);
    const auto bytes = archive.read(u8"text.bin"); // an entry that was never written is pulled here
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), payload);
}

TEST(AdaptersTest, IstreamSourceRefusesAStreamThatCannotSeek)
{
    PipeBuf      buffer("a pipe cannot restart");
    std::istream pipe(&buffer);

    // The length cannot be measured without seek ...
    auto unmeasured = std::make_shared<IstreamSource>(pipe, kUnknownSize);
    EXPECT_EQ(unmeasured->size(), 0u);
    EXPECT_EQ(unmeasured->error(), IoError::Unsupported);

    // ... and a source is pulled again for every save, so one that cannot restart is refused too.
    auto restarted = std::make_shared<IstreamSource>(pipe, 22u);
    EXPECT_EQ(restarted->error(), IoError::Unsupported);

    ZipArchive archive;
    EXPECT_EQ(archive.addFile(u8"text.bin", restarted), IoError::Unsupported) << "a source that is already broken is refused";
}

TEST(AdaptersTest, IstreamSourceConvertsWhilePulling)
{
    std::vector<double> values;
    values.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        values.push_back(static_cast<double>(i) * 0.5);
    }
    std::vector<unsigned char> expected(values.size() * sizeof(float));
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto converted = static_cast<float>(values[i]);
        std::memcpy(expected.data() + i * sizeof(float), &converted, sizeof(float));
    }

    vn::InputSpanStream in(std::as_bytes(std::span<const double>(values)));
    IstreamSource      source(in, values.size(), vn::io::Conversion<double, float>{ [](auto from, auto to) {
        std::transform(from.begin(), from.end(), to.begin(), [](double value) { return static_cast<float>(value); });
    } });
    EXPECT_EQ(source.size(), expected.size());

    // Pulled three bytes at a time, so most calls land inside an element and the leftover has to be carried.
    std::vector<unsigned char> pulled;
    std::array<std::byte, 3>   tiny{};
    for (std::size_t got = 0; (got = source.read(tiny)) != 0;) {
        pulled.insert(pulled.end(), reinterpret_cast<const unsigned char*>(tiny.data()),
                      reinterpret_cast<const unsigned char*>(tiny.data()) + got);
    }
    EXPECT_EQ(source.error(), IoError::Ok);
    EXPECT_EQ(pulled, expected);

    // The same source feeds a package after a rewind, and the entry reads back converted.
    source.rewind();
    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"mesh.bin", std::make_shared<IstreamSource>(std::move(source))), IoError::Ok);
    const auto stored = archive.read(u8"mesh.bin");
    ASSERT_TRUE(stored.ok());
    ASSERT_EQ(stored->size(), expected.size());
    EXPECT_EQ(std::memcmp(stored->data(), expected.data(), expected.size()), 0);
}

TEST(AdaptersTest, OstreamSinkConvertsWhilePushing)
{
    const std::vector<double> values{ 1.0, -2.5, 3.25, 4.125, 0.5 };
    std::vector<unsigned char> raw(values.size() * sizeof(double));
    std::memcpy(raw.data(), values.data(), raw.size());

    std::vector<unsigned char> expected(values.size() * sizeof(float));
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto converted = static_cast<float>(values[i]);
        std::memcpy(expected.data() + i * sizeof(float), &converted, sizeof(float));
    }

    std::ostringstream out(std::ios::binary);
    {
        OstreamSink sink(out, vn::io::Conversion<double, float>{ [](auto from, auto to) {
                            std::transform(from.begin(), from.end(), to.begin(),
                                           [](double value) { return static_cast<float>(value); });
                        } });

        // Pushed three bytes at a time, so a chunk boundary sits inside an element almost every call.
        for (std::size_t off = 0; off < raw.size(); off += 3) {
            const std::size_t count = std::min<std::size_t>(3, raw.size() - off);
            ASSERT_EQ(sink.write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data() + off), count)),
                      IoError::Ok);
        }
        EXPECT_EQ(sink.finalize(), IoError::Ok);
    }
    EXPECT_EQ(out.str(), std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));
}

TEST(AdaptersTest, OstreamSinkReportsContentThatEndsInsideAnElement)
{
    const std::array<std::byte, 7> partial{}; // seven bytes: not one whole double

    std::ostringstream converted_out;
    OstreamSink        converted(converted_out, vn::io::Conversion<double, float>{ [](auto, auto) {} });
    EXPECT_EQ(converted.write(partial), IoError::Ok);
    EXPECT_EQ(converted.finalize(), IoError::InvalidData) << "a half element is content that cannot be read";

    std::ostringstream plain_out;
    OstreamSink        plain(plain_out);
    EXPECT_EQ(plain.write(partial), IoError::Ok);
    EXPECT_EQ(plain.finalize(), IoError::Ok) << "without conversion there are no element boundaries";
}

TEST(AdaptersTest, CopyMovesAnEntryBetweenTrees)
{
    const TempDir dir;

    ZipArchive source;
    ASSERT_EQ(source.addFile(u8"geoms/base.bin", bytesOf("mesh-bytes-abcd")), IoError::Ok);
    ASSERT_EQ(source.addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);

    ZipArchive target;
    EXPECT_EQ(target.copy(source, u8"geoms", u8"x.bin"), IoError::IsADirectory);
    EXPECT_EQ(target.copy(source, u8"nope.bin", u8"x.bin"), IoError::NotFound);
    EXPECT_EQ(target.copy(source, u8"/bad", u8"x.bin"), IoError::InvalidPath);
    ASSERT_EQ(target.copy(source, u8"geoms/base.bin", u8"geoms/copied.bin"), IoError::Ok);

    const auto copied = target.read(u8"geoms/copied.bin");
    ASSERT_TRUE(copied.ok());
    EXPECT_EQ(std::string(copied->begin(), copied->end()), "mesh-bytes-abcd");

    // The same call into a real directory, which writes the bytes through its own writer.
    const auto folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);
    ASSERT_EQ(folder->copy(source, u8"workcell.xml", u8"workcell.xml"), IoError::Ok);
    const auto text = folder->read(u8"workcell.xml");
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(std::string(text->begin(), text->end()), "<workcell/>");
}

TEST(AdaptersTest, CopyOfDamagedContentFailsInsteadOfWritingIt)
{
    const TempDir dir;
    const auto    damaged_pkg = dir.path() / "damaged.zip";

    std::vector<unsigned char> payload(64 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31 + 7) % 251);
    }
    writeDamagedPackage(damaged_pkg, payload);

    const auto opened = ZipArchive::open(damaged_pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());

    // A backend that materializes learns the verdict while it is being fed, so nothing is left behind.
    const auto folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->copy(*opened, u8"mesh.bin", u8"mesh.bin"), IoError::IoFailure);
    EXPECT_FALSE(folder->exists(u8"mesh.bin")) << "content that did not check out must not be written";

    // A backend that keeps the source learns it when it is persisted instead.
    auto stream = opened->openRead(u8"mesh.bin");
    ASSERT_TRUE(stream.ok());
    ZipArchive packager;
    ASSERT_EQ(packager.addFile(u8"mesh.bin", std::shared_ptr<vn::io::DataSource>(std::move(*stream))), IoError::Ok);
    EXPECT_EQ(packager.saveAs(dir.path() / "broken.zip"), IoError::IoFailure);
}

TEST(AdaptersTest, BufferSliceSourceWritesTheBufferItHolds)
{
    auto buffer = vn::make_intrusive<vn::Buffer<float>>(std::vector<float>{ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f });

    const auto source = std::make_shared<BufferSliceSource<float>>(buffer);
    EXPECT_EQ(source->size(), 6u * sizeof(float));

    // Pulled in pieces that do not line up with an element, since nothing here looks at elements.
    std::vector<std::byte> pulled;
    std::array<std::byte, 5> piece{};
    for (std::size_t got = 0; (got = source->read(piece)) != 0;) {
        pulled.insert(pulled.end(), piece.begin(), piece.begin() + static_cast<std::ptrdiff_t>(got));
    }
    EXPECT_EQ(source->error(), IoError::Ok);
    ASSERT_EQ(pulled.size(), 6u * sizeof(float));
    EXPECT_EQ(std::memcmp(pulled.data(), buffer->data(), pulled.size()), 0) << "the bytes are the buffer's own";

    // The source holds the buffer, so dropping every other handle does not take the content away - which is what lets a
    // package that pulls at save time write geometry that nobody else keeps alive.
    EXPECT_EQ(buffer->useCount(), 2u) << "the source counts as a holder";
}

TEST(AdaptersTest, BufferSliceSourceRefusesABufferEditedAfterHandover)
{
    auto                                    buffer = vn::make_intrusive<vn::Buffer<float>>(std::vector<float>{ 1.0f, 2.0f, 3.0f });
    BufferSliceSource<float>                source(buffer);
    std::array<std::byte, 4>                piece{};

    ASSERT_EQ(source.read(piece), 4u);

    buffer->operator[](0) = 9.0f;
    buffer->bumpRevision();
    EXPECT_EQ(source.read(piece), 0u) << "the content it promised is no longer what it would produce";
    EXPECT_EQ(source.error(), IoError::InvalidData);
}

TEST(AdaptersTest, DirectoryVfsWritesASourceWhileItIsPulled)
{
    const TempDir dir;
    const auto    folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);

    const std::string payload(200 * 1024, 'x');
    auto              source = std::make_shared<WatchingSource>(dir.path(), payload);
    ASSERT_EQ(folder->addFile(u8"streamed.bin", source), IoError::Ok);

    EXPECT_GT(source->largestOnDisk(), 0u) << "the file has to grow while the source is pulled, not after it";
    EXPECT_LT(source->largestOnDisk(), payload.size()) << "and nothing may wait for the whole content";

    const auto stored = folder->read(u8"streamed.bin");
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(std::string(stored->begin(), stored->end()), payload);
}

TEST(AdaptersTest, DirectoryVfsPublishesNothingWhenAStreamingSourceFails)
{
    const TempDir dir;
    const auto    folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);
    ASSERT_EQ(folder->addFile(u8"blob.bin", bytesOf("the content that was there")), IoError::Ok);

    // The name is taken, so the replacement is refused before the source is pulled at all.
    EXPECT_EQ(folder->addFile(u8"blob.bin", std::make_shared<UnmeasuredSource>("replacement that stops", IoError::IoFailure)),
              IoError::AlreadyExists);
    const auto kept = folder->read(u8"blob.bin");
    ASSERT_TRUE(kept.ok());
    EXPECT_EQ(std::string(kept->begin(), kept->end()), "the content that was there");

    // A source that stops halfway never becomes a file: the bytes go through a sibling temporary that is dropped, so
    // half of the content is never published under the target name.
    EXPECT_EQ(folder->addFile(u8"new.bin", std::make_shared<UnmeasuredSource>("replacement that stops", IoError::IoFailure)),
              IoError::IoFailure);
    EXPECT_EQ(folder->read(u8"new.bin").error(), IoError::NotFound);
    EXPECT_EQ(folder->read(u8"new.bin.vine-tmp").error(), IoError::NotFound) << "the temporary is not left behind";
}

TEST(AdaptersTest, DirectoryVfsWritesBorrowedPiecesWithoutAssemblingThem)
{
    const TempDir dir;
    const auto    folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);

    const std::string first  = "0123456789";
    const std::string second = "abcdefghij";
    const std::array<Fragment, 3> pieces{
        Fragment{ reinterpret_cast<const unsigned char*>(first.data()), 4 },
        Fragment{ reinterpret_cast<const unsigned char*>(second.data()), 3 },
        Fragment{ reinterpret_cast<const unsigned char*>(first.data()) + 4, 6 },
    };

    ASSERT_EQ(folder->addFile(u8"pieces.bin", pieces), IoError::Ok);
    const auto stored = folder->read(u8"pieces.bin");
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(std::string(stored->begin(), stored->end()), "0123abc456789") << "the pieces are written in order";

    const std::array<Fragment, 1> broken{ Fragment{ nullptr, 4 } };
    EXPECT_EQ(folder->addFile(u8"broken.bin", broken), IoError::InvalidData)
        << "a piece that claims bytes it does not have is refused";
    EXPECT_FALSE(folder->exists(u8"broken.bin"));
}

TEST(AdaptersTest, AddFileTakesASourceThatCannotStateItsLength)
{
    const TempDir     dir;
    const std::string payload = "content nobody measured, and it ends where it ends";

    // A backend that materializes reads it right here, into a buffer that grows until the source stops.
    const auto folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);
    ASSERT_EQ(folder->addFile(u8"unmeasured.bin", std::make_shared<UnmeasuredSource>(payload)), IoError::Ok);
    const auto stored = folder->read(u8"unmeasured.bin");
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(std::string(stored->begin(), stored->end()), payload);

    // A backend that defers the pull hands the source to libzip, which stores the entry with a zip64 header
    // and patches the length in once the last byte has arrived - the local header is written first.
    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"unmeasured.bin", std::make_shared<UnmeasuredSource>(payload)), IoError::Ok);
    const auto before = archive.stat(u8"unmeasured.bin");
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before->size, kUnknownSize) << "nothing has been produced yet, so there is no length to report";
    ASSERT_EQ(archive.saveAs(dir.path() / "unmeasured.zip"), IoError::Ok);

    const auto opened = ZipArchive::open(dir.path() / "unmeasured.zip", ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto after = opened->stat(u8"unmeasured.bin");
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->size, payload.size()) << "the archive holds the length it learned while writing";
    const auto bytes = opened->read(u8"unmeasured.bin");
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), payload);
}

TEST(AdaptersTest, AddFileReportsWhyAnUnmeasuredSourceStoppedShort)
{
    const TempDir     dir;
    const std::string payload = "this one gives up in the middle";

    // A backend that materializes learns the verdict while it is being fed, so nothing is left behind.
    const auto folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(folder->addFile(u8"broken.bin", std::make_shared<UnmeasuredSource>(payload, IoError::IoFailure)),
              IoError::IoFailure);
    EXPECT_FALSE(folder->exists(u8"broken.bin")) << "content that did not check out must not be written";

    // A backend that keeps the source learns it when the source is pulled - here, and then at saveAs().
    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"broken.bin", std::make_shared<UnmeasuredSource>(payload, IoError::IoFailure)), IoError::Ok);
    EXPECT_EQ(archive.read(u8"broken.bin").error(), IoError::IoFailure);
    EXPECT_EQ(archive.saveAs(dir.path() / "broken.zip"), IoError::IoFailure);
}

TEST(AdaptersTest, AddFileReportsWhyASourceStoppedShort)
{
    const TempDir dir;
    const auto    folder = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(folder, nullptr);

    // The same short stop, told apart by what the source says afterwards.
    EXPECT_EQ(folder->addFile(u8"short.bin", std::make_shared<ShortSource>(10u, IoError::Ok)), IoError::InvalidData);
    EXPECT_EQ(folder->addFile(u8"failed.bin", std::make_shared<ShortSource>(10u, IoError::IoFailure)), IoError::IoFailure);
    EXPECT_FALSE(folder->exists(u8"short.bin"));
    EXPECT_FALSE(folder->exists(u8"failed.bin"));
}

TEST(AdaptersTest, VfsEntrySourceRestartsFromTheBeginning)
{
    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"text.bin", bytesOf("0123456789")), IoError::Ok);

    auto source = archive.openRead(u8"text.bin");
    ASSERT_TRUE(source.ok());
    EXPECT_EQ((*source)->size(), 10u);

    std::array<std::byte, 4> head{};
    ASSERT_EQ((*source)->read(head), 4u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(head.data()), 4), "0123");

    (*source)->rewind(); // the DataSource half: back to the first byte
    const auto whole = drain(**source);
    EXPECT_EQ(std::string(whole.begin(), whole.end()), "0123456789");
    EXPECT_EQ((*source)->error(), IoError::Ok);
}

TEST(AdaptersTest, RestartsACompressedEntryOfASavedPackage)
{
    const TempDir dir;

    // Long enough, and repetitive enough, that the archive stores it compressed - which is the case that cannot be
    // seeked: libzip refuses a seek on a compressed entry, and a refused seek leaves its reader unusable afterwards.
    std::string payload;
    for (int i = 0; i < 400; ++i) {
        payload += "0123456789abcdef";
    }

    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"blob.bin", bytesOf(payload)), IoError::Ok);
        ASSERT_EQ(archive.saveAs(dir.path() / "blob.zip"), IoError::Ok);
    }

    const auto opened = ZipArchive::open(dir.path() / "blob.zip", ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    auto source = opened->openRead(u8"blob.bin");
    ASSERT_TRUE(source.ok());
    EXPECT_EQ((*source)->size(), payload.size());
    ASSERT_FALSE((*source)->seekable())
        << "the fixture has to be compressed, or this test says nothing about the reopen path";

    std::array<std::byte, 64> head{};
    ASSERT_EQ((*source)->read(head), 64u);

    // A restart is a reopen behind the scenes, and it has to come back with the whole entry and no error left behind.
    (*source)->rewind();
    const auto whole = drain(**source);
    EXPECT_EQ(std::string(whole.begin(), whole.end()), payload);
    EXPECT_EQ((*source)->error(), IoError::Ok) << "restarting a compressed entry leaves no error behind";
}
