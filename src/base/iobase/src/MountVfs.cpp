#include <vine/io/MountVfs.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "VfsInternal.hpp"

VN_IO_NS_BEGIN

namespace
{

/**
 * @brief Expresses a normalized path relative to a mount prefix.
 *
 * @param prefix The mount prefix; empty is the virtual root.
 * @param path The normalized path, at or below prefix.
 * @return The backend-local path; empty for the mount's own root.
 */
std::filesystem::path relativeTo(const std::filesystem::path& prefix, const std::filesystem::path& path)
{
    if (prefix.empty()) {
        return path;
    }
    if (path == prefix) {
        return {};
    }
    return path.lexically_relative(prefix);
}

/**
 * @brief Spells a backend-local path as a full virtual path.
 *
 * @param prefix The mount prefix; empty is the virtual root.
 * @param path The backend-local path; empty is the backend's root.
 * @return The normalized virtual path.
 */
std::filesystem::path prefixed(const std::filesystem::path& prefix, const std::filesystem::path& path)
{
    if (path.empty()) {
        return prefix;
    }
    return detail::joinVfs(prefix, path);
}

/**
 * @brief Takes the first segment of a normalized relative path.
 *
 * @param path The path; must not be empty.
 * @return The leading name.
 */
std::filesystem::path firstSegment(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    const std::size_t   cut  = text.find(u8'/');
    return std::filesystem::path(cut == std::u8string::npos ? text : text.substr(0, cut));
}

} // namespace

MountVfs::MountVfs() = default;

MountVfs::~MountVfs() = default;

IoError MountVfs::mount(const std::filesystem::path& prefix, std::shared_ptr<Vfs> backend, int priority, bool read_only)
{
    if (backend == nullptr) {
        return IoError::InvalidData;
    }

    std::filesystem::path normalized;
    const IoError        err = detail::normalizeVfsPath(prefix, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    mounts_.push_back(Mount{ std::move(normalized), std::move(backend), priority, read_only });
    return IoError::Ok;
}

bool MountVfs::isReadOnly() const noexcept
{
    for (const Mount& mount : mounts_) {
        if (acceptsWrites(mount)) {
            return false;
        }
    }
    return true;
}

Result<VfsEntryInfo> MountVfs::stat(const std::filesystem::path& path) const
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    for (const Mount* mount : route(normalized)) {
        Result<VfsEntryInfo> info = mount->backend->stat(relativeTo(mount->prefix, normalized));
        if (info.ok()) {
            VfsEntryInfo entry = info.take();
            entry.path         = prefixed(mount->prefix, entry.path);
            return entry;
        }
    }

    if (isImpliedDirectory(normalized)) {
        VfsEntryInfo entry;
        entry.path         = normalized;
        entry.is_directory = true;
        return entry;
    }
    return IoError::NotFound;
}

Result<std::vector<VfsEntryInfo>> MountVfs::list(const std::filesystem::path& dir) const
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(dir, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group = route(normalized);

    bool is_directory = false;
    for (const Mount* mount : group) {
        Result<VfsEntryInfo> info = mount->backend->stat(relativeTo(mount->prefix, normalized));
        if (!info.ok()) {
            continue;
        }
        if (!info.value().is_directory) {
            return IoError::NotADirectory; // the first hit is what a reader sees
        }
        is_directory = true;
        break;
    }
    if (!is_directory && !isImpliedDirectory(normalized)) {
        return IoError::NotFound;
    }

    // A deeper mount always contributes its directory, even when no backend holds it.
    std::map<std::u8string, VfsEntryInfo> merged;
    for (const Mount& mount : mounts_) {
        if (!detail::isPathBelow(normalized, mount.prefix)) {
            continue;
        }
        VfsEntryInfo entry;
        entry.path         = prefixed(normalized, firstSegment(relativeTo(normalized, mount.prefix)));
        entry.is_directory = true;
        merged.try_emplace(entry.path.generic_u8string(), entry);
    }

    // Then every mount of the group, highest priority first: the first claim on a name wins.
    IoError first_error = IoError::Ok;
    for (const Mount* mount : group) {
        Result<std::vector<VfsEntryInfo>> children = mount->backend->list(relativeTo(mount->prefix, normalized));
        if (!children.ok()) {
            if (first_error == IoError::Ok) {
                first_error = children.error();
            }
            continue;
        }
        for (VfsEntryInfo& child : children.value()) {
            child.path = prefixed(mount->prefix, child.path);
            merged.try_emplace(child.path.generic_u8string(), child);
        }
    }

    if (merged.empty() && first_error != IoError::Ok) {
        return first_error; // nothing was listed and at least one mount failed
    }

    std::vector<VfsEntryInfo> entries;
    entries.reserve(merged.size());
    for (std::pair<const std::u8string, VfsEntryInfo>& entry : merged) {
        entries.push_back(std::move(entry.second));
    }
    return entries;
}

