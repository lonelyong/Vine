#include <vine/io/Vfs.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "VfsInternal.hpp"

V_IO_NS_BEGIN

Vfs::~Vfs() = default;

String VfsEntryInfo::name() const
{
    return detail::nameOf(path);
}

VfsEntryKind Vfs::kindOf(const String& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    if (!info.ok()) {
        return VfsEntryKind::Missing;
    }
    return info.value().is_directory ? VfsEntryKind::Directory : VfsEntryKind::File;
}

bool Vfs::exists(const String& path) const
{
    return stat(path).ok();
}

bool Vfs::isFile(const String& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    return info.ok() && !info.value().is_directory;
}

bool Vfs::isDirectory(const String& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    return info.ok() && info.value().is_directory;
}

IoError Vfs::addFile(const String& path, std::span<const Fragment> fragments)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    if (detail::hasDatalessFragment(fragments)) {
        return IoError::InvalidData;
    }

    std::uint64_t total = 0;
    for (const Fragment& piece : fragments) {
        total += piece.size;
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(static_cast<std::size_t>(total));
    for (const Fragment& piece : fragments) {
        if (piece.size != 0) {
            bytes.insert(bytes.end(), piece.data, piece.data + piece.size);
        }
    }
    return addFile(path, std::span<const unsigned char>(bytes));
}

IoError Vfs::addFile(const String& path, std::shared_ptr<DataSource> source)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    if (source == nullptr) {
        return IoError::InvalidData;
    }

    // The default materializes the content right away; a backend that can defer
    // the pull to saveAs() overrides this and keeps the source instead.
    std::vector<unsigned char> bytes(static_cast<std::size_t>(source->size()));
    source->rewind();
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t got = source->read(
            std::span<std::byte>(reinterpret_cast<std::byte*>(bytes.data()) + done, bytes.size() - done));
        if (got == 0) {
            return IoError::InvalidData; // the source stopped short of its size() promise
        }
        done += got;
    }
    return addFile(path, std::span<const unsigned char>(bytes));
}

IoError Vfs::addDirectory(const String& prefix, const std::filesystem::path& dir)
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

        const IoError imported = addFile(path, it->path());
        if (imported != IoError::Ok) {
            return imported;
        }
    }
    return ec ? IoError::IoFailure : IoError::Ok;
}

V_IO_NS_END
