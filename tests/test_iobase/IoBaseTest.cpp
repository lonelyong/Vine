#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vine/String.hpp>
#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/Zip.hpp>
#include <vine/io/ZipArchive.hpp>

using vn::io::DirectoryVfs;
using vn::io::Zip;
using vn::io::ZipArchive;

namespace
{

/**
 * @brief Creates a unique temporary directory that is removed on destruction.
 */
class TempDir
{
  public:
    TempDir()
    {
        static std::atomic<unsigned long long> counter{ 0 };
        std::error_code                        ec;
        path_ = std::filesystem::temp_directory_path(ec) /
                ("vine_iobase_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/**
 * @brief Reports whether an archive index carries an entry with the given path.
 */
bool hasEntry(const std::vector<vn::io::VfsEntryInfo>& entries, const std::filesystem::path& path)
{
    return std::any_of(entries.begin(), entries.end(),
                       [&path](const vn::io::VfsEntryInfo& info) { return info.path == path; });
}

/**
 * @brief Views a string as the bytes a VFS write takes.
 */
std::span<const unsigned char> asBytes(const std::string& text)
{
    return std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

TEST(IoBaseTest, CompressRoundTrip)
{
    const std::string text = "The quick brown fox jumps over the lazy dog. ";
    std::string       payload;
    for (int i = 0; i < 50; ++i) {
        payload += text;
    }

    const auto compressed = Zip::compress(asBytes(payload));
    ASSERT_TRUE(compressed.ok());
    EXPECT_LT(compressed->size(), payload.size());

    const auto decompressed = Zip::decompress(*compressed);
    ASSERT_TRUE(decompressed.ok());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(decompressed->data()), decompressed->size()), payload);
}

TEST(IoBaseTest, DecompressRejectsGarbage)
{
    const unsigned char garbage[] = { 0xFF, 0xFE, 0x00, 0x01, 0x02 };
    EXPECT_EQ(Zip::decompress(std::span<const unsigned char>(garbage, sizeof(garbage))).error(),
              vn::io::IoError::InvalidData);
}

TEST(IoBaseTest, ZipEntryRoundTrip)
{
    TempDir                 dir;
    const std::filesystem::path zip_path = dir.path() / "single.zip";
    const std::string           content  = "hello zip world";

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"greeting.txt", asBytes(content)), vn::io::IoError::Ok);
    ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);

    const auto out = Zip::readEntry(zip_path, u8"greeting.txt");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(std::string(out->begin(), out->end()), content);
}

TEST(IoBaseTest, ZipReadEntryReportsFailures)
{
    TempDir                     dir;
    const std::filesystem::path zip_path = dir.path() / "read.zip";
    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"a.txt", asBytes("x")), vn::io::IoError::Ok);
        ASSERT_EQ(archive.createDirectory(u8"empty"), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);
    }

    EXPECT_EQ(Zip::readEntry(zip_path, u8"nope.txt").error(), vn::io::IoError::NotFound);
    EXPECT_EQ(Zip::readEntry(zip_path, u8"").error(), vn::io::IoError::InvalidPath);
    EXPECT_EQ(Zip::readEntry(zip_path, u8"empty/").error(), vn::io::IoError::IsADirectory);

    const std::vector<unsigned char> garbage{ 'n', 'o', 't', 'a', 'z', 'i', 'p' };
    EXPECT_EQ(Zip::readEntry(garbage, u8"a.txt").error(), vn::io::IoError::InvalidData);
}

TEST(IoBaseTest, ZipReadEntryVerifiesTheChecksum)
{
    TempDir    dir;
    const auto pkg = dir.path() / "checked.zip";

    // Incompressible content, so the entry is stored rather than deflated: flipping
    // bytes in it then leaves the stream readable, and only the checksum the
    // directory recorded can tell that the content changed.
    std::vector<unsigned char> payload(64 * 1024);
    std::uint32_t              state = 0x12345678;
    for (unsigned char& byte : payload) {
        state = state * 1664525u + 1013904223u;
        byte  = static_cast<unsigned char>(state >> 24);
    }
    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"mesh.bin", payload), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(pkg), vn::io::IoError::Ok);
    }

    const auto intact = Zip::readEntry(pkg, u8"mesh.bin");
    ASSERT_TRUE(intact.ok());
    EXPECT_EQ(intact->size(), payload.size());

    // Wreck the entry data but keep the trailing central directory intact.
    {
        std::fstream file(pkg, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekg(0, std::ios::end);
        const std::streamoff size = static_cast<std::streamoff>(file.tellg());
        ASSERT_GT(size, 256);
        const std::vector<char> garbage(64, static_cast<char>(0xAA));
        file.seekp(size / 2);
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        ASSERT_TRUE(file.good());
    }

    // The one-shot read refuses it, the same way an opened archive does.
    EXPECT_EQ(Zip::readEntry(pkg, u8"mesh.bin").error(), vn::io::IoError::IoFailure);
}

