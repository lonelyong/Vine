#include <vine/io/DirectoryVfs.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include "VfsInternal.hpp"

V_IO_NS_BEGIN

DirectoryVfs::DirectoryVfs(const std::filesystem::path& root)
  : root_(root)
{
}

DirectoryVfs::~DirectoryVfs() = default;

std::unique_ptr<DirectoryVfs> DirectoryVfs::openDirectory(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return nullptr;
    }
    return std::make_unique<DirectoryVfs>(dir);
}

std::filesystem::path DirectoryVfs::toReal(const String& vfs_path) const
{
    const String norm = detail::normalizeVfsPath(vfs_path);
    if (norm.empty()) {
        return root_;
    }
    // Reject path traversal.
    if (norm.as_std_u8str().find(u8"..") != std::u8string::npos) {
        return {};
    }
    return root_ / std::filesystem::path(norm.as_std_u8str());
}

bool DirectoryVfs::exists(const String& path) const
{
    std::error_code ec;
    const auto      real = toReal(path);
    return !real.empty() && std::filesystem::exists(real, ec) && !ec;
}

bool DirectoryVfs::isFile(const String& path) const
{
    std::error_code ec;
    const auto      real = toReal(path);
    return !real.empty() && std::filesystem::is_regular_file(real, ec) && !ec;
}

bool DirectoryVfs::isDirectory(const String& path) const
{
    std::error_code ec;
    const auto      real = toReal(path);
    return !real.empty() && std::filesystem::is_directory(real, ec) && !ec;
}

std::vector<String> DirectoryVfs::list(const String& dir) const
{
    std::vector<String> result;
    const auto          real = toReal(dir);
    std::error_code     ec;
    if (real.empty() || !std::filesystem::is_directory(real, ec) || ec) {
        return result;
    }
    for (std::filesystem::directory_iterator it(real, ec), end; !ec && it != end; it.increment(ec)) {
        result.push_back(vine::String(it->path().filename().u8string()));
    }
    return result;
}

bool DirectoryVfs::remove(const String& path)
{
    const String norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return false; // never remove the root
    }
    std::error_code ec;
    const auto      real = toReal(norm);
    if (real.empty()) {
        return false;
    }
    return std::filesystem::remove_all(real, ec) > 0 && !ec;
}

bool DirectoryVfs::writeFile(const String& path, const void* data, std::size_t size)
{
    const auto real = toReal(path);
    if (real.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(real.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(real, std::ios::binary);
    if (!out) {
        return false;
    }
    if (data != nullptr && size > 0) {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return out.good();
}

bool DirectoryVfs::writeFile(const String& path, const std::vector<unsigned char>& data)
{
    return writeFile(path, data.data(), data.size());
}

bool DirectoryVfs::writeFile(const String& path, const String& text)
{
    return writeFile(path, text.data(), text.size());
}

bool DirectoryVfs::mountFile(const String& vfs_path, const std::filesystem::path& real_path)
{
    std::ifstream in(real_path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    return writeFile(vfs_path, bytes.data(), bytes.size());
}

bool DirectoryVfs::readFile(const String& path, std::vector<unsigned char>& out) const
{
    const auto real = toReal(path);
    if (real.empty()) {
        return false;
    }
    std::ifstream in(real, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool DirectoryVfs::save(const std::filesystem::path& path)
{
    // Writes are immediate; only a directory target is meaningful (a no-op).
    std::error_code ec;
    return !path.empty() && std::filesystem::is_directory(path, ec) && !ec;
}

bool DirectoryVfs::save(std::vector<unsigned char>& out)
{
    (void)out;
    return false; // not supported by a directory backend
}

bool DirectoryVfs::save(std::ostream& out)
{
    (void)out;
    return false; // not supported by a directory backend
}

V_IO_NS_END
