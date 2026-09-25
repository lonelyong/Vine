#include <gtest/gtest.h>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vine/String.hpp>
#include <vine/io/MountVfs.hpp>
#include <vine/io/ZipArchive.hpp>

using vn::io::IoError;
using vn::io::MountVfs;
using vn::io::Result;
using vn::io::VfsEntryInfo;
using vn::io::ZipArchive;

namespace
{

/**
 * @brief Views a string as the bytes a VFS write takes.
 */
std::span<const unsigned char> asBytes(const std::string& text)
{
    return std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

/**
 * @brief Reads a whole entry as text.
 */
std::string textOf(const Result<std::vector<unsigned char>>& bytes)
{
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) {
        return {};
    }
    const std::vector<unsigned char>& data = bytes.value();
    return std::string(data.begin(), data.end());
}

/**
 * @brief An empty in-memory ZIP: a writable tree with nothing in it yet.
 */
std::shared_ptr<ZipArchive> writableZip()
{
    return std::make_shared<ZipArchive>();
}

/**
 * @brief Encodes files into ZIP bytes.
 */
std::vector<unsigned char> zipBytesWith(std::initializer_list<std::pair<const char8_t*, std::string>> files)
{
    ZipArchive staging;
    for (const std::pair<const char8_t*, std::string>& file : files) {
        EXPECT_EQ(staging.addFile(file.first, asBytes(file.second)), IoError::Ok);
    }
    Result<std::vector<unsigned char>> bytes = staging.toBytes();
    EXPECT_TRUE(bytes.ok());
    if (!bytes.ok()) {
        return {};
    }
    return bytes.take();
}

/**
 * @brief Builds an in-memory ZIP that only reads.
 */
std::shared_ptr<ZipArchive> readOnlyZip(std::initializer_list<std::pair<const char8_t*, std::string>> files)
{
    Result<ZipArchive> opened = ZipArchive::open(zipBytesWith(files), ZipArchive::OpenMode::ReadOnly);
    EXPECT_TRUE(opened.ok());
    if (!opened.ok()) {
        return nullptr;
    }
    return std::make_shared<ZipArchive>(opened.take());
}

/**
 * @brief Finds one child by its full virtual path.
 */
const VfsEntryInfo* findChild(const std::vector<VfsEntryInfo>& children, const char8_t* path)
{
    for (const VfsEntryInfo& info : children) {
        if (info.path == std::filesystem::path(path)) {
            return &info;
        }
    }
    return nullptr;
}

} // namespace

TEST(MountVfsTest, MountRejectsInvalidRequests)
{
    MountVfs tree;

    EXPECT_EQ(tree.mount(u8"/rooted", writableZip()), IoError::InvalidPath);
    EXPECT_EQ(tree.mount(u8"", nullptr), IoError::InvalidData);
    EXPECT_EQ(tree.mount(u8"ok/path//x/", writableZip()), IoError::Ok);

    // Mount points are spelled in the normalized form, so every separator
    // spelling of the prefix reaches the same mount.
    EXPECT_TRUE(tree.stat(u8"ok//path/x").ok());
    EXPECT_TRUE(tree.stat(u8"ok/path/x")->is_directory);
}

TEST(MountVfsTest, MountPriorityShadowsReads)
{
    const auto early = writableZip();
    const auto late  = writableZip();
    const auto high  = writableZip();
    ASSERT_EQ(early->addFile(u8"data/x.txt", asBytes("early")), IoError::Ok);
    ASSERT_EQ(early->addFile(u8"data/y.txt", asBytes("early-y")), IoError::Ok);
    ASSERT_EQ(late->addFile(u8"data/x.txt", asBytes("late")), IoError::Ok);
    ASSERT_EQ(late->addFile(u8"data/y.txt", asBytes("late-y")), IoError::Ok);
    ASSERT_EQ(high->addFile(u8"data/x.txt", asBytes("high")), IoError::Ok);

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"", early), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"", late), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"", high, /*priority=*/10), IoError::Ok);

    // Priority wins over mount order; equal priorities keep mount order.
    EXPECT_EQ(textOf(tree.read(u8"data/x.txt")), "high");
    EXPECT_EQ(textOf(tree.read(u8"data/y.txt")), "early-y");
    EXPECT_EQ(tree.read(u8"data/absent.txt").error(), IoError::NotFound);
    EXPECT_EQ(tree.stat(u8"data/x.txt")->size, 4U);
    EXPECT_EQ(tree.stat(u8"data/x.txt")->path, std::filesystem::path(u8"data/x.txt"));
}

