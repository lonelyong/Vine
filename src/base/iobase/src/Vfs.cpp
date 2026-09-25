#include <vine/io/Vfs.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "VfsInternal.hpp"

VN_IO_NS_BEGIN

Vfs::~Vfs() = default;

std::filesystem::path VfsEntryInfo::name() const
{
    return detail::nameOf(path);
}

VfsEntryKind Vfs::kindOf(const std::filesystem::path& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    if (!info.ok()) {
        return VfsEntryKind::Missing;
    }
    return info.value().is_directory ? VfsEntryKind::Directory : VfsEntryKind::File;
}

bool Vfs::exists(const std::filesystem::path& path) const
{
    return stat(path).ok();
}

bool Vfs::isFile(const std::filesystem::path& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    return info.ok() && !info.value().is_directory;
}

bool Vfs::isDirectory(const std::filesystem::path& path) const
{
    const Result<VfsEntryInfo> info = stat(path);
    return info.ok() && info.value().is_directory;
}

IoError Vfs::read(const std::filesystem::path& path, DataSink& sink) const
{
    const Result<std::unique_ptr<VfsReadStream>> opened = openRead(path);
    if (!opened.ok()) {
        return opened.error();
    }

    // The chunk is both the unit the sink sees and the whole scratch this push
    // costs: a file is never assembled, however large it is. The end of the
    // entry and a failure read() cannot express are told apart by error().
    constexpr std::size_t  kPushChunk = 64U * 1024U;
    std::vector<std::byte> chunk(kPushChunk);
    while (true) {
        const std::size_t got = opened.value()->read(chunk);
        if (got == 0) {
            return opened.value()->error();
        }
        const IoError refused = sink.write(std::span<const std::byte>(chunk.data(), got));
        if (refused != IoError::Ok) {
            return refused;
        }
    }
}

IoError Vfs::addFile(const std::filesystem::path& path, std::span<const Fragment> fragments)
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

IoError Vfs::addFile(const std::filesystem::path& path, std::shared_ptr<DataSource> source)
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

IoError Vfs::addDirectory(const std::filesystem::path& prefix, const std::filesystem::path& dir)
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

        // Joined as virtual paths, so the spelling stays '/'-separated: the host would
        // have spelled the relative part with '\' on Windows, which is not a virtual path.
        const std::filesystem::path path = detail::joinVfs(prefix, relative);

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

VN_IO_NS_END
