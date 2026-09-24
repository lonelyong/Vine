#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/Zip.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/String.hpp>

#include "VfsTestSupport.hpp"

using vine::String;
using vine::io::DirectoryVfs;
using vine::io::IoError;
using vine::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::TempDir;

namespace
{

/**
 * @brief Writes a real file whose name cannot be spelled in an ANSI code page.
 *
 * The name is deliberately outside the local code page: a boundary that converts through
 * the native narrow form mangles it, and the file system would read the result as broken
 * UTF-8 - which is what these tests pin down.
 *
 * @param dir The directory to write into.
 * @param name The file name, in UTF-8.
 * @param text The content to write.
 */
void writeRealFile(const std::filesystem::path& dir, const std::u8string& name, const std::string& text)
{
    std::ofstream out(dir / std::filesystem::path(name), std::ios::binary);
    ASSERT_TRUE(out.good());
    out << text;
    ASSERT_TRUE(out.good());
}

/**
 * @brief Reads a little-endian 16-bit field of a ZIP record.
 */
std::uint16_t readLe16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      static_cast<std::uint16_t>(bytes[offset + 1] << 8));
}

/**
 * @brief Writes a little-endian 16-bit field of a ZIP record.
 */
void writeLe16(std::vector<unsigned char>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset]     = static_cast<unsigned char>(value & 0xFF);
    bytes[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
}

/**
 * @brief Tells whether some text sits at an offset of a buffer.
 */
bool matchesAt(const std::vector<unsigned char>& bytes, std::size_t offset, const std::string& text)
{
    return offset + text.size() <= bytes.size() &&
           std::equal(text.begin(), text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

/**
 * @brief Replaces an entry's stored name in place, keeping the archive layout intact.
 *
 * The names this library writes come from the path it was given, so an archive holding a
 * name that is text in no encoding cannot be produced by the writer - it can only come
 * from a foreign writer. Replacing the bytes directly is what makes such a fixture: the
 * replacement has the same length, so nothing in the archive has to move, and the UTF-8
 * flag is cleared so the entry really does look like a legacy one.
 *
 * @param bytes The archive bytes to rewrite in place.
 * @param from The name the archive currently holds; must occur in both records.
 * @param to The name to store instead; must have the same length as from.
 */
void rewriteStoredName(std::vector<unsigned char>& bytes, const std::string& from, const std::string& to)
{
    ASSERT_EQ(from.size(), to.size());

    struct Record
    {
        std::array<unsigned char, 4> signature;
        std::size_t                  flags_offset;  ///< General purpose flags, relative to the signature.
        std::size_t                  length_offset; ///< Name length, relative to the signature.
        std::size_t                  extra_offset;  ///< Extra field length, relative to the signature.
        std::size_t                  name_offset;   ///< Name, relative to the signature.
    };
    const Record records[] = {
        { { 0x50, 0x4B, 0x03, 0x04 }, 6, 26, 28, 30 }, // Local file header.
        { { 0x50, 0x4B, 0x01, 0x02 }, 8, 28, 30, 46 }, // Central directory header.
    };

    int rewritten = 0;
    for (const Record& record : records) {
        for (std::size_t offset = 0; offset + record.length_offset + 2 <= bytes.size(); ++offset) {
            if (!std::equal(record.signature.begin(), record.signature.end(),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
                continue;
            }
            if (readLe16(bytes, offset + record.length_offset) != from.size() ||
                !matchesAt(bytes, offset + record.name_offset, from)) {
                continue;
            }
            // An extra field would carry the real name and win over the bytes replaced
            // here, so the fixture only means anything while there is none.
            ASSERT_EQ(readLe16(bytes, offset + record.extra_offset), 0u)
                << "the writer added an extra field; the name is not in the header any more";

            std::copy(to.begin(), to.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + record.name_offset));
            // Bit 11 says "this name is UTF-8"; a legacy entry does not set it, and libzip
            // believes the flag before it looks at the bytes.
            writeLe16(bytes, offset + record.flags_offset,
                      static_cast<std::uint16_t>(readLe16(bytes, offset + record.flags_offset) & ~0x0800u));
            ++rewritten;
        }
    }
    ASSERT_EQ(rewritten, 2) << "the name was expected in the local header and in the directory";
}

} // namespace

TEST(EncodingTest, DirectoryVfsReportsNonAsciiNamesAsUtf8)
{
    const TempDir dir;
    writeRealFile(dir.path(), u8"机械臂.txt", "arm");

    const auto vfs = DirectoryVfs::openDirectory(dir.path());
    ASSERT_NE(vfs, nullptr);

    const auto info = vfs->stat(u8"机械臂.txt");
    ASSERT_TRUE(info.ok()) << vine::io::ioErrorName(info.error());
    EXPECT_EQ(info->path, std::filesystem::path(u8"机械臂.txt"));
    EXPECT_EQ(info->size, 3u);

    const auto listed = vfs->list(std::filesystem::path{});
    ASSERT_TRUE(listed.ok()) << vine::io::ioErrorName(listed.error());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, std::filesystem::path(u8"机械臂.txt"));
    EXPECT_EQ(listed->front().name(), std::filesystem::path(u8"机械臂.txt"));
}