TEST(IoBaseTest, ZipArchiveAddDirectoryRoundTrip)
{
    TempDir                 dir;
    const std::filesystem::path source   = dir.path() / "src";
    const std::filesystem::path dest     = dir.path() / "dest";
    const std::filesystem::path zip_path = dir.path() / "dir.zip";
    std::filesystem::create_directories(source / "sub");
    {
        std::ofstream f1(source / "a.txt");
        f1 << "alpha";
        std::ofstream f2(source / "sub" / "b.txt");
        f2 << "beta";
    }

    ZipArchive archive;
    ASSERT_EQ(archive.addDirectory(std::filesystem::path{}, source), vn::io::IoError::Ok);
    ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);

    const auto entries = Zip::entries(zip_path);
    ASSERT_TRUE(entries.ok());
    EXPECT_TRUE(hasEntry(entries.value(), std::filesystem::path(u8"a.txt")));
    EXPECT_TRUE(hasEntry(entries.value(), std::filesystem::path(u8"sub/b.txt")));

    ASSERT_EQ(Zip::decompressFile(zip_path, dest), vn::io::IoError::Ok);
    std::ifstream in(dest / "sub" / "b.txt");
    ASSERT_TRUE(in.good());
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "beta");
}

TEST(IoBaseTest, ZipMissingFileFails)
{
    EXPECT_EQ(Zip::entries("no_such_file.zip").error(), vn::io::IoError::NotFound);
    EXPECT_EQ(Zip::readEntry("no_such_file.zip", u8"x").error(), vn::io::IoError::NotFound);
    EXPECT_EQ(Zip::decompressFile("no_such_file.zip", "out"), vn::io::IoError::NotFound);
}

TEST(IoBaseTest, ZipCompressDirectoryToFile)
{
    TempDir                     dir;
    const std::filesystem::path source   = dir.path() / "src";
    const std::filesystem::path zip_path = dir.path() / "dir.zip";
    std::filesystem::create_directories(source / "sub");
    {
        std::ofstream f(source / "a.txt");
        f << "hello";
    }

    ASSERT_EQ(Zip::compressDirectory(source, zip_path), vn::io::IoError::Ok);
    EXPECT_TRUE(std::filesystem::exists(zip_path));

    const auto entries = Zip::entries(zip_path);
    ASSERT_TRUE(entries.ok());
    EXPECT_TRUE(hasEntry(entries.value(), std::filesystem::path(u8"a.txt")));
}

TEST(IoBaseTest, ZipDecompressFileToDirectory)
{
    TempDir                     dir;
    const std::filesystem::path source   = dir.path() / "src";
    const std::filesystem::path dest     = dir.path() / "out";
    const std::filesystem::path zip_path = dir.path() / "dir.zip";
    std::filesystem::create_directories(source);
    {
        std::ofstream f(source / "b.txt");
        f << "world";
    }

    ASSERT_EQ(Zip::compressDirectory(source, zip_path), vn::io::IoError::Ok);
    ASSERT_EQ(Zip::decompressFile(zip_path, dest), vn::io::IoError::Ok);

    std::ifstream in(dest / "b.txt");
    ASSERT_TRUE(in.good());
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "world");
}

/**
 * @brief Adds text content to an archive.
 */
void addText(ZipArchive& archive, const std::filesystem::path& name, const std::string& text)
{
    EXPECT_EQ(archive.addFile(name, asBytes(text)), vn::io::IoError::Ok);
}

