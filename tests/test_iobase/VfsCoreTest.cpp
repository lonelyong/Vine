#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Vfs.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/String.hpp>

#include "VfsTestSupport.hpp"

using vine::String;
using vine::io::DirectoryVfs;
using vine::io::VfsEntryInfo;
using vine::io::VfsEntryKind;
using vine::io::IoError;
using vine::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::findInfo;
using vfstest::TempDir;

namespace
{

/**
 * @brief A backend that claims to be read-only, to test the write guard.
 */
class ReadOnlyProbe : public DirectoryVfs
{
  public:
    using DirectoryVfs::DirectoryVfs;

    [[nodiscard]] bool isReadOnly() const noexcept override { return true; }
};

} // namespace

TEST(VfsCoreTest, ResultCarriesValueOrError)
{
    vine::io::Result<int> good{ 7 };
    EXPECT_TRUE(good.ok());
    EXPECT_TRUE(static_cast<bool>(good));
    EXPECT_EQ(good.error(), IoError::Ok);
    EXPECT_EQ(good.value(), 7);
    EXPECT_EQ(*good, 7);

    vine::io::Result<int> taken{ 9 };
    EXPECT_EQ(taken.take(), 9);

    vine::io::Result<VfsEntryInfo> info{ VfsEntryInfo{ std::filesystem::path(u8"a/b.txt"), false, 3 } };
    EXPECT_EQ(info->path, std::filesystem::path(u8"a/b.txt"));
    EXPECT_EQ(info->size, 3u);

    vine::io::Result<int> bad{ IoError::NotFound };
    EXPECT_FALSE(bad.ok());
    EXPECT_FALSE(static_cast<bool>(bad));
    EXPECT_EQ(bad.error(), IoError::NotFound);
}

TEST(VfsCoreTest, ErrorNamesAreReadable)
{
    EXPECT_STREQ(vine::io::ioErrorName(IoError::Ok), "Ok");
    EXPECT_STREQ(vine::io::ioErrorName(IoError::InvalidPath), "InvalidPath");
    EXPECT_STREQ(vine::io::ioErrorName(IoError::NotEmpty), "NotEmpty");
    EXPECT_STREQ(vine::io::ioErrorName(IoError::CapacityExceeded), "CapacityExceeded");
}

TEST(VfsCoreTest, ZipStatReportsKindAndSize)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"geoms/base.bin", bytesOf("abcd")), IoError::Ok);

    const auto root = vfs.stat(u8"");
    ASSERT_TRUE(root.ok());
    EXPECT_TRUE(root->is_directory);
    EXPECT_EQ(root->path, std::filesystem::path(u8""));
    EXPECT_EQ(root->name(), std::filesystem::path(u8""));
    EXPECT_EQ(root->size, 0u);

    const auto geoms = vfs.stat(u8"geoms");
    ASSERT_TRUE(geoms.ok());
    EXPECT_TRUE(geoms->is_directory);
    EXPECT_EQ(geoms->path, std::filesystem::path(u8"geoms"));
    EXPECT_EQ(geoms->size, 0u);

    const auto leaf = vfs.stat(u8"geoms/base.bin");
    ASSERT_TRUE(leaf.ok());
    EXPECT_FALSE(leaf->is_directory);
    EXPECT_EQ(leaf->path, std::filesystem::path(u8"geoms/base.bin"));
    EXPECT_EQ(leaf->name(), std::filesystem::path(u8"base.bin"));
    EXPECT_EQ(leaf->size, 4u);
    EXPECT_EQ(leaf->crc, 0u); // nothing is recorded until the entry is written out

    EXPECT_EQ(vfs.stat(u8"geoms/nope.bin").error(), IoError::NotFound);
}

