#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <vine/io/Vfs.hpp>
#include <vine/String.hpp>

/**
 * @brief Helpers shared by the IOBase VFS test suites.
 */
namespace vfstest
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
                ("vine_vfs_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/**
 * @brief The bytes of a text, for writing binary payloads.
 */
inline std::vector<unsigned char> bytesOf(const std::string& text)
{
    return { text.begin(), text.end() };
}

/**
 * @brief Finds the child with a given full path, or null.
 */
inline const vine::io::VfsEntryInfo* findInfo(const std::vector<vine::io::VfsEntryInfo>& infos, const std::filesystem::path& path)
{
    for (const vine::io::VfsEntryInfo& info : infos) {
        if (info.path == path) {
            return &info;
        }
    }
    return nullptr;
}

/**
 * @brief The own names of some children, sorted, so two backends can be compared.
 */
inline std::vector<std::filesystem::path> sortedNames(const std::vector<vine::io::VfsEntryInfo>& children)
{
    std::vector<std::filesystem::path> names;
    names.reserve(children.size());
    for (const vine::io::VfsEntryInfo& child : children) {
        names.push_back(child.name());
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace vfstest
