#include <vine/io/DirectoryVfs.hpp>

#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <vine/io/Adapters.hpp>

#include "VfsInternal.hpp"

VN_IO_NS_BEGIN

namespace
{

/** @brief Suffix of the file a streaming write grows before it replaces the target. */
constexpr const char* kTemporarySuffix = ".vine-tmp";

/**
 * @brief Writes a file by streaming into a sibling temporary and publishing it when the content is complete.
 *
 * A source can stop halfway (a broken stream, a length it cannot deliver), and a half-written file would look like
 * content - so the bytes go into a temporary first and the target is replaced at the very end. The target is therefore
 * either what was there before or the whole new content, never a mixture; ZipArchive::commit publishes its own file the
 * same way.
 *
 * @param real The file to publish; its parent directory must exist.
 * @param produce Writes the content into the stream; it returns what went wrong, or IoError::Ok when it finished.
 * @return IoError::Ok on success, or what @p produce reported; nothing is published either way.
 */
IoError writeThroughTemporary(const std::filesystem::path& real, const std::function<IoError(std::ostream&)>& produce)
{
    std::filesystem::path staging = real;
    staging += kTemporarySuffix;

    std::error_code ec;
    std::filesystem::remove(staging, ec); // a leftover from an interrupted run is not content to append to
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            return IoError::IoFailure;
        }
        const IoError produced = produce(out);
        out.close();
        if (produced != IoError::Ok || !out) {
            std::filesystem::remove(staging, ec);
            return produced != IoError::Ok ? produced : IoError::IoFailure;
        }
    }

    std::filesystem::rename(staging, real, ec);
    if (ec) {
        std::filesystem::remove(staging, ec);
        return IoError::IoFailure;
    }
    return IoError::Ok;
}

} // namespace

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
        return VfsEntryInfo{ std::move(norm), VfsEntryKind::Directory, 0 };
    }
    if (type != std::filesystem::file_type::regular) {
        return IoError::NotFound; // a device, socket, pipe or similar
    }

    const std::uintmax_t bytes = std::filesystem::file_size(real, ec);
    if (ec) {
        return IoError::IoFailure;
    }
    return VfsEntryInfo{ std::move(norm), VfsEntryKind::File, static_cast<std::uint64_t>(bytes) };
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
        info.path = detail::joinVfs(base, name);
        info.kind = is_dir ? VfsEntryKind::Directory : VfsEntryKind::File;
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

namespace
{

/**
 * @brief A sequential reader over one open real file.
 *
 * The stream owns its handle, which is what lets it outlive the tree (see
 * VfsEntrySource); a file that disappears mid-read fails at the next read().
 */
class FileReadStream final : public VfsEntrySource
{
  public:
    /**
     * @brief Adopts an open binary file.
     *
     * @param file The open file.
     * @param size The number of bytes the reader reports and seeks within.
     */
    FileReadStream(std::ifstream file, std::uint64_t size)
      : file_(std::move(file))
      , size_(size)
    {
    }

    /**
     * @brief Reads the next chunk.
     *
     * @param out Buffer to fill.
     * @return The number of bytes read, or 0 at the end; a failure also
     *         reports 0 here, and error() tells the two apart.
     */
    std::size_t read(std::span<std::byte> out) override
    {
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        const std::streamsize got = file_.gcount();
        if (got == 0 && file_.bad()) {
            error_ = IoError::IoFailure;
        }
        return static_cast<std::size_t>(got);
    }

    /**
     * @brief Reports whether a failure happened behind a 0-byte read.
     *
     * @return IoError::Ok while the file reads cleanly, IoError::IoFailure
     *         when a read failed.
     */
    IoError error() const override { return error_; }

    /**
     * @brief Reports the file size the reader was opened with.
     *
     * @return The size in bytes.
     */
    std::uint64_t size() const noexcept override { return size_; }

    /**
     * @brief Reports that this reader can position itself directly.
     *
     * @return true always: a real file seeks without decoding anything.
     */
    bool seekable() const noexcept override { return true; }

    /**
     * @brief Positions the reader at an absolute offset.
     *
     * @param offset Target offset, counted from the start of the file.
     * @return IoError::Ok on success, IoError::OutOfRange when offset is past
     *         the end.
     */
    IoError seek(std::uint64_t offset) override
    {
        if (offset > size_) {
            return IoError::OutOfRange;
        }
        file_.clear(); // a previous failure must not block the seek itself
        file_.seekg(static_cast<std::streamoff>(offset));
        if (!file_) {
            error_ = IoError::IoFailure;
            return IoError::IoFailure;
        }
        error_ = IoError::Ok;
        return IoError::Ok;
    }

  private:
    std::ifstream file_;
    std::uint64_t size_;
    IoError       error_{ IoError::Ok };
};

} // namespace

Result<std::unique_ptr<VfsEntrySource>> DirectoryVfs::openRead(const std::filesystem::path& path) const
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
    const std::uintmax_t size = std::filesystem::file_size(real, ec);
    if (ec) {
        return IoError::IoFailure;
    }
    // The size is captured here and never re-read: like every reader, this one
    // describes the entry as it was opened, so a file that grows underneath it
    // does not silently extend the content.
    return std::unique_ptr<VfsEntrySource>(new FileReadStream(std::move(in), size));
}

IoError DirectoryVfs::addFile(const std::filesystem::path& path, std::span<const unsigned char> bytes)
{
    std::filesystem::path real;
    if (const IoError prepared = prepareNewFile(path, real); prepared != IoError::Ok) {
        return prepared;
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

IoError DirectoryVfs::addFile(const std::filesystem::path& path, std::shared_ptr<DataSource> source)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    if (source == nullptr) {
        return IoError::InvalidData;
    }
    if (const IoError settled = source->error(); settled != IoError::Ok) {
        return settled; // the source already knows it cannot deliver, so nothing is written
    }

    std::filesystem::path real;
    if (const IoError prepared = prepareNewFile(path, real); prepared != IoError::Ok) {
        return prepared;
    }

    // Streamed, not materialized: the bytes reach the file as they are pulled, so a model that is generated on the fly
    // (and a source that cannot state its length) never has to exist whole in memory first.
    return writeThroughTemporary(real, [&source](std::ostream& out) {
        vn::io::OstreamSink sink(out);
        return detail::pumpSource(*source, sink);
    });
}

IoError DirectoryVfs::addFile(const std::filesystem::path& path, std::span<const Fragment> fragments)
{
    for (const Fragment& piece : fragments) {
        if (piece.data == nullptr && piece.size != 0) {
            return IoError::InvalidData; // a piece that claims bytes it does not have
        }
    }

    std::filesystem::path real;
    if (const IoError prepared = prepareNewFile(path, real); prepared != IoError::Ok) {
        return prepared;
    }

    // Each piece is written where it is, so borrowed data that already lives in more than one buffer is never
    // concatenated first - which is what the base's default does with the same arguments.
    return writeThroughTemporary(real, [fragments](std::ostream& out) {
        for (const Fragment& piece : fragments) {
            if (piece.size != 0) {
                out.write(reinterpret_cast<const char*>(piece.data), static_cast<std::streamsize>(piece.size));
            }
        }
        return out.good() ? IoError::Ok : IoError::IoFailure;
    });
}

IoError DirectoryVfs::prepareNewFile(const std::filesystem::path& path, std::filesystem::path& real)
{
    if (isReadOnly()) {
        return IoError::ReadOnly;
    }
    std::filesystem::path norm;
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
    return IoError::Ok;
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

VN_IO_NS_END