TEST(VfsCoreTest, KindOfAnswersFileDirectoryAndMissing)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"geoms/base.bin", bytesOf("abcd")), IoError::Ok);
    ASSERT_EQ(vfs.createDirectory(u8"empty"), IoError::Ok);

    EXPECT_EQ(vfs.kindOf(u8""), VfsEntryKind::Directory);      // the root always exists
    EXPECT_EQ(vfs.kindOf(u8"geoms"), VfsEntryKind::Directory); // implied by its children
    EXPECT_EQ(vfs.kindOf(u8"empty"), VfsEntryKind::Directory);
    EXPECT_EQ(vfs.kindOf(u8"geoms/base.bin"), VfsEntryKind::File);
    EXPECT_EQ(vfs.kindOf(u8"geoms/nope.bin"), VfsEntryKind::Missing);
    EXPECT_EQ(vfs.kindOf(u8"/x"), VfsEntryKind::Missing); // invalid is absent, not a third answer
    EXPECT_EQ(vfs.kindOf(u8"geoms/../empty"), VfsEntryKind::Directory);

    // The directory backend answers the same way.
    const TempDir temp;
    const auto    root = temp.path() / "kind";
    std::error_code ec;
    std::filesystem::create_directories(root / "sub", ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream out(root / "sub" / "file.txt");
        out << "x";
    }

    const auto dir = DirectoryVfs::openDirectory(root);
    ASSERT_NE(dir, nullptr);
    EXPECT_EQ(dir->kindOf(u8""), VfsEntryKind::Directory);
    EXPECT_EQ(dir->kindOf(u8"sub"), VfsEntryKind::Directory);
    EXPECT_EQ(dir->kindOf(u8"sub/file.txt"), VfsEntryKind::File);
    EXPECT_EQ(dir->kindOf(u8"nope"), VfsEntryKind::Missing);
}

TEST(VfsCoreTest, ZipStatSizesImportedFiles)
{
    const TempDir temp;
    const auto    src = temp.path() / "mesh.bin";
    {
        std::ofstream out(src, std::ios::binary);
        out << "mesh-bytes";
    }

    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"geoms/mesh.bin", src), IoError::Ok);

    const auto info = vfs.stat(u8"geoms/mesh.bin");
    ASSERT_TRUE(info.ok());
    EXPECT_FALSE(info->is_directory);
    EXPECT_EQ(info->size, 10u);

    // An import of something that is not there fails right away.
    EXPECT_EQ(vfs.addFile(u8"geoms/nope.bin", temp.path() / "nope.bin"), IoError::NotFound);
}

TEST(VfsCoreTest, ZipListReportsChildren)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("1")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b/c.txt", bytesOf("22")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b/d.txt", bytesOf("333")), IoError::Ok);

    const auto top = vfs.list(u8"");
    ASSERT_TRUE(top.ok());
    ASSERT_EQ(top->size(), 2u);
    ASSERT_NE(findInfo(top.value(), std::filesystem::path(u8"a.txt")), nullptr);
    ASSERT_NE(findInfo(top.value(), std::filesystem::path(u8"b")), nullptr);
    EXPECT_EQ(findInfo(top.value(), std::filesystem::path(u8"a.txt"))->size, 1u);
    EXPECT_TRUE(findInfo(top.value(), std::filesystem::path(u8"b"))->is_directory);
    EXPECT_EQ(findInfo(top.value(), std::filesystem::path(u8"b"))->size, 0u);

    const auto in_b = vfs.list(u8"b");
    ASSERT_TRUE(in_b.ok());
    ASSERT_EQ(in_b->size(), 2u);
    ASSERT_NE(findInfo(in_b.value(), std::filesystem::path(u8"b/c.txt")), nullptr);
    EXPECT_EQ(findInfo(in_b.value(), std::filesystem::path(u8"b/c.txt"))->size, 2u);
    EXPECT_EQ(findInfo(in_b.value(), std::filesystem::path(u8"b/d.txt"))->size, 3u);

    EXPECT_EQ(vfs.list(u8"a.txt").error(), IoError::NotADirectory);
    EXPECT_EQ(vfs.list(u8"nope").error(), IoError::NotFound);
}