Result<std::vector<unsigned char>> MountVfs::read(const std::filesystem::path& path) const
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group = route(normalized);
    const Mount*                    owner = firstHit(group, normalized);
    if (owner == nullptr) {
        return IoError::NotFound;
    }
    return owner->backend->read(relativeTo(owner->prefix, normalized));
}

Result<std::unique_ptr<VfsReadStream>> MountVfs::openRead(const std::filesystem::path& path) const
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group = route(normalized);
    const Mount*                    owner = firstHit(group, normalized);
    if (owner == nullptr) {
        return IoError::NotFound;
    }
    return owner->backend->openRead(relativeTo(owner->prefix, normalized));
}

IoError MountVfs::addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->addFile(relativeTo(target->prefix, normalized), bytes);
}

IoError MountVfs::addFile(const std::filesystem::path& path, const std::filesystem::path& real_path)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->addFile(relativeTo(target->prefix, normalized), real_path);
}

IoError MountVfs::addFile(const std::filesystem::path& path, std::span<const Fragment> fragments)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->addFile(relativeTo(target->prefix, normalized), fragments);
}

IoError MountVfs::addFile(const std::filesystem::path& path, std::shared_ptr<DataSource> source)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->addFile(relativeTo(target->prefix, normalized), std::move(source));
}

IoError MountVfs::createDirectory(const std::filesystem::path& path)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->createDirectory(relativeTo(target->prefix, normalized));
}

IoError MountVfs::createDirectories(const std::filesystem::path& path)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group  = route(normalized);
    const Mount*                    target = writeTarget(group, normalized);
    if (target == nullptr) {
        return IoError::ReadOnly;
    }
    return target->backend->createDirectories(relativeTo(target->prefix, normalized));
}

IoError MountVfs::rename(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::filesystem::path source;
    std::filesystem::path target;
    IoError               err = detail::normalizeVfsPath(from, source);
    if (err != IoError::Ok) {
        return err;
    }
    err = detail::normalizeVfsPath(to, target);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> source_group = route(source);
    const Mount*                    owner        = firstHit(source_group, source);
    if (owner == nullptr) {
        return IoError::NotFound;
    }
    if (!acceptsWrites(*owner)) {
        return IoError::ReadOnly;
    }

    const std::vector<const Mount*> target_group = route(target);
    const Mount*                    occupied     = firstHit(target_group, target);
    if (occupied != nullptr) {
        if (occupied->backend != owner->backend) {
            return IoError::AlreadyExists; // never overwrite, or merge, across backends
        }
        if (!acceptsWrites(*occupied)) {
            return IoError::ReadOnly;
        }
        return owner->backend->rename(relativeTo(owner->prefix, source), relativeTo(occupied->prefix, target));
    }

    const Mount* destination = firstWritable(target_group);
    if (destination == nullptr) {
        return IoError::ReadOnly;
    }
    if (destination->backend == owner->backend) {
        return owner->backend->rename(relativeTo(owner->prefix, source), relativeTo(destination->prefix, target));
    }

    // Across backends: copy the content, then drop the source; a failure keeps
    // the source in place. The copy holds one entry in memory, because a
    // deferred backend would otherwise pull from a source that is already
    // gone by the time it does.
    const Result<VfsEntryInfo> info = owner->backend->stat(relativeTo(owner->prefix, source));
    if (!info.ok()) {
        return info.error();
    }
    if (info.value().is_directory) {
        return IoError::Unsupported; // a directory is not copied piecewise
    }

    const Result<std::vector<unsigned char>> content = owner->backend->read(relativeTo(owner->prefix, source));
    if (!content.ok()) {
        return content.error();
    }
    err = destination->backend->addFile(relativeTo(destination->prefix, target),
                                        std::span<const unsigned char>(content.value()));
    if (err != IoError::Ok) {
        return err;
    }
    return owner->backend->remove(relativeTo(owner->prefix, source));
}

