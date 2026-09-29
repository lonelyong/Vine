#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/Zip.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/String.hpp>

#include "VfsTestSupport.hpp"

using vn::String;
using vn::io::VfsEntryInfo;
using vn::io::VfsEntryKind;
using vn::io::IoError;
using vn::io::Zip;
using vn::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::findInfo;
using vfstest::sortedNames;
using vfstest::TempDir;

namespace
{

/**
 * @brief Fills a memory backend with the package tree these tests are built on.
 */
void fillPackageTree(ZipArchive& vfs)
{
    EXPECT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
    EXPECT_EQ(vfs.addFile(u8"geoms/base.bin", bytesOf("abcd")), IoError::Ok);
    EXPECT_EQ(vfs.addFile(u8"devices/robot.vdev", bytesOf("xyz")), IoError::Ok);
    EXPECT_EQ(vfs.createDirectory(u8"empty"), IoError::Ok);
    EXPECT_EQ(vfs.createDirectories(u8"nested/deep"), IoError::Ok);
}

/**
 * @brief Builds a small package with files, an explicit directory and a nested tree.
 */
std::vector<unsigned char> buildPackage()
{
    ZipArchive vfs;
    fillPackageTree(vfs);

    auto bytes = vfs.toBytes();
    EXPECT_TRUE(bytes.ok());
    return bytes.take();
}

/**
 * @brief Renames one entry in an archive's central directory, in place.
 *
 * Two entries under one name cannot be produced through this library - the second addFile() is refused - so a fixture
 * that holds one has to be written by hand: one of two equally long names is replaced in its directory record, and
 * nothing in the archive has to move for it.
 *
 * @param bytes The archive bytes to rewrite in place.
 * @param from The name the directory currently holds.
 * @param to The name to store instead; must have the same length as from.
 */
void rewriteNameInDirectory(std::vector<unsigned char>& bytes, const std::string& from, const std::string& to)
{
    ASSERT_EQ(from.size(), to.size());

    // A central directory header spells its name length at 28 and its name at 46.
    int rewritten = 0;
    for (std::size_t offset = 0; offset + 46 + to.size() <= bytes.size(); ++offset) {
        const bool is_record = bytes[offset] == 0x50 && bytes[offset + 1] == 0x4B && bytes[offset + 2] == 0x01 &&
                               bytes[offset + 3] == 0x02;
        if (!is_record) {
            continue;
        }
        const std::uint16_t length =
                static_cast<std::uint16_t>(bytes[offset + 28] | static_cast<std::uint16_t>(bytes[offset + 29] << 8));
        if (length != from.size() || !std::equal(from.begin(), from.end(), bytes.begin() + offset + 46)) {
            continue;
        }
        std::copy(to.begin(), to.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 46));
        ++rewritten;
    }
    ASSERT_EQ(rewritten, 1) << "the name was expected in exactly one directory record";
}

/**
 * @brief Reports whether a child is present with the expected kind and size.
 */
bool matches(const std::vector<VfsEntryInfo>& children, const std::filesystem::path& path, VfsEntryKind kind, std::uint64_t size)
{
    const VfsEntryInfo* info = findInfo(children, path);
    return info != nullptr && info->kind == kind && info->size == size;
}

} // namespace

