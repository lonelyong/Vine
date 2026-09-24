#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <vine/String.hpp>
#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/ZipArchive.hpp>

#include "VfsTestSupport.hpp"

using vn::io::DirectoryVfs;
using vn::io::IoError;
using vn::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::sortedNames;
using vfstest::TempDir;

namespace
{

bool contains(const std::vector<std::filesystem::path>& list, const std::filesystem::path& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}

} // namespace

TEST(VfsTest, ZipWriteReadRoundTrip)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell name=\"demo\"/>")), IoError::Ok);
    const std::string binary = "\x00\x01\x02hello";
    ASSERT_EQ(vfs.addFile(u8"geoms/base.bin", bytesOf(binary)), IoError::Ok);

    const auto xml = vfs.read(u8"workcell.xml");
    ASSERT_TRUE(xml.ok());
    EXPECT_EQ(xml.value(), bytesOf("<workcell name=\"demo\"/>"));

    const auto bin = vfs.read(u8"geoms/base.bin");
    ASSERT_TRUE(bin.ok());
    EXPECT_EQ(bin.value(), bytesOf(binary));

    // Directory semantics.
    EXPECT_TRUE(vfs.exists(u8""));
    EXPECT_TRUE(vfs.exists(u8"workcell.xml"));
    EXPECT_TRUE(vfs.exists(u8"geoms"));
    EXPECT_TRUE(vfs.isFile(u8"workcell.xml"));
    EXPECT_TRUE(vfs.isDirectory(u8"geoms"));
    EXPECT_FALSE(vfs.isFile(u8"geoms"));
    EXPECT_FALSE(vfs.exists(u8"nope"));

    EXPECT_EQ(vfs.read(u8"geoms").error(), IoError::IsADirectory);
    EXPECT_EQ(vfs.read(u8"nope").error(), IoError::NotFound);
}

TEST(VfsTest, ZipListAndRemove)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("1")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b/c.txt", bytesOf("2")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b/d.txt", bytesOf("3")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"e/f/g.txt", bytesOf("4")), IoError::Ok);

    const auto top = vfs.list(u8"");
    ASSERT_TRUE(top.ok());
    ASSERT_EQ(top->size(), 3u); // a.txt, b, e
    const auto top_names = sortedNames(top.value());
    EXPECT_TRUE(contains(top_names, std::filesystem::path(u8"a.txt")));
    EXPECT_TRUE(contains(top_names, std::filesystem::path(u8"b")));
    EXPECT_TRUE(contains(top_names, std::filesystem::path(u8"e")));

    const auto in_b = vfs.list(u8"b");
    ASSERT_TRUE(in_b.ok());
    EXPECT_EQ(in_b->size(), 2u);

    // A directory with entries cannot be removed one level at a time.
    EXPECT_EQ(vfs.remove(u8"b"), IoError::NotEmpty);
    ASSERT_EQ(vfs.removeAll(u8"b"), IoError::Ok);
    EXPECT_FALSE(vfs.exists(u8"b"));
    EXPECT_TRUE(vfs.exists(u8"a.txt"));

    // A single file is removed by remove().
    ASSERT_EQ(vfs.remove(u8"a.txt"), IoError::Ok);
    EXPECT_FALSE(vfs.exists(u8"a.txt"));
    EXPECT_EQ(vfs.remove(u8"a.txt"), IoError::NotFound);
}

TEST(VfsTest, ZipSaveOpenFileRoundTrip)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "pkg.zip";

    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell name=\"demo\"/>")), IoError::Ok);
        ASSERT_EQ(vfs.addFile(u8"devices/robot.vdev", bytesOf("<device name=\"robot\"/>")), IoError::Ok);
        ASSERT_EQ(vfs.saveAs(pkg), IoError::Ok);
    }

    auto opened = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto xml = opened->read(u8"workcell.xml");
    ASSERT_TRUE(xml.ok());
    EXPECT_EQ(xml.value(), bytesOf("<workcell name=\"demo\"/>"));
    EXPECT_TRUE(opened->isDirectory(u8"devices"));
    EXPECT_TRUE(opened->exists(u8"devices/robot.vdev"));
}

