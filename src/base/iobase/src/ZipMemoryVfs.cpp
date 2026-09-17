#include <vine/io/ZipMemoryVfs.hpp>

#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include <vine/io/ZipArchive.hpp>

#include "VfsInternal.hpp"

V_IO_NS_BEGIN

ZipMemoryVfs::ZipMemoryVfs() = default;

ZipMemoryVfs::~ZipMemoryVfs() = default;

std::unique_ptr<ZipMemoryVfs> ZipMemoryVfs::openZip(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return nullptr;
    }
    const auto names = ZipArchive::entryNames(path);
    auto       vfs   = std::make_unique<ZipMemoryVfs>();
    for (const String& name : names) {
        if (!name.empty() && name.as_std_u8str().back() == u8'/') {
            continue; // directory entry; directories are derived from paths
        }
        std::vector<unsigned char> bytes;
        if (!ZipArchive::readEntry(path, name, bytes)) {
            return nullptr;
        }
        vfs->entries_.emplace(name, Entry{ std::move(bytes), {}, false });
    }
    return vfs;
}

std::unique_ptr<ZipMemoryVfs> ZipMemoryVfs::openZip(const void* data, std::size_t size)
{
    if (data == nullptr) {
        return nullptr;
    }
    const auto names = ZipArchive::entryNames(data, size);
    auto       vfs   = std::make_unique<ZipMemoryVfs>();
    for (const String& name : names) {
        if (!name.empty() && name.as_std_u8str().back() == u8'/') {
            continue;
        }
        std::vector<unsigned char> bytes;
        if (!ZipArchive::readEntry(data, size, name, bytes)) {
            return nullptr;
        }
        vfs->entries_.emplace(name, Entry{ std::move(bytes), {}, false });
    }
    return vfs;
}

bool ZipMemoryVfs::exists(const String& path) const
{
    return isFile(path) || isDirectory(path);
}

bool ZipMemoryVfs::isFile(const String& path) const
{
    const String norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return false;
    }
    return entries_.find(norm) != entries_.end();
}

bool ZipMemoryVfs::isDirectory(const String& path) const
{
    const String norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return true; // the root
    }
    const std::u8string prefix = norm.as_std_u8str() + u8"/";
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        if (key.as_std_u8str().compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<String> ZipMemoryVfs::list(const String& dir) const
{
    const String          norm   = detail::normalizeVfsPath(dir);
    const std::u8string   prefix = norm.empty() ? std::u8string() : norm.as_std_u8str() + u8"/";
    std::vector<String>   result;
    std::set<std::u8string> seen;
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        const std::u8string& k = key.as_std_u8str();
        if (k.empty()) {
            continue;
        }
        std::size_t begin = 0;
        if (!prefix.empty()) {
            if (k.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            begin = prefix.size();
        }
        const std::size_t slash = k.find(u8'/', begin);
        const std::u8string segment = k.substr(begin, slash == std::u8string::npos ? std::u8string::npos : slash - begin);
        if (segment.empty() || !seen.insert(segment).second) {
            continue;
        }
        result.emplace_back(segment);
    }
    return result;
}

bool ZipMemoryVfs::remove(const String& path)
{
    const String          norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return false; // never remove the root
    }
    const std::u8string& n = norm.as_std_u8str();
    bool                 removed = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const std::u8string& k = it->first.as_std_u8str();
        if (k == n || k.compare(0, n.size() + 1, n + u8"/") == 0) {
            it = entries_.erase(it);
            removed = true;
        }
        else {
            ++it;
        }
    }
    return removed;
}

bool ZipMemoryVfs::writeFile(const String& path, const void* data, std::size_t size)
{
    const String norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return false;
    }
    Entry entry;
    if (data != nullptr && size > 0) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        entry.data.assign(bytes, bytes + size);
    }
    entries_[norm] = std::move(entry);
    return true;
}

bool ZipMemoryVfs::writeFile(const String& path, const std::vector<unsigned char>& data)
{
    return writeFile(path, data.data(), data.size());
}

bool ZipMemoryVfs::writeFile(const String& path, const String& text)
{
    return writeFile(path, text.data(), text.size());
}

bool ZipMemoryVfs::mountFile(const String& vfs_path, const std::filesystem::path& real_path)
{
    const String norm = detail::normalizeVfsPath(vfs_path);
    if (norm.empty() || real_path.empty()) {
        return false;
    }
    Entry entry;
    entry.src       = real_path;
    entry.from_file = true;
    entries_[norm]  = std::move(entry);
    return true;
}

bool ZipMemoryVfs::readFile(const String& path, std::vector<unsigned char>& out) const
{
    const String norm = detail::normalizeVfsPath(path);
    if (norm.empty()) {
        return false;
    }
    const auto it = entries_.find(norm);
    if (it == entries_.end()) {
        return false;
    }
    if (it->second.from_file) {
        std::ifstream in(it->second.src, std::ios::binary);
        if (!in) {
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }
    out = it->second.data;
    return true;
}

bool ZipMemoryVfs::fillArchive(ZipArchive& archive) const
{
    for (const auto& [key, entry] : entries_) {
        if (entry.from_file) {
            if (!archive.addFile(key, entry.src)) {
                return false;
            }
        }
        else if (!archive.addFile(key, entry.data)) {
            return false;
        }
    }
    return true;
}

bool ZipMemoryVfs::save(const std::filesystem::path& path)
{
    ZipArchive archive;
    return fillArchive(archive) && archive.save(path);
}

bool ZipMemoryVfs::save(std::vector<unsigned char>& out)
{
    ZipArchive archive;
    return fillArchive(archive) && archive.save(out);
}

bool ZipMemoryVfs::save(std::ostream& out)
{
    ZipArchive archive;
    return fillArchive(archive) && archive.save(out);
}

V_IO_NS_END