IoError MountVfs::remove(const std::filesystem::path& path)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group = route(normalized);
    const Mount*                    owner = firstHit(group, normalized);
    if (owner == nullptr) {
        return IoError::NotFound;
    }
    if (!acceptsWrites(*owner)) {
        return IoError::ReadOnly;
    }
    return owner->backend->remove(relativeTo(owner->prefix, normalized));
}

IoError MountVfs::removeAll(const std::filesystem::path& path)
{
    std::filesystem::path normalized;
    const IoError         err = detail::normalizeVfsPath(path, normalized);
    if (err != IoError::Ok) {
        return err;
    }

    const std::vector<const Mount*> group = route(normalized);
    const Mount*                    owner = firstHit(group, normalized);
    if (owner == nullptr) {
        return IoError::NotFound;
    }
    if (!acceptsWrites(*owner)) {
        return IoError::ReadOnly;
    }
    return owner->backend->removeAll(relativeTo(owner->prefix, normalized));
}

IoError MountVfs::commit()
{
    IoError                 first_error = IoError::Ok;
    std::vector<const Vfs*> committed;

    for (const Mount& mount : mounts_) {
        if (!acceptsWrites(mount)) {
            continue;
        }
        if (std::find(committed.begin(), committed.end(), mount.backend.get()) != committed.end()) {
            continue; // one backend commits once
        }
        committed.push_back(mount.backend.get());

        const IoError err = mount.backend->commit();
        if (err != IoError::Ok && first_error == IoError::Ok) {
            first_error = err;
        }
    }
    return first_error;
}

IoError MountVfs::saveAs(const std::filesystem::path& path) const
{
    (void)path;
    return IoError::Unsupported; // a mounted tree is not one package
}

IoError MountVfs::saveAs(std::ostream& out) const
{
    (void)out;
    return IoError::Unsupported; // a mounted tree is not one package
}

Result<std::vector<unsigned char>> MountVfs::toBytes() const
{
    return IoError::Unsupported; // a mounted tree is not one package
}

std::vector<const MountVfs::Mount*> MountVfs::route(const std::filesystem::path& normalized) const
{
    const std::filesystem::path* deepest = nullptr;
    for (const Mount& mount : mounts_) {
        if (mount.prefix != normalized && !detail::isPathBelow(mount.prefix, normalized)) {
            continue;
        }
        if (deepest == nullptr || mount.prefix.native().size() > deepest->native().size()) {
            deepest = &mount.prefix;
        }
    }

    std::vector<const Mount*> group;
    if (deepest == nullptr) {
        return group; // nothing is mounted over this path
    }
    for (const Mount& mount : mounts_) {
        if (mount.prefix == *deepest) {
            group.push_back(&mount);
        }
    }
    std::stable_sort(group.begin(), group.end(),
                     [](const Mount* left, const Mount* right) { return left->priority > right->priority; });
    return group;
}

const MountVfs::Mount* MountVfs::firstHit(const std::vector<const Mount*>& group,
                                           const std::filesystem::path&    normalized) const
{
    for (const Mount* mount : group) {
        if (mount->backend->stat(relativeTo(mount->prefix, normalized)).ok()) {
            return mount;
        }
    }
    return nullptr;
}

const MountVfs::Mount* MountVfs::writeTarget(const std::vector<const Mount*>& group,
                                              const std::filesystem::path&    normalized) const
{
    for (const Mount* mount : group) {
        if (mount->backend->stat(relativeTo(mount->prefix, normalized)).ok()) {
            return acceptsWrites(*mount) ? mount : nullptr; // the owner decides: no fall-through
        }
    }
    return firstWritable(group);
}

const MountVfs::Mount* MountVfs::firstWritable(const std::vector<const Mount*>& group) const
{
    for (const Mount* mount : group) {
        if (acceptsWrites(*mount)) {
            return mount;
        }
    }
    return nullptr;
}

bool MountVfs::isImpliedDirectory(const std::filesystem::path& normalized) const
{
    for (const Mount& mount : mounts_) {
        if (mount.prefix == normalized || detail::isPathBelow(normalized, mount.prefix)) {
            return true;
        }
    }
    return false;
}

bool MountVfs::acceptsWrites(const Mount& mount)
{
    return !mount.read_only && !mount.backend->isReadOnly();
}

VN_IO_NS_END
