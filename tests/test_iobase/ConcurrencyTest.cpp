#include <gtest/gtest.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <vine/io/DirectoryVfs.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/ZipArchive.hpp>

#include "VfsTestSupport.hpp"

using vn::io::DirectoryVfs;
using vn::io::IoError;
using vn::io::ZipArchive;
using vfstest::bytesOf;
using vfstest::TempDir;

namespace
{

/**
 * @brief Runs one thread's worth of VFS work over storage of its own.
 *
 * Every object here is thread-confined: a real directory, an in-memory archive,
 * the streams taken from them and the bytes handed over. Only the returned
 * string leaves the thread.
 *
 * @param index Distinguishes the payload, so content mixed up between threads shows.
 * @return "ok" when every read returned what was written, otherwise the first failure.
 */
std::string exerciseOwnStorage(int index)
{
    const TempDir dir;

    std::unique_ptr<DirectoryVfs> folder = DirectoryVfs::openDirectory(dir.path());
    if (folder == nullptr) {
        return "no directory backend";
    }
    const std::string text = "thread " + std::to_string(index) + " content";
    if (folder->addFile(u8"note.txt", bytesOf(text)) != IoError::Ok) {
        return "folder write failed";
    }
    const auto read_back = folder->read(u8"note.txt");
    if (!read_back.ok()) {
        return "folder read failed";
    }
    if (std::string(read_back->begin(), read_back->end()) != text) {
        return "folder content changed";
    }

    // An in-memory archive touches the compression library through its own handle.
    ZipArchive archive;
    const std::string filler(200, static_cast<char>('a' + index));
    if (archive.addFile(u8"a.bin", bytesOf(text)) != IoError::Ok) {
        return "archive write failed";
    }
    if (archive.addFile(u8"sub/b.bin", bytesOf(filler)) != IoError::Ok) {
        return "nested archive write failed";
    }

    auto serialized = archive.toBytes();
    if (!serialized.ok()) {
        return "archive serialize failed";
    }
    const auto reopened = ZipArchive::open(serialized.take(), ZipArchive::OpenMode::ReadOnly);
    if (!reopened.ok()) {
        return "archive reopen failed";
    }
    const auto a = reopened->read(u8"a.bin");
    const auto b = reopened->read(u8"sub/b.bin");
    if (!a.ok() || !b.ok()) {
        return "archive read failed";
    }
    if (std::string(a->begin(), a->end()) != text || std::string(b->begin(), b->end()) != filler) {
        return "archive content changed";
    }
    return "ok";
}

} // namespace

TEST(ConcurrencyTest, VfsInstancesRunInParallelOverDistinctStorage)
{
    constexpr int threads = 4;

    std::vector<std::string> results(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([t, &results] {
            try {
                results[t] = exerciseOwnStorage(t);
            }
            catch (const std::exception& error) {
                results[t] = std::string("exception: ") + error.what();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    for (const std::string& result : results) {
        EXPECT_EQ(result, "ok");
    }
}
