#include <vine/io/DirectoryVfs.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <utility>

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

IoError DirectoryVfs::resolve(const std::filesystem::path& vfs_path, std::filesystem::path& normalized,
                              std::filesystem::path& out) const
{
    const IoError error = detail::normalizeVfsPath(vfs_path, normalized);
    if (error != IoError::Ok) {
        return error;
    }
    if (normalized.empty()) {
        out = root_;
        return IoError::Ok;
    }
    // A normalized virtual path is relative and carries no root name, so joining it can
    // only extend root_ - it has no spelling left that would replace it.
    if (normalized.is_absolute() || normalized.has_root_name()) {
        return IoError::InvalidPath; // defense in depth
    }
    out = root_ / normalized;
    return IoError::Ok;
}

bool DirectoryVfs::isReadOnly() const noexcept
{
    return false;
}

Result<VfsEntryInfo> DirectoryVfs::stat(const std::filesystem::path& path) const
{
    std::filesystem::path norm;
    std::filesystem::path real;
    const IoError         error = resolve(path, norm, real);
    if (error != IoError::Ok) {
        return error;
    }

    // The type decides, not the error code: MSVC also sets it for a missing
    // path, where the standard only asks for file_type::not_found.
    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (type == std::filesystem::file_type::directory) {
        return VfsEntryInfo{ std::move(norm), true, 0 };
    }
    if (type != std::filesystem::file_type::regular) {
        return IoError::NotFound; // a device, socket, pipe or similar
    }

    const std::uintmax_t bytes = std::filesystem::file_size(real, ec);
    if (ec) {
        return IoError::IoFailure;
    }
    return VfsEntryInfo{ std::move(norm), false, static_cast<std::uint64_t>(bytes) };
}

Result<std::vector<VfsEntryInfo>> DirectoryVfs::list(const std::filesystem::path& dir) const
{
    std::filesystem::path base;
    std::filesystem::path real;
    const IoError         error = resolve(dir, base, real);
    if (error != IoError::Ok) {
        return error;
    }

    // The type decides, not the error code: MSVC also sets it for a missing
    // path, where the standard only asks for file_type::not_found.
    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (type != std::filesystem::file_type::directory) {
        return IoError::NotADirectory;
    }

    std::vector<VfsEntryInfo> children;
    for (std::filesystem::directory_iterator it(real, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;

        std::error_code                  entry_ec;
        const std::filesystem::file_type entry_type = entry.status(entry_ec).type();
        if (entry_ec) {
            return IoError::IoFailure;
        }
        const bool is_dir = entry_type == std::filesystem::file_type::directory;

        const std::filesystem::path name = entry.path().filename();
        VfsEntryInfo info;
        info.path         = detail::joinVfs(base, name);
        info.is_directory = is_dir;
        if (!is_dir) {
            std::error_code      size_ec;
            const std::uintmax_t bytes = entry.file_size(size_ec);
            if (size_ec) {
                return IoError::IoFailure;
            }
            info.size = static_cast<std::uint64_t>(bytes);
        }
        children.push_back(std::move(info));
    }
    if (ec) {
        return IoError::IoFailure;
    }
    return children;
}

IoError DirectoryVfs::remove(const std::filesystem::path& path)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path base;
    std::filesystem::path real;
    const IoError         error = resolve(path, base, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (base.empty()) {
        return IoError::InvalidPath; // the root cannot be removed
    }

    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (type == std::filesystem::file_type::directory && !std::filesystem::is_empty(real, ec)) {
        return ec ? IoError::IoFailure : IoError::NotEmpty;
    }

    std::filesystem::remove(real, ec);
    if (ec) {
        return ec == std::errc::permission_denied ? IoError::PermissionDenied : IoError::IoFailure;
    }
    return IoError::Ok;
}

IoError DirectoryVfs::removeAll(const std::filesystem::path& path)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path base;
    std::filesystem::path real;
    const IoError         error = resolve(path, base, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (base.empty()) {
        return IoError::InvalidPath; // the root cannot be removed
    }

    std::error_code ec;
    if (std::filesystem::status(real, ec).type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    std::filesystem::remove_all(real, ec);
    if (ec) {
        return ec == std::errc::permission_denied ? IoError::PermissionDenied : IoError::IoFailure;
    }
    return IoError::Ok;
}

IoError DirectoryVfs::createDirectory(const std::filesystem::path& path)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path base;
    std::filesystem::path real;
    const IoError         error = resolve(path, base, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (base.empty()) {
        return IoError::AlreadyExists; // the root always exists
    }

    // The type decides, not the error code: MSVC also sets it for a missing
    // path, where the standard only asks for file_type::not_found.
    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type != std::filesystem::file_type::not_found) {
        return IoError::AlreadyExists; // already a directory, or a file owns the name
    }

    const std::filesystem::file_type parent_type = std::filesystem::status(real.parent_path(), ec).type();
    if (parent_type != std::filesystem::file_type::directory) {
        return parent_type == std::filesystem::file_type::not_found ? IoError::NotFound : IoError::NotADirectory;
    }

    std::filesystem::create_directory(real, ec);
    if (ec) {
        return ec == std::errc::permission_denied ? IoError::PermissionDenied : IoError::IoFailure;
    }
    return IoError::Ok;
}

IoError DirectoryVfs::createDirectories(const std::filesystem::path& path)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path base;
    std::filesystem::path real;
    const IoError         error = resolve(path, base, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (base.empty()) {
        return IoError::Ok; // the root always exists
    }

    std::error_code ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type == std::filesystem::file_type::directory) {
        return IoError::Ok; // already there
    }
    if (type != std::filesystem::file_type::not_found) {
        return IoError::AlreadyExists; // a file owns the name
    }

    std::filesystem::create_directories(real, ec);
    if (ec) {
        if (ec == std::errc::permission_denied) {
            return IoError::PermissionDenied;
        }
        if (ec == std::errc::file_exists || ec == std::errc::not_a_directory) {
            return IoError::NotADirectory; // a file blocks the way
        }
        return IoError::IoFailure;
    }
    return IoError::Ok;
}