TEST(VfsCoreTest, InvalidPathsAreRejected)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("1")), IoError::Ok);

    // A leading '/' is refused too: a virtual path has no working directory, so
    // there is nothing an absolute spelling could name.
    const std::vector<std::filesystem::path> invalid{ std::filesystem::path(u8".."),
                                       std::filesystem::path(u8"a/../.."),
                                       std::filesystem::path(u8"../../x"),
                                       std::filesystem::path(u8"C:/escape.txt"),
                                       std::filesystem::path(u8"a\\b"),
                                       std::filesystem::path(u8"/a.txt"),
                                       std::filesystem::path(u8"/") };
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        const std::filesystem::path& path = invalid[i];
        EXPECT_EQ(vfs.stat(path).error(), IoError::InvalidPath) << "bad path #" << i;
        EXPECT_EQ(vfs.list(path).error(), IoError::InvalidPath) << "bad path #" << i;
        EXPECT_EQ(vfs.read(path).error(), IoError::InvalidPath) << "bad path #" << i;
        EXPECT_EQ(vfs.addFile(path, bytesOf("x")), IoError::InvalidPath) << "bad path #" << i;
        EXPECT_FALSE(vfs.exists(path)) << "bad path #" << i;
        EXPECT_FALSE(vfs.isDirectory(path)) << "bad path #" << i;
    }

    // An invalid path must never create a file.
    EXPECT_FALSE(vfs.exists(std::filesystem::path(u8"C:/escape.txt")));

    // ".." inside a segment is an ordinary name, not a traversal.
    ASSERT_EQ(vfs.addFile(u8"dir/a..b", bytesOf("x")), IoError::Ok);
    EXPECT_EQ(vfs.stat(u8"dir/a..b")->path, std::filesystem::path(u8"dir/a..b"));

    // "." and repeated separators are folded rather than rejected.
    EXPECT_EQ(vfs.stat(u8"a/../a.txt")->path, std::filesystem::path(u8"a.txt"));
    EXPECT_EQ(vfs.stat(u8"./a.txt").error(), IoError::Ok);
    EXPECT_EQ(vfs.stat(u8"a//b//c.txt").error(), IoError::NotFound);
}

TEST(VfsCoreTest, DirectoryStatAndList)
{
    const TempDir   temp;
    const auto      root = temp.path() / "tree";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);

    auto dir = DirectoryVfs::openDirectory(root);
    ASSERT_NE(dir, nullptr);
    ASSERT_EQ(dir->addFile(u8"workcell.xml", bytesOf("<workcell/>")), IoError::Ok);
    ASSERT_EQ(dir->addFile(u8"geoms/a.bin", bytesOf("abc")), IoError::Ok);

    const auto xml = dir->stat(u8"workcell.xml");
    ASSERT_TRUE(xml.ok());
    EXPECT_FALSE(xml->is_directory);
    EXPECT_EQ(xml->size, 11u);
    EXPECT_EQ(xml->path, std::filesystem::path(u8"workcell.xml"));

    const auto root_info = dir->stat(u8"");
    ASSERT_TRUE(root_info.ok());
    EXPECT_TRUE(root_info->is_directory);
    EXPECT_EQ(dir->stat(u8"nope").error(), IoError::NotFound);

    const auto children = dir->list(u8"");
    ASSERT_TRUE(children.ok());
    ASSERT_EQ(children->size(), 2u);
    const VfsEntryInfo* geoms = findInfo(children.value(), std::filesystem::path(u8"geoms"));
    ASSERT_NE(geoms, nullptr);
    EXPECT_TRUE(geoms->is_directory);
    EXPECT_EQ(geoms->size, 0u);
    const VfsEntryInfo* leaf = findInfo(children.value(), std::filesystem::path(u8"workcell.xml"));
    ASSERT_NE(leaf, nullptr);
    EXPECT_FALSE(leaf->is_directory);
    EXPECT_EQ(leaf->size, 11u);

    const auto in_geoms = dir->list(u8"geoms");
    ASSERT_TRUE(in_geoms.ok());
    ASSERT_EQ(in_geoms->size(), 1u);
    EXPECT_EQ((*in_geoms)[0].path, std::filesystem::path(u8"geoms/a.bin"));
    EXPECT_EQ((*in_geoms)[0].size, 3u);

    EXPECT_EQ(dir->list(u8"workcell.xml").error(), IoError::NotADirectory);
    EXPECT_EQ(dir->list(u8"nope").error(), IoError::NotFound);
}