TEST(MountVfsTest, MountMergesListsAndSynthesizesIntermediateDirectories)
{
    const auto root_a = writableZip();
    const auto root_b = writableZip();
    const auto deep   = writableZip();
    ASSERT_EQ(root_a->addFile(u8"data/a.txt", asBytes("a")), IoError::Ok);
    ASSERT_EQ(root_b->addFile(u8"data/b.txt", asBytes("b")), IoError::Ok);
    ASSERT_EQ(deep->addFile(u8"z.txt", asBytes("z")), IoError::Ok);

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"", root_a), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"", root_b), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"data/deep", deep), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"data/deep/inner", writableZip()), IoError::Ok);

    // Both claims on "data" merge, and a deeper mount contributes its directory.
    const Result<std::vector<VfsEntryInfo>> root_list = tree.list(u8"");
    ASSERT_TRUE(root_list.ok());
    ASSERT_NE(findChild(*root_list, u8"data"), nullptr);
    EXPECT_TRUE(findChild(*root_list, u8"data")->is_directory);

    const Result<std::vector<VfsEntryInfo>> data_list = tree.list(u8"data");
    ASSERT_TRUE(data_list.ok());
    EXPECT_EQ(data_list->size(), 3U);
    ASSERT_NE(findChild(*data_list, u8"data/a.txt"), nullptr);
    ASSERT_NE(findChild(*data_list, u8"data/b.txt"), nullptr);
    EXPECT_FALSE(findChild(*data_list, u8"data/a.txt")->is_directory);
    ASSERT_NE(findChild(*data_list, u8"data/deep"), nullptr);
    EXPECT_TRUE(findChild(*data_list, u8"data/deep")->is_directory);

    // "data/deep" itself is only implied by the mount at "data/deep/inner".
    const Result<VfsEntryInfo> implied = tree.stat(u8"data/deep");
    ASSERT_TRUE(implied.ok());
    EXPECT_TRUE(implied->is_directory);
    EXPECT_EQ(implied->path, std::filesystem::path(u8"data/deep"));

    // Below the implied directory the mounted backend's own entries show up
    // next to the directory the deeper mount implies.
    const Result<std::vector<VfsEntryInfo>> deep_list = tree.list(u8"data/deep");
    ASSERT_TRUE(deep_list.ok());
    EXPECT_EQ(deep_list->size(), 2U);
    ASSERT_NE(findChild(*deep_list, u8"data/deep/inner"), nullptr);
    EXPECT_TRUE(findChild(*deep_list, u8"data/deep/inner")->is_directory);
    ASSERT_NE(findChild(*deep_list, u8"data/deep/z.txt"), nullptr);
    EXPECT_FALSE(findChild(*deep_list, u8"data/deep/z.txt")->is_directory);
    EXPECT_EQ(textOf(tree.read(u8"data/deep/z.txt")), "z");

    // The empty mount at "data/deep/inner" answers for itself: it lists nothing.
    const Result<std::vector<VfsEntryInfo>> leaf_list = tree.list(u8"data/deep/inner");
    ASSERT_TRUE(leaf_list.ok());
    EXPECT_TRUE(leaf_list->empty());

    EXPECT_EQ(tree.list(u8"data/absent").error(), IoError::NotFound);
    EXPECT_EQ(tree.list(u8"data/a.txt").error(), IoError::NotADirectory);
}

TEST(MountVfsTest, MountRoutesWritesToTheOwningOrFirstWritableBackend)
{
    const auto package = readOnlyZip({ { u8"pkg.txt", "original" }, { u8"readme.txt", "ro" } });
    const auto overlay = writableZip();
    ASSERT_NE(package, nullptr);

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"", package), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"", overlay, /*priority=*/1), IoError::Ok);
    EXPECT_FALSE(tree.isReadOnly());

    // A read falls through the overlay into the package underneath it.
    EXPECT_EQ(textOf(tree.read(u8"pkg.txt")), "original");

    // A new path goes to the first writable mount of the group.
    ASSERT_EQ(tree.addFile(u8"new.txt", asBytes("fresh")), IoError::Ok);
    EXPECT_TRUE(overlay->stat(u8"new.txt").ok());
    EXPECT_EQ(package->stat(u8"new.txt").error(), IoError::NotFound);
    EXPECT_EQ(textOf(tree.read(u8"new.txt")), "fresh");

    ASSERT_EQ(tree.createDirectories(u8"newdir/sub"), IoError::Ok);
    EXPECT_TRUE(overlay->stat(u8"newdir/sub")->is_directory);
    EXPECT_EQ(tree.remove(u8"newdir/sub"), IoError::Ok);

    // A path the read-only owner holds is refused where it is - it is NOT
    // routed to the writable mount, so a reader cannot lose sight of a write.
    EXPECT_EQ(tree.addFile(u8"pkg.txt", asBytes("patched")), IoError::ReadOnly);
    EXPECT_EQ(overlay->stat(u8"pkg.txt").error(), IoError::NotFound);
    EXPECT_EQ(textOf(tree.read(u8"pkg.txt")), "original");

    // The patch workflow writes the file where the next read looks.
    ASSERT_EQ(overlay->addFile(u8"pkg.txt", asBytes("patched")), IoError::Ok);
    EXPECT_EQ(textOf(tree.read(u8"pkg.txt")), "patched");
}