IoError DirectoryVfs::addFile(const std::filesystem::path& path, const std::filesystem::path& real_path)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path norm;
    std::filesystem::path real;
    const IoError         error = resolve(path, norm, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return IoError::IsADirectory; // the root is a directory
    }

    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real_path, ec).type();
    if (type == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (type != std::filesystem::file_type::regular) {
        return IoError::NotFound; // only regular files can be brought in
    }
    if (std::filesystem::status(real, ec).type() == std::filesystem::file_type::directory) {
        return IoError::IsADirectory; // a directory is never replaced by a file
    }

    std::filesystem::create_directories(real.parent_path(), ec); // parents are implied
    if (ec) {
        return IoError::IoFailure;
    }

    // A directory backend copies the file right away, streaming it.
    std::filesystem::copy_file(real_path, real, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return ec == std::errc::permission_denied ? IoError::PermissionDenied : IoError::IoFailure;
    }
    return IoError::Ok;
}

IoError DirectoryVfs::rename(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path source;
    std::filesystem::path source_real;
    IoError               error = resolve(from, source, source_real);
    if (error != IoError::Ok) {
        return error;
    }
    std::filesystem::path target;
    std::filesystem::path target_real;
    error = resolve(to, target, target_real);
    if (error != IoError::Ok) {
        return error;
    }
    if (source.empty() || target.empty()) {
        return IoError::InvalidPath; // the root is neither movable nor a target
    }
    if (source == target) {
        return IoError::Ok; // nothing to do
    }
    if (detail::isPathBelow(source, target)) {
        return IoError::InvalidPath; // a tree cannot move into itself
    }

    std::error_code ec;
    if (std::filesystem::status(source_real, ec).type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (std::filesystem::status(target_real, ec).type() != std::filesystem::file_type::not_found) {
        return IoError::AlreadyExists; // never overwrite, on any platform
    }
    const std::filesystem::file_type parent_type = std::filesystem::status(target_real.parent_path(), ec).type();
    if (parent_type != std::filesystem::file_type::directory) {
        return parent_type == std::filesystem::file_type::not_found ? IoError::NotFound : IoError::NotADirectory;
    }

    std::filesystem::rename(source_real, target_real, ec);
    if (ec) {
        return ec == std::errc::permission_denied ? IoError::PermissionDenied : IoError::IoFailure;
    }
    return IoError::Ok;
}

Result<std::vector<unsigned char>> DirectoryVfs::read(const std::filesystem::path& path) const
{
    std::filesystem::path norm;
    std::filesystem::path real;
    const IoError         error = resolve(path, norm, real);
    if (error != IoError::Ok) {
        return error;
    }

    // The type decides, not the error code: MSVC also sets it for a missing
    // path, where the standard only asks for file_type::not_found.
    std::error_code                  ec;
    const std::filesystem::file_type type = std::filesystem::status(real, ec).type();
    if (type == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }
    if (type == std::filesystem::file_type::directory) {
        return IoError::IsADirectory;
    }
    if (type != std::filesystem::file_type::regular) {
        return IoError::NotFound;
    }

    std::ifstream in(real, std::ios::binary);
    if (!in) {
        return IoError::IoFailure;
    }
    std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    if (in.bad()) {
        return IoError::IoFailure;
    }
    return bytes;
}

IoError DirectoryVfs::addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path norm;
    std::filesystem::path real;
    const IoError         error = resolve(path, norm, real);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return IoError::IsADirectory; // the root is a directory
    }

    std::error_code ec;
    if (std::filesystem::status(real, ec).type() == std::filesystem::file_type::directory) {
        return IoError::IsADirectory; // a directory is never replaced by a file
    }

    std::filesystem::create_directories(real.parent_path(), ec); // parents are implied
    if (ec) {
        if (ec == std::errc::not_a_directory || ec == std::errc::file_exists) {
            return IoError::NotADirectory;
        }
        return IoError::IoFailure;
    }

    std::ofstream out(real, std::ios::binary | std::ios::trunc);
    if (!out) {
        return IoError::IoFailure;
    }
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return out.good() ? IoError::Ok : IoError::IoFailure;
}

IoError DirectoryVfs::commit()
{
    return IoError::Ok; // writes reach the directory as they happen
}

IoError DirectoryVfs::saveAs(const std::filesystem::path& path) const
{
    (void)path;
    return IoError::Unsupported; // a directory backend cannot produce an archive
}

IoError DirectoryVfs::saveAs(std::ostream& out) const
{
    (void)out;
    return IoError::Unsupported; // a directory backend cannot produce an archive
}

Result<std::vector<unsigned char>> DirectoryVfs::toBytes() const
{
    return IoError::Unsupported; // a directory backend cannot produce an archive
}

V_IO_NS_END