TEST(VfsCoreTest, DirectoryVfsNeverEscapesItsRoot)
{
    const TempDir root;
    const TempDir outside;

    auto dir = DirectoryVfs::openDirectory(root.path());
    ASSERT_NE(dir, nullptr);

    // Joining an absolute path with std::filesystem *replaces* the root, so it
    // has to be rejected before the path reaches the filesystem.
    const std::filesystem::path escape{ (outside.path() / "escaped.txt").u8string() };
    EXPECT_EQ(dir->addFile(escape, bytesOf("x")), IoError::InvalidPath);
    EXPECT_EQ(dir->addFile(escape, bytesOf("x")), IoError::InvalidPath);
    EXPECT_EQ(dir->stat(escape).error(), IoError::InvalidPath);
    EXPECT_EQ(dir->addFile(escape, outside.path() / "escaped.txt"), IoError::InvalidPath);

    // Plain traversal is rejected too.
    EXPECT_EQ(dir->addFile(u8"../escaped.txt", bytesOf("x")), IoError::InvalidPath);
    EXPECT_FALSE(std::filesystem::exists(outside.path() / "escaped.txt"));
}

TEST(VfsCoreTest, DirectoryBackendRefusesArchives)
{
    const TempDir temp;

    auto dir = DirectoryVfs::openDirectory(temp.path());
    ASSERT_NE(dir, nullptr);

    EXPECT_FALSE(dir->isReadOnly());
    EXPECT_EQ(dir->toBytes().error(), IoError::Unsupported);
    EXPECT_EQ(dir->saveAs(temp.path() / "pkg.zip"), IoError::Unsupported);

    std::ostringstream stream;
    EXPECT_EQ(dir->saveAs(stream), IoError::Unsupported);

    // A directory target means "already persisted", because writes are immediate.
    EXPECT_EQ(dir->commit(), IoError::Ok);
}

TEST(VfsCoreTest, ZipBackendRoundTripsArchives)
{
    ZipArchive vfs;
    EXPECT_FALSE(vfs.isReadOnly());
    ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("hello")), IoError::Ok);

    auto bytes = vfs.toBytes();
    ASSERT_TRUE(bytes.ok());

    auto opened = ZipArchive::open(bytes.take(), ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto text = opened->read(u8"a.txt");
    ASSERT_TRUE(text.ok());
    EXPECT_EQ(text.value(), bytesOf("hello"));
}

TEST(VfsCoreTest, ReadOnlyBackendRefusesEveryChange)
{
    const TempDir temp;
    ReadOnlyProbe probe(temp.path());
    ASSERT_TRUE(probe.isReadOnly());

    EXPECT_EQ(probe.addFile(u8"new/deep.txt", bytesOf("x")), IoError::ReadOnly);
    EXPECT_EQ(probe.createDirectory(u8"new"), IoError::ReadOnly);
    EXPECT_EQ(probe.createDirectories(u8"new/deep"), IoError::ReadOnly);
    EXPECT_EQ(probe.rename(u8"a", u8"b"), IoError::ReadOnly);
    EXPECT_EQ(probe.remove(u8"a"), IoError::ReadOnly);
    EXPECT_EQ(probe.removeAll(u8"a"), IoError::ReadOnly);

    EXPECT_FALSE(std::filesystem::exists(temp.path() / "new"));
}