TEST(MountVfsTest, MountRenamesWithinAndAcrossBackends)
{
    const auto one = writableZip();
    const auto two = writableZip();

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"one", one), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"two", two), IoError::Ok);
    ASSERT_EQ(tree.addFile(u8"one/f.txt", asBytes("content")), IoError::Ok);
    ASSERT_EQ(tree.addFile(u8"two/g.txt", asBytes("occupied")), IoError::Ok);

    // Inside one backend the rename is native.
    ASSERT_EQ(tree.rename(u8"one/f.txt", u8"one/f2.txt"), IoError::Ok);
    EXPECT_EQ(textOf(tree.read(u8"one/f2.txt")), "content");
    EXPECT_EQ(tree.stat(u8"one/f.txt").error(), IoError::NotFound);
    EXPECT_TRUE(one->stat(u8"f2.txt").ok());

    // Across backends the content is copied and the source dropped.
    ASSERT_EQ(tree.rename(u8"one/f2.txt", u8"two/copied.txt"), IoError::Ok);
    EXPECT_EQ(textOf(tree.read(u8"two/copied.txt")), "content");
    EXPECT_EQ(tree.stat(u8"one/f2.txt").error(), IoError::NotFound);
    EXPECT_TRUE(two->stat(u8"copied.txt").ok());

    // A directory is not copied piecewise.
    ASSERT_EQ(tree.createDirectory(u8"one/dir"), IoError::Ok);
    EXPECT_EQ(tree.rename(u8"one/dir", u8"two/dir"), IoError::Unsupported);
    EXPECT_TRUE(tree.stat(u8"one/dir").ok());

    // An occupied target is never overwritten, or merged, across backends.
    ASSERT_EQ(tree.addFile(u8"one/local.txt", asBytes("x")), IoError::Ok);
    EXPECT_EQ(tree.rename(u8"one/local.txt", u8"two/g.txt"), IoError::AlreadyExists);
    EXPECT_TRUE(tree.stat(u8"one/local.txt").ok());
    EXPECT_EQ(textOf(tree.read(u8"two/g.txt")), "occupied");
}

TEST(MountVfsTest, MountRefusesWritesWhenNoTargetIsWritable)
{
    const auto package = readOnlyZip({ { u8"p.txt", "p" } });
    const auto scratch = writableZip();
    ASSERT_NE(package, nullptr);

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"", package), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"scratch", scratch, /*priority=*/0, /*read_only=*/true), IoError::Ok);
    EXPECT_TRUE(tree.isReadOnly());

    EXPECT_EQ(tree.addFile(u8"x.txt", asBytes("x")), IoError::ReadOnly);
    EXPECT_EQ(tree.addFile(u8"p.txt", asBytes("x")), IoError::ReadOnly);
    EXPECT_EQ(tree.addFile(u8"scratch/s.txt", asBytes("x")), IoError::ReadOnly);
    EXPECT_EQ(tree.remove(u8"p.txt"), IoError::ReadOnly);
    EXPECT_EQ(tree.removeAll(u8"p.txt"), IoError::ReadOnly);
    EXPECT_EQ(tree.createDirectory(u8"d"), IoError::ReadOnly);
    EXPECT_EQ(tree.createDirectories(u8"d/e"), IoError::ReadOnly);
    EXPECT_EQ(scratch->stat(u8"s.txt").error(), IoError::NotFound);

    // Reading still works, and nothing is left to commit.
    EXPECT_EQ(textOf(tree.read(u8"p.txt")), "p");
    EXPECT_EQ(tree.commit(), IoError::Ok);
    EXPECT_EQ(tree.saveAs(std::filesystem::path(u8"x.zip")), IoError::Unsupported);
    EXPECT_EQ(tree.toBytes().error(), IoError::Unsupported);

    // A rename into a read-only mount keeps the source in place.
    const auto source = writableZip();
    MountVfs mixed;
    ASSERT_EQ(mixed.mount(u8"src", source), IoError::Ok);
    ASSERT_EQ(mixed.mount(u8"dst", writableZip(), /*priority=*/0, /*read_only=*/true), IoError::Ok);
    ASSERT_EQ(mixed.addFile(u8"src/f.txt", asBytes("f")), IoError::Ok);

    EXPECT_EQ(mixed.rename(u8"src/f.txt", u8"dst/f.txt"), IoError::ReadOnly);
    EXPECT_TRUE(mixed.stat(u8"src/f.txt").ok());
    EXPECT_EQ(mixed.stat(u8"dst/f.txt").error(), IoError::NotFound);
}