TEST(EncodingTest, AddDirectoryKeepsNonAsciiNamesThroughAZip)
{
    const TempDir dir;
    writeRealFile(dir.path(), u8"机械臂.txt", "arm");

    ZipArchive archive;
    ASSERT_EQ(archive.addDirectory(std::filesystem::path{}, dir.path()), IoError::Ok);
    ASSERT_TRUE(archive.stat(u8"机械臂.txt").ok()) << "the imported name did not survive the import";

    auto bytes = archive.toBytes();
    ASSERT_TRUE(bytes.ok()) << vine::io::ioErrorName(bytes.error());

    auto reopened = ZipArchive::open(bytes.take(), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(reopened.ok()) << vine::io::ioErrorName(reopened.error());

    const auto read = reopened->read(u8"机械臂.txt");
    ASSERT_TRUE(read.ok()) << vine::io::ioErrorName(read.error());
    EXPECT_EQ(std::string(read->begin(), read->end()), "arm");

    const auto listed = reopened->list(std::filesystem::path{});
    ASSERT_TRUE(listed.ok()) << vine::io::ioErrorName(listed.error());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, std::filesystem::path(u8"机械臂.txt"));
}

TEST(EncodingTest, AddDirectoryIntoADirectoryVfsKeepsNonAsciiNames)
{
    const TempDir source;
    writeRealFile(source.path(), u8"机械臂.txt", "arm");

    const TempDir destination;
    const auto    vfs = DirectoryVfs::openDirectory(destination.path());
    ASSERT_NE(vfs, nullptr);

    // Both directions of the boundary are exercised here: the real name becomes a virtual
    // path, and that path is joined back onto the backend's own root.
    ASSERT_EQ(vfs->addDirectory(std::filesystem::path{}, source.path()), IoError::Ok);

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(destination.path() / std::filesystem::path(u8"机械臂.txt"), ec)) << ec.message();
    EXPECT_TRUE(vfs->stat(u8"机械臂.txt").ok());
    EXPECT_TRUE(vfs->read(u8"机械臂.txt").ok());
}