TEST(ZipArchiveTest, ArchiveIndexReportsNamesSizesAndKinds)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "index.zip";
    {
        const std::vector<unsigned char> bytes = buildPackage();
        std::ofstream                    out(pkg, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    const auto entries = Zip::entries(pkg);
    ASSERT_TRUE(entries.ok());

    const auto found = [&entries](const std::filesystem::path& path) -> const VfsEntryInfo* {
        for (const VfsEntryInfo& entry : entries.value()) {
            if (entry.path == path) {
                return &entry;
            }
        }
        return nullptr;
    };

    const VfsEntryInfo* xml = found(std::filesystem::path(u8"workcell.xml"));
    ASSERT_NE(xml, nullptr);
    EXPECT_EQ(xml->kind, VfsEntryKind::File);
    EXPECT_EQ(xml->size, 11u);

    const VfsEntryInfo* empty = found(std::filesystem::path(u8"empty"));
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->kind, VfsEntryKind::Directory);
    EXPECT_EQ(empty->size, 0u);

    EXPECT_NE(xml->crc, 0u); // the archive records a checksum for every file
    EXPECT_EQ(empty->crc, 0u);

    // An opened archive answers with the same checksum as the one-shot listing.
    const auto opened = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto xml_stat = opened->stat(u8"workcell.xml");
    ASSERT_TRUE(xml_stat.ok());
    EXPECT_EQ(xml_stat->size, xml->size);
    EXPECT_EQ(xml_stat->crc, xml->crc);

    // The in-memory form reports the same index.
    const std::vector<unsigned char> bytes = buildPackage();
    const auto                       from_memory = Zip::entries(bytes);
    ASSERT_TRUE(from_memory.ok());
    EXPECT_EQ(from_memory->size(), entries->size());

    // Nothing that is not a zip can be indexed.
    const std::vector<unsigned char> garbage{ 'n', 'o', 't', 'a', 'z', 'i', 'p' };
    EXPECT_EQ(Zip::entries(garbage).error(), IoError::InvalidData);
    EXPECT_EQ(Zip::entries(temp.path() / "missing.zip").error(), IoError::NotFound);
}

TEST(ZipArchiveTest, MatchesTheMemoryBackendForTheSameContent)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "same.zip";

    // The writable tree is the reference implementation: the lazy reader has to
    // answer the same questions about the package that tree produced.
    ZipArchive built;
    fillPackageTree(built);
    ASSERT_EQ(built.saveAs(pkg), IoError::Ok);

    auto lazy = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(lazy.ok());

    for (const std::filesystem::path& dir :
         { std::filesystem::path(u8""), std::filesystem::path(u8"geoms"), std::filesystem::path(u8"devices"), std::filesystem::path(u8"nested"), std::filesystem::path(u8"empty") }) {
        const auto lazy_children = lazy->list(dir);
        const auto tree_children = built.list(dir);
        ASSERT_TRUE(lazy_children.ok()) << dir.generic_string();
        ASSERT_TRUE(tree_children.ok()) << dir.generic_string();
        EXPECT_EQ(sortedNames(lazy_children.value()), sortedNames(tree_children.value())) << dir.generic_string();
        EXPECT_EQ(lazy_children->size(), tree_children->size()) << dir.generic_string();
    }

    // The same paths also fail the same way.
    for (const std::filesystem::path& path : { std::filesystem::path(u8"nope"), std::filesystem::path(u8"workcell.xml"), std::filesystem::path(u8"empty") }) {
        EXPECT_EQ(lazy->list(path).error(), built.list(path).error()) << path.generic_string();
    }

    for (const std::filesystem::path& path :
         { std::filesystem::path(u8"workcell.xml"), std::filesystem::path(u8"geoms"), std::filesystem::path(u8"geoms/base.bin"), std::filesystem::path(u8"empty"), std::filesystem::path(u8"nested/deep") }) {
        const auto lazy_info = lazy->stat(path);
        const auto tree_info = built.stat(path);
        ASSERT_TRUE(lazy_info.ok()) << path.generic_string();
        ASSERT_TRUE(tree_info.ok()) << path.generic_string();
        EXPECT_EQ(lazy_info->path, tree_info->path) << path.generic_string();
        EXPECT_EQ(lazy_info->kind, tree_info->kind) << path.generic_string();
        EXPECT_EQ(lazy_info->size, tree_info->size) << path.generic_string();
    }

    for (const std::filesystem::path& path : { std::filesystem::path(u8"workcell.xml"), std::filesystem::path(u8"geoms/base.bin"), std::filesystem::path(u8"devices/robot.vdev") }) {
        const auto lazy_bytes = lazy->read(path);
        const auto tree_bytes = built.read(path);
        ASSERT_TRUE(lazy_bytes.ok()) << path.generic_string();
        ASSERT_TRUE(tree_bytes.ok()) << path.generic_string();
        EXPECT_EQ(lazy_bytes.value(), tree_bytes.value()) << path.generic_string();
    }
}