TEST(VfsTest, ZipSaveMemoryRoundTrip)
{
    std::vector<unsigned char> pkg;
    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
        const auto bytes = vfs.toBytes();
        ASSERT_TRUE(bytes.ok());
        pkg = bytes.value();
        EXPECT_FALSE(pkg.empty());
    }

    auto opened = ZipArchive::open(std::move(pkg), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto xml = opened->read(u8"workcell.xml");
    ASSERT_TRUE(xml.ok());
    EXPECT_EQ(xml.value(), bytesOf("<workcell/>"));
}

TEST(VfsTest, ZipSaveStreamRoundTrip)
{
    std::ostringstream stream;
    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("hello")), IoError::Ok);
        ASSERT_EQ(vfs.saveAs(stream), IoError::Ok);
    }
    const std::string data = stream.str();
    ASSERT_FALSE(data.empty());

    auto opened = ZipArchive::open(std::vector<unsigned char>(data.begin(), data.end()), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto out = opened->read(u8"a.txt");
    ASSERT_TRUE(out.ok());
    EXPECT_EQ(out.value(), bytesOf("hello"));
}

TEST(VfsTest, ZipAddFileFromDisk)
{
    const TempDir temp;
    const auto    src = temp.path() / "mesh.bin";
    {
        std::ofstream out(src, std::ios::binary);
        out << "mesh-bytes";
    }

    std::vector<unsigned char> pkg;
    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"geoms/mesh.bin", src), IoError::Ok);
        EXPECT_EQ(vfs.stat(u8"geoms/mesh.bin")->size, 10u);
        const auto bytes = vfs.toBytes();
        ASSERT_TRUE(bytes.ok());
        pkg = bytes.value();
    }

    auto opened = ZipArchive::open(std::move(pkg), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto mesh = opened->read(u8"geoms/mesh.bin");
    ASSERT_TRUE(mesh.ok());
    EXPECT_EQ(mesh.value(), bytesOf("mesh-bytes"));
}

TEST(VfsTest, DirectoryVfsRoundTrip)
{
    const TempDir     temp;
    const auto        root = temp.path() / "tree";
    std::error_code   ec;
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);

    auto dir = DirectoryVfs::openDirectory(root);
    ASSERT_NE(dir, nullptr);
    ASSERT_EQ(dir->addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
    ASSERT_EQ(dir->addFile(u8"geoms/a.bin", bytesOf("abc")), IoError::Ok);

    const auto xml = dir->read(u8"workcell.xml");
    ASSERT_TRUE(xml.ok());
    EXPECT_EQ(xml.value(), bytesOf("<workcell/>"));
    EXPECT_TRUE(dir->isDirectory(u8"geoms"));
    EXPECT_TRUE(dir->isFile(u8"geoms/a.bin"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "workcell.xml", ec));

    // A directory backend cannot produce an archive.
    EXPECT_EQ(dir->toBytes().error(), IoError::Unsupported);
    EXPECT_EQ(dir->saveAs(root / "pkg.zip"), IoError::Unsupported);
    // A directory target means the tree is already persisted.
    EXPECT_EQ(dir->commit(), IoError::Ok);

    // Consistency with the zip backend: the same tree yields the same entries.
    std::vector<unsigned char> pkg;
    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
        ASSERT_EQ(vfs.addFile(u8"geoms/a.bin", bytesOf("abc")), IoError::Ok);
        const auto bytes = vfs.toBytes();
        ASSERT_TRUE(bytes.ok());
        pkg = bytes.value();
    }
    auto zip = ZipArchive::open(std::move(pkg), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(zip.ok());

    const auto dir_children = dir->list(u8"");
    const auto zip_children = zip->list(u8"");
    ASSERT_TRUE(dir_children.ok());
    ASSERT_TRUE(zip_children.ok());
    EXPECT_EQ(sortedNames(dir_children.value()), sortedNames(zip_children.value()));
}