/**
 * @brief Generates content on the fly and counts how often it is pulled.
 *
 * A ZIP pulls content only while it is written, so the counters are how a test
 * tells "read while saving" apart from "read up front".
 */
class GeneratedSource final : public vn::io::DataSource
{
  public:
    GeneratedSource(std::uint64_t produced, std::uint64_t reported, unsigned char fill)
        : produced_(produced), reported_(reported), fill_(fill)
    {
    }

    std::uint64_t size() const override { return reported_; }

    void rewind() override
    {
        position_ = 0;
        ++rewinds_;
    }

    std::size_t read(std::span<std::byte> out) override
    {
        ++pulls_;
        const std::uint64_t left = position_ < produced_ ? produced_ - position_ : 0;
        const std::size_t   take = static_cast<std::size_t>(std::min<std::uint64_t>(left, out.size()));
        for (std::size_t i = 0; i < take; ++i) {
            out[i] = static_cast<std::byte>(fill_);
        }
        position_ += take;
        return take;
    }

    [[nodiscard]] int pulls() const noexcept { return pulls_; }
    [[nodiscard]] int rewinds() const noexcept { return rewinds_; }

  private:
    std::uint64_t produced_;
    std::uint64_t reported_;
    unsigned char fill_;
    std::uint64_t position_{ 0 };
    int           pulls_{ 0 };
    int           rewinds_{ 0 };
};

TEST(IoBaseTest, ZipArchiveOpensAndReadsOnDemand)
{
    TempDir    dir;
    const auto zip_path = dir.path() / "open.zip";
    {
        ZipArchive archive;
        addText(archive, u8"a.txt", "<workcell/>");
        addText(archive, u8"geoms/base.bin", "abcd");
        ASSERT_EQ(archive.createDirectory(u8"empty"), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);
    }

    const auto opened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    EXPECT_TRUE(opened->isOpen());
    EXPECT_TRUE(opened->isFile(u8"a.txt"));
    EXPECT_TRUE(opened->isDirectory(u8"geoms"));
    EXPECT_TRUE(opened->isDirectory(u8"empty"));
    EXPECT_FALSE(opened->exists(u8"geoms/nope"));
    EXPECT_TRUE(opened->exists(u8"geoms/base.bin"));
    EXPECT_EQ(opened->list(u8"")->size(), 3u);
    EXPECT_EQ(opened->list(u8"geoms")->size(), 1u);

    const auto bytes = opened->read(u8"geoms/base.bin");
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), "abcd");
    EXPECT_EQ(opened->read(u8"nope").error(), vn::io::IoError::NotFound);
    EXPECT_EQ(opened->read(u8"empty").error(), vn::io::IoError::IsADirectory);

    auto stream = opened->openRead(u8"a.txt");
    ASSERT_TRUE(stream.ok());
    EXPECT_EQ((*stream)->size(), 11u);
    std::array<std::byte, 16> buffer{};
    ASSERT_EQ((*stream)->read(buffer), 11u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), 11), "<workcell/>");
    EXPECT_EQ((*stream)->read(buffer), 0u);
    ASSERT_EQ((*stream)->seek(0), vn::io::IoError::Ok);
    EXPECT_EQ((*stream)->read(buffer), 11u);

    EXPECT_EQ(ZipArchive::open(dir.path() / "missing.zip", ZipArchive::OpenMode::ReadWrite).error(), vn::io::IoError::NotFound);
}