TEST(ZipArchiveTest, RefusesAnArchiveThatHoldsOneNameTwice)
{
    ZipArchive writer;
    ASSERT_EQ(writer.addFile(u8"a.txt", bytesOf("first")), IoError::Ok);
    ASSERT_EQ(writer.addFile(u8"b.txt", bytesOf("second")), IoError::Ok);
    auto bytes = writer.toBytes();
    ASSERT_TRUE(bytes.ok());

    // Two entries under one name contradict each other, and keeping the last one would silently drop content that the
    // archive went to the trouble of storing - so the archive as a whole is refused.
    rewriteNameInDirectory(bytes.value(), "b.txt", "a.txt");

    const auto opened = ZipArchive::open(bytes.value(), ZipArchive::OpenMode::ReadOnly);
    EXPECT_FALSE(opened.ok());
    EXPECT_EQ(opened.error(), IoError::InvalidData);
}

TEST(ZipArchiveTest, ReadsEntriesOnDemand)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "lazy.zip";
    {
        const std::vector<unsigned char> bytes = buildPackage();
        std::ofstream                    out(pkg, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    auto zip = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(zip.ok());
    EXPECT_TRUE(zip->read(u8"workcell.xml").ok());

    // The archive is held open, so the source file cannot be pulled out from under
    // the tree: removing it fails on Windows, and the read keeps working either way.
    std::error_code ec;
    std::filesystem::remove(pkg, ec);
    EXPECT_TRUE(zip->read(u8"workcell.xml").ok()) << "a held-open archive stays readable";

    // The index is in memory and stays usable as well.
    EXPECT_TRUE(zip->stat(u8"workcell.xml").ok());
    EXPECT_TRUE(zip->list(u8"").ok());
}

TEST(ZipArchiveTest, OpensAnInMemoryArchive)
{
    std::vector<unsigned char> bytes = buildPackage();
    auto                       zip   = ZipArchive::open(std::move(bytes), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(zip.ok());

    const auto text = zip->read(u8"workcell.xml");
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(text.value(), bytesOf("<workcell/>"));
    EXPECT_EQ(zip->stat(u8"empty")->size, 0u);
    EXPECT_TRUE(zip->isDirectory(u8"nested/deep"));

    // Bytes can only be taken from a valid archive.
    EXPECT_FALSE(ZipArchive::open(std::vector<unsigned char>{}, ZipArchive::OpenMode::ReadOnly).ok());
    EXPECT_FALSE(ZipArchive::open(std::vector<unsigned char>{ 'n', 'o', 'p', 'e' }, ZipArchive::OpenMode::ReadOnly).ok());
    EXPECT_FALSE(ZipArchive::open(std::filesystem::path("does-not-exist.zip"), ZipArchive::OpenMode::ReadOnly).ok());
}

TEST(ZipArchiveTest, OpensBorrowedBytes)
{
    const TempDir              temp;
    std::vector<unsigned char> bytes = buildPackage(); // the caller keeps owning these

    std::unique_ptr<vn::io::VfsEntrySource> reader;
    {
        // An lvalue is borrowed, not copied: the archive reads the caller's block.
        auto zip = ZipArchive::open(bytes, ZipArchive::OpenMode::ReadOnly);
        ASSERT_TRUE(zip.ok());
        EXPECT_TRUE(zip->isReadOnly());

        const auto text = zip->read(u8"workcell.xml");
        ASSERT_TRUE(text.ok());
        EXPECT_EQ(text.value(), bytesOf("<workcell/>"));

        auto stream = zip->openRead(u8"workcell.xml");
        ASSERT_TRUE(stream.ok());
        reader = stream.take();

        EXPECT_EQ(zip->saveAs(temp.path() / "borrowed.zip"), IoError::Ok);
    }

    // A reader keeps the archive's handle alive, so it outlives the archive as
    // long as the borrowed bytes are still around.
    std::array<std::byte, 16> buffer{};
    ASSERT_EQ(reader->read(buffer), 11u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), 11), "<workcell/>");

    // An empty span is not an archive.
    EXPECT_EQ(ZipArchive::open(std::span<const unsigned char>{}, ZipArchive::OpenMode::ReadOnly).error(),
              IoError::InvalidData);
}