TEST(VfsCoreTest, ZipCreateDirectoryIsStrict)
{
    ZipArchive vfs;
    EXPECT_EQ(vfs.createDirectory(u8""), IoError::AlreadyExists); // the root always exists

    ASSERT_EQ(vfs.createDirectory(u8"one"), IoError::Ok);
    EXPECT_TRUE(vfs.isDirectory(u8"one"));
    EXPECT_FALSE(vfs.isFile(u8"one"));
    EXPECT_EQ(vfs.createDirectory(u8"one"), IoError::AlreadyExists);

    // The parent has to be there already.
    EXPECT_EQ(vfs.createDirectory(u8"two/three"), IoError::NotFound);

    // A file blocks the name, and an ancestor file is a different story.
    ASSERT_EQ(vfs.addFile(u8"file.txt", bytesOf("x")), IoError::Ok);
    EXPECT_EQ(vfs.createDirectory(u8"file.txt"), IoError::AlreadyExists);
    EXPECT_EQ(vfs.createDirectory(u8"file.txt/deep"), IoError::NotADirectory);
}

TEST(VfsCoreTest, ZipCreateDirectoriesMakesTheWholeChain)
{
    ZipArchive vfs;
    EXPECT_EQ(vfs.createDirectories(u8""), IoError::Ok); // the root always exists

    ASSERT_EQ(vfs.createDirectories(u8"two/three"), IoError::Ok);
    EXPECT_TRUE(vfs.isDirectory(u8"two"));
    EXPECT_TRUE(vfs.isDirectory(u8"two/three"));
    EXPECT_EQ(vfs.createDirectories(u8"two/three"), IoError::Ok); // idempotent

    const auto top = vfs.list(u8"");
    ASSERT_TRUE(top.ok());
    ASSERT_EQ(top->size(), 1u);
    EXPECT_EQ((*top)[0].path, std::filesystem::path(u8"two"));

    ASSERT_EQ(vfs.addFile(u8"file.txt", bytesOf("x")), IoError::Ok);
    EXPECT_EQ(vfs.createDirectories(u8"file.txt"), IoError::AlreadyExists);
    EXPECT_EQ(vfs.createDirectories(u8"file.txt/deep"), IoError::NotADirectory);
}

TEST(VfsCoreTest, ZipWriteOverDirectoryIsRefused)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.createDirectory(u8"empty"), IoError::Ok);
    EXPECT_EQ(vfs.addFile(u8"empty", bytesOf("x")), IoError::IsADirectory);
    EXPECT_EQ(vfs.addFile(u8"empty/a.txt", bytesOf("x")), IoError::Ok); // inside one is fine

    ASSERT_EQ(vfs.addFile(u8"dir/leaf.txt", bytesOf("1")), IoError::Ok);
    EXPECT_EQ(vfs.addFile(u8"dir", bytesOf("x")), IoError::IsADirectory); // "dir" exists implicitly
    EXPECT_TRUE(vfs.isFile(u8"dir/leaf.txt"));

    // The root is a directory as well.
    EXPECT_EQ(vfs.addFile(u8"", bytesOf("x")), IoError::IsADirectory);
}