TEST(MountVfsTest, MountNestsInsideAnotherMount)
{
    const auto inner_zip = writableZip();
    const auto inner     = std::make_shared<MountVfs>();
    ASSERT_EQ(inner->mount(u8"", inner_zip), IoError::Ok);
    ASSERT_EQ(inner->addFile(u8"f.txt", asBytes("inner")), IoError::Ok);

    MountVfs outer;
    ASSERT_EQ(outer.mount(u8"outer", inner), IoError::Ok);

    EXPECT_EQ(textOf(outer.read(u8"outer/f.txt")), "inner");
    EXPECT_EQ(outer.stat(u8"outer/f.txt")->path, std::filesystem::path(u8"outer/f.txt"));

    const Result<std::vector<VfsEntryInfo>> listed = outer.list(u8"outer");
    ASSERT_TRUE(listed.ok());
    ASSERT_NE(findChild(*listed, u8"outer/f.txt"), nullptr);

    // Writes travel through both routers into the innermost backend.
    ASSERT_EQ(outer.addFile(u8"outer/new.txt", asBytes("new")), IoError::Ok);
    EXPECT_TRUE(inner_zip->stat(u8"new.txt").ok());
    EXPECT_EQ(textOf(outer.read(u8"outer/new.txt")), "new");

    // A path no mount covers has nowhere to go.
    EXPECT_EQ(outer.addFile(u8"other/x.txt", asBytes("x")), IoError::ReadOnly);
}

TEST(MountVfsTest, MountLongestPrefixWinsWithoutFallback)
{
    const auto data = writableZip();
    ASSERT_EQ(data->addFile(u8"x.txt", asBytes("root-x")), IoError::Ok);
    ASSERT_EQ(data->addFile(u8"a/inner.txt", asBytes("shadowed")), IoError::Ok);

    const auto hidden = writableZip();
    const auto leaf   = writableZip();

    MountVfs tree;
    ASSERT_EQ(tree.mount(u8"data", data), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"data/a", hidden), IoError::Ok);
    ASSERT_EQ(tree.mount(u8"data/a/b", leaf), IoError::Ok);

    EXPECT_EQ(textOf(tree.read(u8"data/x.txt")), "root-x");
    EXPECT_TRUE(tree.stat(u8"data/a")->is_directory);

    // The mount at "data/a" answers for its whole subtree, so the deeper entry
    // of the mount at "data" is out of sight - nothing falls back to it.
    EXPECT_EQ(tree.read(u8"data/a/inner.txt").error(), IoError::NotFound);
    const Result<std::vector<VfsEntryInfo>> hidden_list = tree.list(u8"data/a");
    ASSERT_TRUE(hidden_list.ok());
    EXPECT_EQ(hidden_list->size(), 1U);
    ASSERT_NE(findChild(*hidden_list, u8"data/a/b"), nullptr);
    EXPECT_TRUE(findChild(*hidden_list, u8"data/a/b")->is_directory);
    EXPECT_TRUE(data->stat(u8"a/inner.txt").ok());

    const Result<std::vector<VfsEntryInfo>> data_list = tree.list(u8"data");
    ASSERT_TRUE(data_list.ok());
    ASSERT_NE(findChild(*data_list, u8"data/a"), nullptr);
    EXPECT_TRUE(findChild(*data_list, u8"data/a")->is_directory);
    EXPECT_NE(findChild(*data_list, u8"data/x.txt"), nullptr);

    // A write under the deepest prefix lands in its mount.
    ASSERT_EQ(tree.addFile(u8"data/a/b/leaf.txt", asBytes("leaf")), IoError::Ok);
    EXPECT_TRUE(leaf->stat(u8"leaf.txt").ok());
    EXPECT_EQ(textOf(tree.read(u8"data/a/b/leaf.txt")), "leaf");
}