TEST(ZipArchiveTest, ReportsErrorsAndRefusesEveryChange)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "readonly.zip";
    {
        const std::vector<unsigned char> bytes = buildPackage();
        std::ofstream                    out(pkg, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    auto zip = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(zip.ok());
    ASSERT_TRUE(zip->isReadOnly());

    EXPECT_EQ(zip->stat(u8"nope").error(), IoError::NotFound);
    EXPECT_EQ(zip->stat(u8"../x").error(), IoError::InvalidPath);
    EXPECT_EQ(zip->list(u8"workcell.xml").error(), IoError::NotADirectory);
    EXPECT_EQ(zip->list(u8"nope").error(), IoError::NotFound);
    EXPECT_EQ(zip->read(u8"empty").error(), IoError::IsADirectory);
    EXPECT_EQ(zip->read(u8"nope").error(), IoError::NotFound);

    EXPECT_EQ(zip->addFile(u8"new.txt", bytesOf("x")), IoError::ReadOnly);
    EXPECT_EQ(zip->createDirectory(u8"new"), IoError::ReadOnly);
    EXPECT_EQ(zip->createDirectories(u8"new/deep"), IoError::ReadOnly);
    EXPECT_EQ(zip->rename(u8"a", u8"b"), IoError::ReadOnly);
    EXPECT_EQ(zip->remove(u8"workcell.xml"), IoError::ReadOnly);
    EXPECT_EQ(zip->removeAll(u8"geoms"), IoError::ReadOnly);
    EXPECT_EQ(zip->addFile(u8"a", pkg), IoError::ReadOnly);
    EXPECT_EQ(zip->addFile(u8"a", std::shared_ptr<vn::io::DataSource>{}), IoError::ReadOnly);
    EXPECT_EQ(zip->addFile(u8"a", std::span<const vn::io::Fragment>{}), IoError::ReadOnly);

    // A read-only view still exports: writing a copy elsewhere does not change it,
    // while committing back to the file it came from is refused.
    EXPECT_EQ(zip->saveAs(temp.path() / "out.zip"), IoError::Ok);
    EXPECT_TRUE(zip->toBytes().ok());
    EXPECT_EQ(zip->commit(), IoError::ReadOnly);

    // The archive is untouched by all of that.
    EXPECT_TRUE(zip->exists(u8"workcell.xml"));
}

TEST(ZipArchiveTest, ListReportsEveryChildShape)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "shapes.zip";
    {
        const std::vector<unsigned char> bytes = buildPackage();
        std::ofstream                    out(pkg, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    auto zip = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(zip.ok());

    const auto top = zip->list(u8"");
    ASSERT_TRUE(top.ok());
    EXPECT_TRUE(matches(top.value(), std::filesystem::path(u8"workcell.xml"), VfsEntryKind::File, 11u));
    EXPECT_TRUE(matches(top.value(), std::filesystem::path(u8"geoms"), VfsEntryKind::Directory, 0u));
    EXPECT_TRUE(matches(top.value(), std::filesystem::path(u8"empty"), VfsEntryKind::Directory, 0u));
    EXPECT_TRUE(matches(top.value(), std::filesystem::path(u8"nested"), VfsEntryKind::Directory, 0u));
    EXPECT_TRUE(matches(top.value(), std::filesystem::path(u8"devices"), VfsEntryKind::Directory, 0u));
    EXPECT_EQ(top->size(), 5u);

    const auto nested = zip->list(u8"nested");
    ASSERT_TRUE(nested.ok());
    ASSERT_EQ(nested->size(), 1u);
    EXPECT_TRUE(matches(nested.value(), std::filesystem::path(u8"nested/deep"), VfsEntryKind::Directory, 0u));

    const auto devices = zip->list(u8"devices");
    ASSERT_TRUE(devices.ok());
    ASSERT_EQ(devices->size(), 1u);
    EXPECT_TRUE(matches(devices.value(), std::filesystem::path(u8"devices/robot.vdev"), VfsEntryKind::File, 3u));
}