TEST(IoBaseTest, ZipArchiveOpensWithoutReadingContent)
{
    TempDir    dir;
    const auto source_pkg = dir.path() / "source.zip";

    std::vector<unsigned char> payload(64 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<unsigned char>((i * 31 + 7) % 251);
    }
    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"mesh.bin", payload), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(source_pkg), vn::io::IoError::Ok);
    }

    // Wreck the entry data but keep the trailing central directory intact: only a
    // reader that walks the directory can still make sense of this file at all.
    {
        std::fstream file(source_pkg, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekg(0, std::ios::end);
        const std::streamoff size = static_cast<std::streamoff>(file.tellg());
        ASSERT_GT(size, 256);
        const std::vector<char> garbage(64, static_cast<char>(0xAA));
        file.seekp(size / 2);
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
        ASSERT_TRUE(file.good());
    }

    const auto opened = ZipArchive::open(source_pkg, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok()) << "opening reads the directory only";
    const auto listed = opened->list(u8"");
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, std::filesystem::path(u8"mesh.bin"));
    EXPECT_EQ(listed->front().size, payload.size());

    // Damaged content is reported here as well: read() checks the result against the
    // checksum the directory recorded, which libzip itself never looks at.
    EXPECT_EQ(opened->read(u8"mesh.bin").error(), vn::io::IoError::IoFailure);

    auto stream = opened->openRead(u8"mesh.bin");
    ASSERT_TRUE(stream.ok());
    std::array<std::byte, 4096> buffer{};
    while ((*stream)->read(buffer) != 0) {
    }
    EXPECT_EQ((*stream)->error(), vn::io::IoError::IoFailure) << "a damaged entry is not a clean end";

    // Copying an entry onto a new archive reads it as well (libzip checks the content
    // while it writes), so the save refuses rather than producing a broken archive.
    // Reading it is refused too, because read() checks the content against the
    // checksum the directory recorded.
    EXPECT_EQ(opened->saveAs(dir.path() / "target.zip"), vn::io::IoError::IoFailure);
}

TEST(IoBaseTest, ZipArchiveCopiesUntouchedEntriesOnSave)
{
    TempDir    dir;
    const auto source_pkg = dir.path() / "source.zip";
    const auto target_pkg = dir.path() / "target.zip";

    std::vector<unsigned char> payload(256 * 1024, static_cast<unsigned char>(0x33));
    {
        ZipArchive archive;
        addText(archive, u8"workcell.xml", "<workcell/>");
        ASSERT_EQ(archive.addFile(u8"mesh.bin", payload), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(source_pkg), vn::io::IoError::Ok);
    }

    auto opened = ZipArchive::open(source_pkg, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    addText(*opened, u8"extra.txt", "added");
    ASSERT_EQ(opened->saveAs(target_pkg), vn::io::IoError::Ok);

    const auto copy = ZipArchive::open(target_pkg, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(copy.ok());
    EXPECT_EQ(copy->list(u8"")->size(), 3u);
    const auto mesh = copy->read(u8"mesh.bin");
    ASSERT_TRUE(mesh.ok());
    EXPECT_EQ(mesh.value(), payload);
    const auto text = copy->read(u8"workcell.xml");
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(std::string(text->begin(), text->end()), "<workcell/>");
    EXPECT_TRUE(copy->exists(u8"extra.txt"));

    // The archive that was opened is left as it was.
    const auto again = ZipArchive::open(source_pkg, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->list(u8"")->size(), 2u);
    EXPECT_FALSE(again->exists(u8"extra.txt"));
}

TEST(IoBaseTest, ZipArchiveWritesFragmentedContent)
{
    TempDir    dir;
    const auto zip_path = dir.path() / "fragments.zip";

    const std::vector<unsigned char> first{ 'a', 'b' };
    const std::vector<unsigned char> second{ 'c' };
    const std::vector<unsigned char> third{ 'd', 'e', 'f' };
    const std::vector<vn::io::Fragment> pieces{ { first.data(), first.size() },
                                                 { second.data(), second.size() },
                                                 { third.data(), third.size() } };

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(u8"joined.bin", pieces), vn::io::IoError::Ok);

    // The borrowed pieces go through the same gate as a byte span: a path that is
    // not a virtual path, a name taken by a directory, or no source at all.
    EXPECT_EQ(archive.addFile(u8"/escaped.bin", pieces), vn::io::IoError::InvalidPath);
    EXPECT_EQ(archive.addFile(u8"joined.bin", std::shared_ptr<vn::io::DataSource>{}), vn::io::IoError::InvalidData);

    const auto listed = archive.list(u8"");
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().size, 6u);
    ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);

    const auto opened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    const auto bytes = opened->read(u8"joined.bin");
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), "abcdef");

    // An empty piece list writes a zero-length file, like an empty byte span does.
    ZipArchive blank;
    ASSERT_EQ(blank.addFile(u8"empty.bin", std::span<const vn::io::Fragment>{}), vn::io::IoError::Ok);
    const auto blank_info = blank.stat(u8"empty.bin");
    ASSERT_TRUE(blank_info.ok());
    EXPECT_EQ(blank_info->size, 0u);
}