TEST(VfsCoreTest, ZipRemoveAndRemoveAll)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"dir/leaf.txt", bytesOf("1")), IoError::Ok);
    ASSERT_EQ(vfs.createDirectory(u8"empty"), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"plain.txt", bytesOf("1")), IoError::Ok);

    EXPECT_EQ(vfs.remove(u8""), IoError::InvalidPath); // the root cannot be removed
    EXPECT_EQ(vfs.remove(u8"missing"), IoError::NotFound);
    EXPECT_EQ(vfs.remove(u8"dir"), IoError::NotEmpty);
    EXPECT_EQ(vfs.removeAll(u8"missing"), IoError::NotFound);

    ASSERT_EQ(vfs.remove(u8"empty"), IoError::Ok);
    EXPECT_FALSE(vfs.exists(u8"empty"));
    EXPECT_EQ(vfs.remove(u8"empty"), IoError::NotFound);

    ASSERT_EQ(vfs.remove(u8"plain.txt"), IoError::Ok);
    EXPECT_FALSE(vfs.exists(u8"plain.txt"));

    ASSERT_EQ(vfs.removeAll(u8"dir"), IoError::Ok);
    EXPECT_FALSE(vfs.exists(u8"dir"));
    EXPECT_FALSE(vfs.exists(u8"dir/leaf.txt"));
}

TEST(VfsCoreTest, ZipRenameMovesAWholeSubtree)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"b/c.txt", bytesOf("22")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b/d.txt", bytesOf("333")), IoError::Ok);
    ASSERT_EQ(vfs.createDirectory(u8"b/sub"), IoError::Ok);

    ASSERT_EQ(vfs.rename(u8"b", u8"renamed"), IoError::Ok);

    EXPECT_EQ(vfs.stat(u8"b").error(), IoError::NotFound);
    const auto leaf = vfs.stat(u8"renamed/c.txt");
    ASSERT_TRUE(leaf.ok());
    EXPECT_EQ(leaf->size, 2u);
    const auto sub = vfs.stat(u8"renamed/sub");
    ASSERT_TRUE(sub.ok());
    EXPECT_TRUE(sub->is_directory);

    const auto children = vfs.list(u8"renamed");
    ASSERT_TRUE(children.ok());
    EXPECT_EQ(children->size(), 3u);
}

TEST(VfsCoreTest, ZipRenameRejectsBadTargets)
{
    ZipArchive vfs;
    ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("1")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"b.txt", bytesOf("2")), IoError::Ok);
    ASSERT_EQ(vfs.addFile(u8"dir/leaf.txt", bytesOf("3")), IoError::Ok);

    EXPECT_EQ(vfs.rename(u8"missing.txt", u8"x.txt"), IoError::NotFound);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8"b.txt"), IoError::AlreadyExists);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8"dir"), IoError::AlreadyExists);
    EXPECT_EQ(vfs.rename(u8"dir", u8"dir/nested"), IoError::InvalidPath);
    EXPECT_EQ(vfs.rename(u8"", u8"x"), IoError::InvalidPath);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8""), IoError::InvalidPath);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8"../x.txt"), IoError::InvalidPath);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8"nope/x.txt"), IoError::NotFound);
    EXPECT_EQ(vfs.rename(u8"a.txt", u8"a.txt"), IoError::Ok); // nothing to do

    EXPECT_TRUE(vfs.exists(u8"a.txt"));
    EXPECT_TRUE(vfs.exists(u8"dir/leaf.txt"));
}

TEST(VfsCoreTest, ZipDirectoryEntriesSurviveSaveAndOpen)
{
    const TempDir temp;
    const auto    pkg = temp.path() / "dirs.zip";
    {
        ZipArchive vfs;
        ASSERT_EQ(vfs.addFile(u8"a.txt", bytesOf("1")), IoError::Ok);
        ASSERT_EQ(vfs.createDirectory(u8"empty"), IoError::Ok);
        ASSERT_EQ(vfs.createDirectories(u8"nested/deep"), IoError::Ok);
        ASSERT_EQ(vfs.saveAs(pkg), IoError::Ok);
    }

    auto opened = ZipArchive::open(pkg, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());

    const auto empty = opened->stat(u8"empty");
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty->is_directory);
    const auto deep = opened->stat(u8"nested/deep");
    ASSERT_TRUE(deep.ok());
    EXPECT_TRUE(deep->is_directory);

    const auto children = opened->list(u8"");
    ASSERT_TRUE(children.ok());
    ASSERT_EQ(children->size(), 3u);
    ASSERT_NE(findInfo(children.value(), std::filesystem::path(u8"empty")), nullptr);
    EXPECT_TRUE(findInfo(children.value(), std::filesystem::path(u8"empty"))->is_directory);
    ASSERT_NE(findInfo(children.value(), std::filesystem::path(u8"nested")), nullptr);
    EXPECT_TRUE(findInfo(children.value(), std::filesystem::path(u8"nested"))->is_directory);
    EXPECT_EQ(findInfo(children.value(), std::filesystem::path(u8"a.txt"))->size, 1u);
}