TEST(EncodingTest, HostEncodedNamesSurviveAZipRoundTrip)
{
    // These bytes are "arm.txt" spelled in the host's own code page - what a writer on a
    // machine like this one stored before UTF-8 became the norm. A host path can only hold
    // text, so the bytes are read as the host's encoding; the name then has to travel
    // through the tree, come back from a repack and still name the same entry.
    const std::filesystem::path name(std::string("\xBB\xFA\xD0\xB5\xB1\xDB.txt"));

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(name, bytesOf("legacy")), IoError::Ok);
    ASSERT_TRUE(archive.stat(name).ok()) << "an entry has to be reachable by the name it was given";

    auto bytes = archive.toBytes();
    ASSERT_TRUE(bytes.ok()) << vine::io::ioErrorName(bytes.error());

    auto reopened = ZipArchive::open(bytes.take(), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(reopened.ok()) << vine::io::ioErrorName(reopened.error());

    const auto listed = reopened->list(std::filesystem::path{});
    ASSERT_TRUE(listed.ok()) << vine::io::ioErrorName(listed.error());
    ASSERT_EQ(listed->size(), 1u);
    EXPECT_EQ(listed->front().path, name) << "the name came back as something else";
    EXPECT_EQ(listed->front().name(), name);

    const auto read = reopened->read(name);
    ASSERT_TRUE(read.ok()) << vine::io::ioErrorName(read.error());
    EXPECT_EQ(std::string(read->begin(), read->end()), "legacy");

    // A second repack keeps it stable, so a package can be edited and rewritten.
    auto again = reopened->toBytes();
    ASSERT_TRUE(again.ok());

    auto second = ZipArchive::open(again.take(), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(second.ok());
    const auto twice = second->list(std::filesystem::path{});
    ASSERT_TRUE(twice.ok());
    ASSERT_EQ(twice->size(), 1u);
    EXPECT_EQ(twice->front().path, name);
}

TEST(EncodingTest, NamesThatAreTextInNoEncodingStayReachable)
{
    // 0x81 is a GBK lead byte followed by a space, which no double-byte code page accepts,
    // and it is a continuation byte in UTF-8 - so these bytes are text in neither. A name
    // like that still has to be listed, read back and written out unchanged: refusing it
    // would make the whole entry, not just its name, unreachable. The bytes are held
    // unsigned because a signed char would never compare equal to a byte above 0x7F.
    const std::vector<unsigned char> unreadable{ 0x81, 0x20, 0x81, 0x20, 0x81, 0x2E, 0x81, 0x2E };
    const std::string                unreadable_name(unreadable.begin(), unreadable.end());
    const std::string                placeholder = "AAAAAAAA";

    ZipArchive archive;
    ASSERT_EQ(archive.addFile(std::filesystem::path(placeholder), bytesOf("payload")), IoError::Ok);
    auto written = archive.toBytes();
    ASSERT_TRUE(written.ok()) << vine::io::ioErrorName(written.error());

    std::vector<unsigned char> raw = written.take();
    rewriteStoredName(raw, placeholder, unreadable_name);

    auto reopened = ZipArchive::open(raw, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(reopened.ok()) << vine::io::ioErrorName(reopened.error());

    const auto listed = reopened->list(std::filesystem::path{});
    ASSERT_TRUE(listed.ok()) << vine::io::ioErrorName(listed.error());
    ASSERT_EQ(listed->size(), 1u) << "an entry whose name cannot be decoded is still an entry";
    const std::filesystem::path name = listed->front().path;
    ASSERT_FALSE(name.empty());

    const auto read = reopened->read(name);
    ASSERT_TRUE(read.ok()) << vine::io::ioErrorName(read.error());
    EXPECT_EQ(std::string(read->begin(), read->end()), "payload");

    // The flat reader looks entries up by name, so the name the listing handed out has to
    // find the entry again even though it spells something the archive never stored.
    const auto direct = vine::io::Zip::readEntry(std::span<const unsigned char>(raw), name);
    ASSERT_TRUE(direct.ok()) << vine::io::ioErrorName(direct.error());
    EXPECT_EQ(std::string(direct->begin(), direct->end()), "payload");

    // A repack has to put the archive's own bytes back, not a spelling of what they meant.
    auto repacked = reopened->toBytes();
    ASSERT_TRUE(repacked.ok()) << vine::io::ioErrorName(repacked.error());
    const std::vector<unsigned char>& out = repacked.value();
    EXPECT_NE(std::search(out.begin(), out.end(), unreadable.begin(), unreadable.end()), out.end())
        << "the stored name bytes were replaced by a spelling of them";
}