TEST(IoBaseTest, ZipArchivePullsGeneratedContent)
{
    TempDir    dir;
    const auto zip_path = dir.path() / "generated.zip";

    auto source = std::make_shared<GeneratedSource>(100 * 1024, 100 * 1024, static_cast<unsigned char>(0x5A));
    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"stream.bin", source), vn::io::IoError::Ok);
        ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);
        EXPECT_GT(source->pulls(), 0) << "the content is pulled while the archive is written";
        EXPECT_EQ(source->rewinds(), 1);
    }

    const auto opened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    const auto bytes = opened->read(u8"stream.bin");
    ASSERT_TRUE(bytes.ok());
    ASSERT_EQ(bytes->size(), 100 * 1024u);
    EXPECT_EQ((*bytes)[0], 0x5A);
    EXPECT_EQ(bytes->back(), 0x5A);

    // The declared size is authoritative: a source that stops early fails the save
    // instead of writing a truncated entry, and one that stops late is cut off.
    auto short_source = std::make_shared<GeneratedSource>(10, 20, static_cast<unsigned char>(0x11));
    ZipArchive truncating;
    ASSERT_EQ(truncating.addFile(u8"short.bin", short_source), vn::io::IoError::Ok);
    EXPECT_NE(truncating.saveAs(dir.path() / "short.zip"), vn::io::IoError::Ok);

    auto long_source = std::make_shared<GeneratedSource>(20, 10, static_cast<unsigned char>(0x22));
    ZipArchive clipped;
    ASSERT_EQ(clipped.addFile(u8"clip.bin", long_source), vn::io::IoError::Ok);
    ASSERT_EQ(clipped.saveAs(dir.path() / "clip.zip"), vn::io::IoError::Ok);
    const auto clipped_file = ZipArchive::open(dir.path() / "clip.zip", ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(clipped_file.ok());
    const auto clipped_bytes = clipped_file->read(u8"clip.bin");
    ASSERT_TRUE(clipped_bytes.ok());
    EXPECT_EQ(clipped_bytes->size(), 10u);
}

TEST(IoBaseTest, DirectoryVfsAssemblesSourcesThroughTheBaseInterface)
{
    TempDir         dir;
    std::error_code ec;
    const auto      root = dir.path() / "tree";
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);

    DirectoryVfs   real(root);
    vn::io::Vfs& base = real; // a caller usually holds the base interface only

    const std::vector<unsigned char>      first{ 'a', 'b' };
    const std::vector<unsigned char>      second{ 'c' };
    const std::vector<vn::io::Fragment> pieces{ { first.data(), first.size() }, { second.data(), second.size() } };
    ASSERT_EQ(base.addFile(u8"joined.bin", pieces), vn::io::IoError::Ok);
    ASSERT_EQ(real.addFile(u8"direct.bin", pieces), vn::io::IoError::Ok); // the base overload stays reachable on the backend itself
    const auto joined = base.read(u8"joined.bin");
    ASSERT_TRUE(joined.ok());
    EXPECT_EQ(std::string(joined->begin(), joined->end()), "abc");
    const auto direct = base.read(u8"direct.bin");
    ASSERT_TRUE(direct.ok());
    EXPECT_EQ(std::string(direct->begin(), direct->end()), "abc");

    // The default materializes the content while adding, because this backend stores it right away.
    auto source = std::make_shared<GeneratedSource>(4 * 1024, 4 * 1024, static_cast<unsigned char>(0x6C));
    ASSERT_EQ(base.addFile(u8"gen.bin", source), vn::io::IoError::Ok);
    EXPECT_GT(source->pulls(), 0) << "the default pulls the source right away";
    const auto generated = base.read(u8"gen.bin");
    ASSERT_TRUE(generated.ok());
    ASSERT_EQ(generated->size(), 4 * 1024u);
    EXPECT_EQ((*generated)[0], 0x6C);

    // A null source, a source that stops short, and a piece without data are refused and write nothing.
    EXPECT_EQ(base.addFile(u8"null.bin", std::shared_ptr<vn::io::DataSource>{}), vn::io::IoError::InvalidData);
    auto short_source = std::make_shared<GeneratedSource>(2, 8, static_cast<unsigned char>(0x11));
    EXPECT_EQ(base.addFile(u8"short.bin", short_source), vn::io::IoError::InvalidData);
    const std::vector<vn::io::Fragment> dataless{ { nullptr, 3 } };
    EXPECT_EQ(base.addFile(u8"dataless.bin", dataless), vn::io::IoError::InvalidData);
    EXPECT_FALSE(base.exists(u8"null.bin"));
    EXPECT_FALSE(base.exists(u8"short.bin"));
    EXPECT_FALSE(base.exists(u8"dataless.bin"));
}