TEST(VfsCoreTest, DirectoryCreateRenameRemove)
{
    const TempDir temp;

    auto dir = DirectoryVfs::openDirectory(temp.path());
    ASSERT_NE(dir, nullptr);

    EXPECT_EQ(dir->createDirectory(u8""), IoError::AlreadyExists);
    ASSERT_EQ(dir->createDirectory(u8"one"), IoError::Ok);
    EXPECT_TRUE(dir->isDirectory(u8"one"));
    EXPECT_EQ(dir->createDirectory(u8"one"), IoError::AlreadyExists);

    EXPECT_EQ(dir->createDirectory(u8"two/three"), IoError::NotFound);
    ASSERT_EQ(dir->createDirectories(u8"two/three"), IoError::Ok);
    const auto deep = dir->stat(u8"two/three");
    ASSERT_TRUE(deep.ok());
    EXPECT_TRUE(deep->is_directory);

    // Rename a directory, then a file inside it.
    ASSERT_EQ(dir->rename(u8"two", u8"moved"), IoError::Ok);
    EXPECT_EQ(dir->stat(u8"two").error(), IoError::NotFound);
    const auto moved_three = dir->stat(u8"moved/three");
    ASSERT_TRUE(moved_three.ok());
    EXPECT_TRUE(moved_three->is_directory);

    ASSERT_EQ(dir->addFile(u8"moved/leaf.txt", bytesOf("abc")), IoError::Ok);
    ASSERT_EQ(dir->rename(u8"moved/leaf.txt", u8"moved/renamed.txt"), IoError::Ok);
    EXPECT_FALSE(dir->exists(u8"moved/leaf.txt"));
    EXPECT_TRUE(dir->isFile(u8"moved/renamed.txt"));

    EXPECT_EQ(dir->rename(u8"missing", u8"x"), IoError::NotFound);
    EXPECT_EQ(dir->rename(u8"one", u8"moved"), IoError::AlreadyExists);
    EXPECT_EQ(dir->rename(u8"moved", u8"moved/nested"), IoError::InvalidPath);
    EXPECT_EQ(dir->rename(u8"one", u8"nope/x"), IoError::NotFound);

    // A file blocks the way.
    ASSERT_EQ(dir->addFile(u8"blocker", bytesOf("1")), IoError::Ok);
    EXPECT_EQ(dir->createDirectory(u8"blocker"), IoError::AlreadyExists);
    EXPECT_EQ(dir->createDirectories(u8"blocker/deep"), IoError::NotADirectory);

    EXPECT_EQ(dir->remove(u8""), IoError::InvalidPath);
    EXPECT_EQ(dir->remove(u8"missing"), IoError::NotFound);
    EXPECT_EQ(dir->remove(u8"moved"), IoError::NotEmpty);
    ASSERT_EQ(dir->remove(u8"blocker"), IoError::Ok); // a file is removed by remove()
    EXPECT_FALSE(dir->exists(u8"blocker"));

    ASSERT_EQ(dir->remove(u8"one"), IoError::Ok);
    EXPECT_FALSE(dir->exists(u8"one"));

    ASSERT_EQ(dir->removeAll(u8"moved"), IoError::Ok);
    EXPECT_FALSE(dir->exists(u8"moved"));
    EXPECT_EQ(dir->remove(u8"moved"), IoError::NotFound);
}
