#include <vine/io/Vfs.hpp>

#include <cstddef>
#include <span>
#include <vector>

#include "VfsInternal.hpp"

V_IO_NS_BEGIN

Vfs::~Vfs() = default;

String FileInfo::name() const
{
    return detail::nameOf(path);
}

bool Vfs::exists(const String& path) const
{
    return stat(path).ok();
}

bool Vfs::isFile(const String& path) const
{
    const Result<FileInfo> info = stat(path);
    return info.ok() && !info.value().is_directory;
}

bool Vfs::isDirectory(const String& path) const
{
    const Result<FileInfo> info = stat(path);
    return info.ok() && info.value().is_directory;
}

IoError Vfs::importDirectory(const String& prefix, const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return IoError::NotFound;
    }

    if (!prefix.empty()) {
        const IoError made = createDirectories(prefix);
        if (made != IoError::Ok) {
            return made;
        }
    }

    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path relative = std::filesystem::relative(it->path(), dir, ec);
        if (ec) {
            return IoError::IoFailure;
        }

        const std::string suffix = relative.generic_string();
        const String      path   = prefix.empty()
                                       ? detail::fromUtf8(suffix.c_str(), suffix.size())
                                       : String(prefix.as_std_u8str() + u8"/" + detail::fromUtf8(suffix.c_str(), suffix.size()).as_std_u8str());

        if (it->is_directory(ec)) {
            if (ec) {
                return IoError::IoFailure;
            }
            const IoError made = createDirectory(path); // empty directories would be lost otherwise
            if (made != IoError::Ok && made != IoError::AlreadyExists) {
                return made;
            }
            continue;
        }
        if (!it->is_regular_file(ec) || ec) {
            return IoError::NotFound; // only regular files are brought in
        }

        const IoError imported = importFile(path, it->path());
        if (imported != IoError::Ok) {
            return imported;
        }
    }
    return ec ? IoError::IoFailure : IoError::Ok;
}

V_IO_NS_END