TEST(IoBaseTest, ZipArchiveKeepsSourcesLazyThroughTheBaseInterface)
{
    TempDir    dir;
    const auto zip_path = dir.path() / "base.zip";

    const std::vector<unsigned char>      head{ 'a', 'b' };
    const std::vector<unsigned char>      tail{ 'c', 'd', 'e' };
    const std::vector<vn::io::Fragment> pieces{ { head.data(), head.size() }, { tail.data(), tail.size() } };
    auto source = std::make_shared<GeneratedSource>(3 * 1024, 3 * 1024, static_cast<unsigned char>(0x27));

    {
        ZipArchive     archive;
        vn::io::Vfs& base = archive; // the override, not the default, has to run here

        ASSERT_EQ(base.addFile(u8"gen.bin", source), vn::io::IoError::Ok);
        ASSERT_EQ(base.addFile(u8"joined.bin", pieces), vn::io::IoError::Ok);
        EXPECT_EQ(source->pulls(), 0) << "the archive defers the pull to the save";
        EXPECT_EQ(source->rewinds(), 0);

        const std::vector<vn::io::Fragment> dataless{ { nullptr, 3 } };
        EXPECT_EQ(base.addFile(u8"dataless.bin", dataless), vn::io::IoError::InvalidData);

        ASSERT_EQ(base.saveAs(zip_path), vn::io::IoError::Ok);
        EXPECT_GT(source->pulls(), 0) << "the content is pulled while the archive is written";
        EXPECT_EQ(source->rewinds(), 1);
    }

    const auto opened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    const auto joined = opened->read(u8"joined.bin");
    ASSERT_TRUE(joined.ok());
    EXPECT_EQ(std::string(joined->begin(), joined->end()), "abcde");
    const auto generated = opened->read(u8"gen.bin");
    ASSERT_TRUE(generated.ok());
    ASSERT_EQ(generated->size(), 3 * 1024u);
    EXPECT_EQ((*generated)[0], 0x27);
    EXPECT_FALSE(opened->exists(u8"dataless.bin"));
}

TEST(IoBaseTest, ZipArchiveCommitReplacesTheFile)
{
    TempDir    dir;
    const auto zip_path = dir.path() / "commit.zip";
    {
        ZipArchive archive;
        addText(archive, u8"a.txt", "one");
        ASSERT_EQ(archive.saveAs(zip_path), vn::io::IoError::Ok);
    }

    auto opened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(opened.ok());
    addText(*opened, u8"b.txt", "two");
    ASSERT_EQ(opened->remove(u8"a.txt"), vn::io::IoError::Ok);
    ASSERT_EQ(opened->commit(), vn::io::IoError::Ok);

    std::filesystem::path temp = zip_path;
    temp += ".vine-tmp";
    EXPECT_FALSE(std::filesystem::exists(temp)) << "the temporary file is gone";
    EXPECT_TRUE(opened->isFile(u8"b.txt"));
    EXPECT_FALSE(opened->exists(u8"a.txt"));
    const auto bytes = opened->read(u8"b.txt");
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), "two");

    const auto reopened = ZipArchive::open(zip_path, ZipArchive::OpenMode::ReadWrite);
    ASSERT_TRUE(reopened.ok());
    EXPECT_TRUE(reopened->exists(u8"b.txt"));
    EXPECT_FALSE(reopened->exists(u8"a.txt"));

    // An archive that was not opened from a file has nothing to commit to.
    ZipArchive empty;
    EXPECT_EQ(empty.commit(), vn::io::IoError::Unsupported);
}

} // namespace
