#include <vine/io/ZipArchive.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <zip.h>
#include <zlib.h>

#include "VfsInternal.hpp"

V_IO_NS_BEGIN

namespace
{

/**
 * @brief Returns true when an entry name cannot escape the extraction root.
 */
bool isSafeEntry(const std::string& name)
{
    return !name.empty() && name.front() != '/' && name.find("..") == std::string::npos;
}

/**
 * @brief Reads the entry index of an open archive.
 *
 * A ZIP marks a directory by a trailing '/'; such an entry carries no data, so
 * its size is reported as 0.
 */
Result<std::vector<ZipEntryInfo>> readEntries(zip_t* archive)
{
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    if (count < 0) {
        return IoError::InvalidData;
    }

    std::vector<ZipEntryInfo> entries;
    entries.reserve(static_cast<std::size_t>(count));
    for (zip_int64_t i = 0; i < count; ++i) {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, i, ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_CRC, &st) != 0 || st.name == nullptr) {
            return IoError::InvalidData;
        }

        ZipEntryInfo entry;
        entry.name         = detail::fromUtf8(st.name, std::strlen(st.name));
        entry.is_directory = !entry.name.empty() && entry.name.as_std_u8str().back() == u8'/';
        if (!entry.is_directory && (st.valid & ZIP_STAT_SIZE) != 0) {
            entry.size = static_cast<std::uint64_t>(st.size);
        }
        if (!entry.is_directory && (st.valid & ZIP_STAT_CRC) != 0) {
            entry.crc = static_cast<std::uint32_t>(st.crc);
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

/**
 * @brief Reads a whole stream into a byte buffer.
 */
std::vector<unsigned char> readStream(std::istream& in)
{
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

/**
 * @brief The name a ZIP stores for an entry; directories end with '/'.
 */
std::string toStoredName(const String& name, bool is_directory)
{
    std::string stored(reinterpret_cast<const char*>(name.data()), static_cast<std::size_t>(name.size()));
    if (is_directory && !stored.empty() && stored.back() != '/') {
        stored.push_back('/');
    }
    return stored;
}

/**
 * @brief Removes the trailing '/' a ZIP stores for a directory.
 */
String fromStoredName(const String& name, bool& is_directory)
{
    is_directory = !name.empty() && name.as_std_u8str().back() == u8'/';
    if (!is_directory) {
        return name;
    }
    std::u8string trimmed = name.as_std_u8str();
    trimmed.pop_back();
    return String(std::move(trimmed));
}

/**
 * @brief Bridges a DataSource onto the libzip source callback protocol.
 *
 * libzip pulls content when it writes the archive, so this keeps the pull
 * position and enforces that a source produces exactly the length it promised:
 * a source that runs short fails the save instead of silently truncating an
 * entry.
 */
class GeneratorBridge
{
  public:
    explicit GeneratorBridge(std::shared_ptr<DataSource> source) : source_(std::move(source))
    {
        zip_error_init(&error_);
    }

    ~GeneratorBridge()
    {
        zip_error_fini(&error_);
    }

    GeneratorBridge(const GeneratorBridge&) = delete;
    GeneratorBridge& operator=(const GeneratorBridge&) = delete;

    void rewind()
    {
        source_->rewind();
        produced_ = 0;
    }

    std::uint64_t size() const { return source_->size(); }

    zip_int64_t read(void* data, zip_uint64_t length)
    {
        const std::uint64_t promised  = size();
        const std::uint64_t remaining = produced_ < promised ? promised - produced_ : 0;
        const zip_uint64_t  wanted    = std::min<zip_uint64_t>(length, remaining);
        if (wanted == 0) {
            return 0; // end of content, or nothing left to promise
        }

        const std::size_t got = source_->read(
            std::span<std::byte>(static_cast<std::byte*>(data), static_cast<std::size_t>(wanted)));
        if (got == 0 || got > wanted) {
            // A source that stops early would leave the entry shorter than the
            // size written into the archive, so this is an error, not an end.
            zip_error_set(&error_, ZIP_ER_READ, 0);
            return -1;
        }
        produced_ += got;
        return static_cast<zip_int64_t>(got);
    }

    zip_error_t* error() { return &error_; }

  private:
    std::shared_ptr<DataSource> source_;
    zip_error_t                 error_{};
    std::uint64_t               produced_{ 0 };
};

/**
 * @brief The libzip callback that pulls from a GeneratorBridge.
 */
zip_int64_t generatorCallback(void* userdata, void* data, zip_uint64_t length, zip_source_cmd_t command)
{
    auto* bridge = static_cast<GeneratorBridge*>(userdata);
    switch (command) {
    case ZIP_SOURCE_OPEN:
        bridge->rewind();
        return 0;
    case ZIP_SOURCE_READ:
        return bridge->read(data, length);
    case ZIP_SOURCE_CLOSE:
        return 0;
    case ZIP_SOURCE_STAT: {
        auto* stat = static_cast<struct zip_stat*>(data);
        zip_stat_init(stat);
        stat->size  = bridge->size();
        stat->valid = ZIP_STAT_SIZE;
        return sizeof(struct zip_stat);
    }
    case ZIP_SOURCE_ERROR:
        return zip_error_to_data(bridge->error(), data, length);
    case ZIP_SOURCE_FREE:
        delete bridge;
        return 0;
    case ZIP_SOURCE_SUPPORTS:
        return zip_source_make_command_bitmap(ZIP_SOURCE_OPEN, ZIP_SOURCE_READ, ZIP_SOURCE_CLOSE, ZIP_SOURCE_STAT,
                                              ZIP_SOURCE_ERROR, ZIP_SOURCE_FREE, -1);
    default:
        zip_error_set(bridge->error(), ZIP_ER_OPNOTSUPP, 0);
        return -1;
    }
}

/**
 * @brief A source that walks a list of borrowed pieces without copying them.
 */
class FragmentSource final : public DataSource
{
  public:
    explicit FragmentSource(std::span<const Fragment> fragments) : fragments_(fragments.begin(), fragments.end())
    {
        for (const Fragment& fragment : fragments_) {
            total_ += fragment.size;
        }
    }

    std::uint64_t size() const override { return total_; }

    void rewind() override
    {
        index_  = 0;
        offset_ = 0;
    }

    std::size_t read(std::span<std::byte> out) override
    {
        std::size_t written = 0;
        while (written < out.size() && index_ < fragments_.size()) {
            const Fragment&     fragment  = fragments_[index_];
            const std::uint64_t available = fragment.size - offset_;
            if (available == 0) {
                ++index_;
                offset_ = 0;
                continue;
            }

            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(available, out.size() - written));
            std::memcpy(out.data() + written, fragment.data + offset_, take);
            written += take;
            offset_ += take;
            if (offset_ == fragment.size) {
                ++index_;
                offset_ = 0;
            }
        }
        return written;
    }

  private:
    std::vector<Fragment> fragments_;
    std::uint64_t         total_{ 0 };
    std::size_t           index_{ 0 };
    std::uint64_t         offset_{ 0 };
};

} // namespace

/**
 * @brief Holds an open source archive; the bytes it borrows live here too.
 */
class ZipArchive::EntryReadStream final : public VfsReadStream
{
  public:
    EntryReadStream(std::shared_ptr<ArchiveHandle> handle, zip_file_t* file, std::uint64_t size,
                    std::uint32_t expected_crc)
        : handle_(std::move(handle)), file_(file), size_(size), expected_crc_(expected_crc)
    {
    }

    explicit EntryReadStream(std::vector<unsigned char> bytes) : bytes_(std::move(bytes)), size_(bytes_.size()) {}

    ~EntryReadStream() override
    {
        if (file_ != nullptr) {
            zip_fclose(file_);
        }
    }

    EntryReadStream(const EntryReadStream&) = delete;
    EntryReadStream& operator=(const EntryReadStream&) = delete;

    std::size_t read(std::span<std::byte> out) override
    {
        if (out.empty()) {
            return 0;
        }
        if (file_ == nullptr) {
            const std::size_t available = static_cast<std::size_t>(size_ - position_);
            const std::size_t take      = std::min(available, out.size());
            std::memcpy(out.data(), bytes_.data() + position_, take);
            position_ += take;
            return take;
        }

        const zip_int64_t got = zip_fread(file_, out.data(), out.size());
        if (got < 0) {
            error_ = IoError::IoFailure;
            return 0;
        }
        position_ += static_cast<std::uint64_t>(got);
        if (crc_valid_ && got > 0) {
            crc_ = static_cast<std::uint32_t>(::crc32(crc_, reinterpret_cast<const unsigned char*>(out.data()),
                                                      static_cast<uInt>(got)));
            if (position_ == size_ && expected_crc_ != 0 && crc_ != expected_crc_) {
                // The last byte arrived, so the checksum can be settled - and libzip
                // itself does not look at it while reading.
                error_ = IoError::IoFailure;
            }
        }
        return static_cast<std::size_t>(got);
    }

    std::uint64_t size() const noexcept override { return size_; }

    IoError error() const override { return error_; }

    bool seekable() const noexcept override { return file_ == nullptr || zip_file_is_seekable(file_) != 0; }

    IoError seek(std::uint64_t offset) override
    {
        if (offset > size_) {
            return IoError::OutOfRange;
        }
        if (file_ == nullptr) {
            position_ = offset;
            crc_valid_ = offset == 0;
            crc_       = 0;
            return IoError::Ok;
        }
        if (zip_fseek(file_, static_cast<zip_int64_t>(offset), SEEK_SET) != 0) {
            return IoError::OutOfRange;
        }
        position_ = offset;
        // The running checksum only holds for a straight read, so any seek but a
        // rewind turns the check off.
        crc_valid_ = offset == 0;
        crc_       = 0;
        return IoError::Ok;
    }

  private:
    std::shared_ptr<ArchiveHandle> handle_; ///< Keeps the source archive alive for this reader.
    std::vector<unsigned char>     bytes_;  ///< Buffered content, when the entry is not source-backed.
    zip_file_t*                    file_{ nullptr };
    std::uint64_t                  size_{ 0 };
    std::uint64_t                  position_{ 0 };
    std::uint32_t                  crc_{ 0 };                            ///< Running checksum of what was read.
    std::uint32_t                  expected_crc_{ 0 };                   ///< What the archive directory recorded.
    bool                           crc_valid_{ true };                   ///< Cleared when the reader seeks off the start.
    IoError                        error_{ IoError::Ok };
};

ZipArchive::ArchiveHandle::ArchiveHandle() = default;

ZipArchive::ArchiveHandle::~ArchiveHandle()
{
    if (archive != nullptr) {
        zip_close(static_cast<zip_t*>(archive));
    }
}

ZipArchive::ZipArchive() = default;
ZipArchive::~ZipArchive() = default;
ZipArchive::ZipArchive(ZipArchive&&) noexcept = default;
ZipArchive& ZipArchive::operator=(ZipArchive&&) noexcept = default;

bool ZipArchive::insertBytes(const String& path, std::span<const unsigned char> bytes)
{
    if (path.empty()) {
        return false;
    }

    Entry entry;
    entry.data.assign(bytes.begin(), bytes.end());
    entries_.insert_or_assign(path, std::move(entry));
    return true;
}

bool ZipArchive::insertFileBacked(const String& path, const std::filesystem::path& src_path)
{
    if (path.empty() || src_path.empty()) {
        return false;
    }
    Entry entry;
    entry.src       = src_path;
    entry.from_file = true;
    entries_.insert_or_assign(path, std::move(entry));
    return true;
}

bool ZipArchive::addFile(const String& name, std::shared_ptr<DataSource> source)
{
    if (name.empty() || source == nullptr) {
        return false;
    }
    Entry entry;
    entry.generator = std::move(source);
    entries_.insert_or_assign(name, std::move(entry));
    return true;
}

bool ZipArchive::addFile(const String& name, std::span<const Fragment> fragments)
{
    if (name.empty() || fragments.empty()) {
        return false;
    }
    return addFile(name, std::make_shared<FragmentSource>(fragments));
}

bool ZipArchive::insertDirectory(const String& path)
{
    if (path.empty()) {
        return false;
    }

    Entry entry;
    entry.is_directory = true;
    entries_.insert_or_assign(path, std::move(entry));
    return true;
}

bool ZipArchive::removeEntry(const String& name)
{
    return entries_.erase(name) > 0;
}

bool ZipArchive::renameEntry(const String& from, const String& to)
{
    const auto it = entries_.find(from);
    if (it == entries_.end() || to.empty() || entries_.find(to) != entries_.end()) {
        return false;
    }
    Entry entry = std::move(it->second);
    entries_.erase(it);
    entries_.insert_or_assign(to, std::move(entry));
    return true;
}

bool ZipArchive::isOpen() const noexcept
{
    return handle_ != nullptr;
}

std::vector<ZipEntryInfo> ZipArchive::index() const
{
    std::vector<ZipEntryInfo> listed;
    listed.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        ZipEntryInfo info;
        info.name         = name;
        info.is_directory = entry.is_directory;
        if (!info.is_directory) {
            info.size = entrySize(entry);
        }
        listed.push_back(std::move(info));
    }
    return listed;
}

ZipArchive::EntryKind ZipArchive::kindOf(const String& name) const
{
    const auto it = entries_.find(name);
    if (it != entries_.end()) {
        return it->second.is_directory ? EntryKind::Directory : EntryKind::File;
    }
    for (const auto& [key, entry] : entries_) {
        (void)entry;
        if (detail::isPathBelow(name, key)) {
            return EntryKind::Directory; // a directory that exists through its children
        }
    }
    return EntryKind::Missing;
}

std::uint64_t ZipArchive::sizeOf(const String& name) const
{
    const auto it = entries_.find(name);
    return it == entries_.end() ? 0 : entrySize(it->second);
}

std::vector<ZipEntryInfo> ZipArchive::children(const String& dir) const
{
    std::vector<ZipEntryInfo> children;
    const std::u8string       prefix = dir.empty() ? std::u8string() : dir.as_std_u8str() + u8"/";
    std::set<std::u8string>   seen;
    for (const auto& [key, entry] : entries_) {
        const std::u8string& k = key.as_std_u8str();
        if (!prefix.empty() && k.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }

        const std::size_t   slash   = k.find(u8'/', prefix.size());
        const bool          is_leaf = slash == std::u8string::npos;
        const std::u8string segment = k.substr(prefix.size(), is_leaf ? std::u8string::npos : slash - prefix.size());
        if (segment.empty() || !seen.insert(segment).second) {
            continue;
        }

        ZipEntryInfo child;
        child.name         = dir.empty() ? String(segment) : String(dir.as_std_u8str() + u8"/" + segment);
        child.is_directory = !is_leaf || entry.is_directory;
        if (!child.is_directory) {
            child.size = entrySize(entry);
        }
        children.push_back(std::move(child));
    }
    return children;
}

std::uint64_t ZipArchive::entrySize(const Entry& entry) const
{
    if (entry.from_source) {
        return entry.size;
    }
    if (entry.from_file) {
        std::error_code      ec;
        const std::uintmax_t bytes = std::filesystem::file_size(entry.src, ec);
        return ec ? 0 : static_cast<std::uint64_t>(bytes);
    }
    if (entry.generator != nullptr) {
        return entry.generator->size();
    }
    return static_cast<std::uint64_t>(entry.data.size());
}

Result<std::vector<unsigned char>> ZipArchive::contentOf(const Entry& entry) const
{
    if (entry.from_file) {
        std::ifstream in(entry.src, std::ios::binary);
        if (!in) {
            return IoError::IoFailure;
        }
        std::vector<unsigned char> bytes = readStream(in);
        if (in.bad()) {
            return IoError::IoFailure;
        }
        return bytes;
    }
    if (entry.generator == nullptr) {
        return entry.data;
    }

    // A generator is pulled in chunks; the archive only holds its promise.
    std::vector<unsigned char> bytes(static_cast<std::size_t>(entry.generator->size()));
    entry.generator->rewind();
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t got = entry.generator->read(
            std::span<std::byte>(reinterpret_cast<std::byte*>(bytes.data()) + done, bytes.size() - done));
        if (got == 0) {
            return IoError::InvalidData; // shorter than promised
        }
        done += got;
    }
    return bytes;
}

Result<std::vector<unsigned char>> ZipArchive::read(const String& path) const
{
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    return readStored(norm);
}

Result<std::vector<unsigned char>> ZipArchive::readStored(const String& name) const
{
    const EntryKind kind = kindOf(name);
    if (kind == EntryKind::Missing) {
        return IoError::NotFound;
    }
    if (kind == EntryKind::Directory) {
        return IoError::IsADirectory;
    }

    const Entry& entry = entries_.at(name);
    if (!entry.from_source) {
        return contentOf(entry);
    }
    if (handle_ == nullptr) {
        return IoError::IoFailure;
    }

    zip_file_t* const file = zip_fopen_index(static_cast<zip_t*>(handle_->archive),
                                             static_cast<zip_uint64_t>(entry.source_index), ZIP_FL_UNCHANGED);
    if (file == nullptr) {
        return IoError::IoFailure;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(entry.size));
    std::size_t                done = 0;
    bool                       ok   = true;
    while (done < bytes.size()) {
        const zip_int64_t got = zip_fread(file, bytes.data() + done, bytes.size() - done);
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    // Decompression finishes, and the content is checked, only once the stream is
    // read past its last byte - so this probe is what turns a corrupt entry into an
    // error instead of silently wrong bytes.
    if (ok && done == bytes.size()) {
        std::byte probe{};
        if (zip_fread(file, &probe, 1) < 0) {
            ok = false;
        }
    }
    zip_fclose(file);
    if (!ok || done != bytes.size()) {
        return IoError::IoFailure;
    }
    // libzip's read path does not check the checksum, so the content is verified here
    // against what the archive directory recorded for the entry.
    if (entry.crc != 0 && static_cast<std::uint32_t>(::crc32(0, bytes.data(), static_cast<uInt>(bytes.size()))) != entry.crc) {
        return IoError::IoFailure;
    }
    return bytes;
}

Result<std::unique_ptr<VfsReadStream>> ZipArchive::openRead(const String& name) const
{
    const EntryKind kind = kindOf(name);
    if (kind == EntryKind::Missing) {
        return IoError::NotFound;
    }
    if (kind == EntryKind::Directory) {
        return IoError::IsADirectory;
    }

    const Entry& entry = entries_.at(name);
    if (entry.from_source) {
        if (handle_ == nullptr) {
            return IoError::IoFailure;
        }
        zip_file_t* const file = zip_fopen_index(static_cast<zip_t*>(handle_->archive),
                                                static_cast<zip_uint64_t>(entry.source_index), ZIP_FL_UNCHANGED);
        if (file == nullptr) {
            return IoError::IoFailure;
        }
        // The handle is held by the reader, so the stream outlives this archive.
        return std::unique_ptr<VfsReadStream>(new EntryReadStream(handle_, file, entry.size, entry.crc));
    }

    auto bytes = contentOf(entry);
    if (!bytes) {
        return bytes.error();
    }
    return std::unique_ptr<VfsReadStream>(new EntryReadStream(bytes.take()));
}

Result<ZipArchive> ZipArchive::open(const std::filesystem::path& path)
{
    ZipArchive    archive;
    const IoError error = archive.adoptFile(path);
    if (error != IoError::Ok) {
        return error;
    }
    return archive;
}

Result<ZipArchive> ZipArchive::open(std::vector<unsigned char> bytes)
{
    ZipArchive    archive;
    const IoError error = archive.adoptBytes(std::move(bytes));
    if (error != IoError::Ok) {
        return error;
    }
    return archive;
}

IoError ZipArchive::adoptFile(const std::filesystem::path& path)
{
    // Decided from the file status, so "there is no such file" stays apart from
    // "this file is not an archive".
    std::error_code ec;
    const auto      status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return IoError::InvalidData;
    }

    auto handle     = std::make_shared<ArchiveHandle>();
    handle->archive = archive;
    return adoptHandle(std::move(handle), path);
}

IoError ZipArchive::adoptBytes(std::vector<unsigned char> bytes)
{
    if (bytes.empty()) {
        return IoError::InvalidData;
    }

    auto handle   = std::make_shared<ArchiveHandle>();
    handle->bytes = std::move(bytes);

    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* const source = zip_source_buffer_create(handle->bytes.data(), handle->bytes.size(), 0, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        zip_source_free(source);
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    zip_error_fini(&error);

    handle->archive = archive;
    return adoptHandle(std::move(handle), std::filesystem::path{});
}

IoError ZipArchive::adoptHandle(std::shared_ptr<ArchiveHandle> handle, const std::filesystem::path& path)
{
    const auto listed = readEntries(static_cast<zip_t*>(handle->archive));
    if (!listed) {
        return listed.error(); // the handle closes itself on the way out
    }

    std::map<String, Entry> entries;
    for (std::size_t i = 0; i < listed->size(); ++i) {
        const ZipEntryInfo& info = (*listed)[i];
        bool                is_directory = false;
        String              name         = fromStoredName(info.name, is_directory);
        if (name.empty()) {
            continue; // the root, which always exists
        }

        Entry entry;
        entry.from_source  = !is_directory; // a directory entry has no content to copy through
        entry.source_index = i;
        entry.is_directory = is_directory;
        entry.size         = info.size;
        entry.crc          = info.crc;
        entries.insert_or_assign(std::move(name), std::move(entry));
    }

    entries_     = std::move(entries);
    handle_      = std::move(handle);
    source_path_ = path;
    return IoError::Ok;
}

bool ZipArchive::emit(void* target) const
{
    auto* const archive = static_cast<zip_t*>(target);

    for (const auto& [name, entry] : entries_) {
        const std::string stored = toStoredName(name, entry.is_directory);
        zip_source_t*     source = nullptr;

        if (entry.is_directory) {
            source = zip_source_buffer(archive, nullptr, 0, 0);
        }
        else if (entry.from_source) {
            if (handle_ == nullptr) {
                return false;
            }
            // Copied straight across: the bytes stay compressed and are never decompressed.
            source = zip_source_zip_file(archive, static_cast<zip_t*>(handle_->archive),
                                         static_cast<zip_uint64_t>(entry.source_index), ZIP_FL_UNCHANGED, 0, -1,
                                         nullptr);
        }
        else if (entry.from_file) {
            zip_error_t error;
            zip_error_init(&error);
            const std::string src_utf8 = detail::toUtf8(entry.src);
            source                     = zip_source_file_create(src_utf8.c_str(), 0, -1, &error);
            zip_error_fini(&error);
        }
        else if (entry.generator != nullptr) {
            zip_error_t error;
            zip_error_init(&error);
            auto* const bridge = new GeneratorBridge(entry.generator);
            source             = zip_source_function_create(&generatorCallback, bridge, &error);
            if (source == nullptr) {
                delete bridge;
            }
            zip_error_fini(&error);
        }
        else {
            source = zip_source_buffer(archive, entry.data.data(), entry.data.size(), 0);
        }

        if (source == nullptr || zip_file_add(archive, stored.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
            zip_source_free(source);
            return false;
        }
    }
    return true;
}

IoError ZipArchive::saveAs(const std::filesystem::path& path) const
{
    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      target    = zip_open(path_utf8.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (target == nullptr) {
        return IoError::IoFailure;
    }

    if (!emit(target)) {
        zip_discard(target);
        return IoError::IoFailure;
    }
    if (zip_close(target) < 0) {
        zip_discard(target); // the handle survives a failed close
        return IoError::IoFailure;
    }
    return IoError::Ok;
}

Result<std::vector<unsigned char>> ZipArchive::buildToBytes() const
{
    zip_error_t error;
    zip_error_init(&error);
    // Growable in-memory source; kept alive past zip_close() to read it back.
    zip_source_t* const buffer = zip_source_buffer_create(nullptr, 0, 1, &error);
    if (buffer == nullptr) {
        zip_error_fini(&error);
        return IoError::IoFailure;
    }
    zip_t* const target = zip_open_from_source(buffer, ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (target == nullptr) {
        zip_source_free(buffer);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }
    zip_source_keep(buffer);

    if (!emit(target)) {
        zip_discard(target);
        zip_source_free(buffer);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }
    if (zip_close(target) < 0) {
        zip_source_free(buffer);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_source_stat(buffer, &st) < 0 || zip_source_open(buffer) < 0) {
        zip_source_free(buffer);
        zip_error_fini(&error);
        return IoError::IoFailure;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(st.size));
    const zip_uint64_t got = static_cast<zip_uint64_t>(zip_source_read(buffer, bytes.data(), st.size));
    zip_source_close(buffer);
    zip_source_free(buffer);
    zip_error_fini(&error);
    if (got != st.size) {
        return IoError::IoFailure;
    }
    return bytes;
}

Result<std::vector<unsigned char>> ZipArchive::toBytes() const
{
    return buildToBytes();
}

IoError ZipArchive::saveAs(std::ostream& out) const
{
    const auto bytes = buildToBytes();
    if (!bytes) {
        return bytes.error();
    }
    out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    return out.good() ? IoError::Ok : IoError::IoFailure;
}

IoError ZipArchive::commit()
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    if (source_path_.empty()) {
        return IoError::Unsupported; // nothing was opened from a file
    }

    std::filesystem::path temp = source_path_;
    temp += ".vine-tmp";
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    // Written next to the target, while the source handle is still open: the
    // untouched entries are copied from it.
    const IoError written = saveAs(temp);
    if (written != IoError::Ok) {
        std::filesystem::remove(temp, ec);
        return written;
    }

    // The source has to go before the replacement: Windows refuses to rename over
    // a file this process still has open.
    handle_.reset();
    std::filesystem::rename(temp, source_path_, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return IoError::IoFailure;
    }

    // Re-index the new file, so the changes are now source-backed entries.
    return adoptFile(source_path_);
}

bool ZipArchive::isReadOnly() const noexcept
{
    return read_only_;
}

IoError ZipArchive::guard() const
{
    return read_only_ ? IoError::ReadOnly : IoError::Ok;
}

bool ZipArchive::isDirectoryPath(const String& normalized) const
{
    return normalized.empty() || kindOf(normalized) == EntryKind::Directory;
}

IoError ZipArchive::ancestorBlockerOf(const String& normalized) const
{
    for (String ancestor = detail::parentOf(normalized); !ancestor.empty(); ancestor = detail::parentOf(ancestor)) {
        const EntryKind kind = kindOf(ancestor);
        if (kind == EntryKind::File) {
            return IoError::NotADirectory;
        }
        if (kind == EntryKind::Directory) {
            return IoError::Ok;
        }
    }
    return IoError::Ok;
}

Result<FileInfo> ZipArchive::stat(const String& path) const
{
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return FileInfo{ String{}, true, 0 };
    }

    switch (kindOf(norm)) {
    case EntryKind::File:
        return FileInfo{ norm, false, sizeOf(norm) };
    case EntryKind::Directory:
        return FileInfo{ norm, true, 0 };
    case EntryKind::Missing:
        break;
    }
    return IoError::NotFound;
}

Result<std::vector<FileInfo>> ZipArchive::list(const String& dir) const
{
    String        norm;
    const IoError error = detail::normalizeVfsPath(dir, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (!norm.empty() && kindOf(norm) == EntryKind::Missing) {
        return IoError::NotFound;
    }
    if (!norm.empty() && kindOf(norm) == EntryKind::File) {
        return IoError::NotADirectory;
    }

    std::vector<FileInfo> listed;
    for (const ZipEntryInfo& entry : children(norm)) {
        listed.push_back(FileInfo{ entry.name, entry.is_directory, entry.size });
    }
    return listed;
}

IoError ZipArchive::write(const String& path, std::span<const unsigned char> bytes)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (isDirectoryPath(norm)) {
        return IoError::IsADirectory; // the root, or a directory, is never replaced by a file
    }
    return insertBytes(norm, bytes) ? IoError::Ok : IoError::IoFailure;
}

IoError ZipArchive::createDirectory(const String& path)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return IoError::AlreadyExists; // the root always exists
    }
    if (kindOf(norm) != EntryKind::Missing) {
        return IoError::AlreadyExists; // a file or a directory owns the name
    }
    if (const IoError blocker = ancestorBlockerOf(norm); blocker != IoError::Ok) {
        return blocker;
    }

    const String parent = detail::parentOf(norm);
    if (!parent.empty() && !isDirectoryPath(parent)) {
        return IoError::NotFound; // mkdir, not mkdir -p
    }
    return insertDirectory(norm) ? IoError::Ok : IoError::IoFailure;
}

IoError ZipArchive::createDirectories(const String& path)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (isDirectoryPath(norm)) {
        return IoError::Ok; // already there - the root included
    }
    if (kindOf(norm) == EntryKind::File) {
        return IoError::AlreadyExists; // a file owns the name
    }
    if (const IoError blocker = ancestorBlockerOf(norm); blocker != IoError::Ok) {
        return blocker;
    }

    // Missing parents exist implicitly as soon as the entry is there, so only the
    // leaf needs a directory entry of its own.
    return insertDirectory(norm) ? IoError::Ok : IoError::IoFailure;
}

IoError ZipArchive::rename(const String& from, const String& to)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String  source;
    IoError error = detail::normalizeVfsPath(from, source);
    if (error != IoError::Ok) {
        return error;
    }
    String target;
    error = detail::normalizeVfsPath(to, target);
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
    if (kindOf(source) == EntryKind::Missing) {
        return IoError::NotFound;
    }
    if (kindOf(target) != EntryKind::Missing) {
        return IoError::AlreadyExists; // never overwrite
    }
    if (const IoError blocker = ancestorBlockerOf(target); blocker != IoError::Ok) {
        return blocker;
    }

    const String parent = detail::parentOf(target);
    if (!parent.empty() && !isDirectoryPath(parent)) {
        return IoError::NotFound; // the target's parent is missing
    }

    // A whole subtree moves with its directory, so every entry below it follows.
    std::vector<std::pair<String, String>> moved;
    for (const ZipEntryInfo& entry : index()) {
        if (entry.name == source || detail::isPathBelow(source, entry.name)) {
            const String suffix = entry.name.size() == source.size()
                                      ? String{}
                                      : String(entry.name.as_std_u8str().substr(source.size()));
            moved.emplace_back(entry.name, String(target.as_std_u8str() + suffix.as_std_u8str()));
        }
    }
    for (const auto& [old_name, new_name] : moved) {
        if (!renameEntry(old_name, new_name)) {
            return IoError::NotFound;
        }
    }
    return IoError::Ok;
}

IoError ZipArchive::remove(const String& path)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return IoError::InvalidPath; // the root cannot be removed
    }
    if (!children(norm).empty()) {
        return IoError::NotEmpty; // implicit or explicit, the directory holds entries
    }
    return removeEntry(norm) ? IoError::Ok : IoError::NotFound;
}

IoError ZipArchive::removeAll(const String& path)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (norm.empty()) {
        return IoError::InvalidPath; // the root cannot be removed
    }
    if (kindOf(norm) == EntryKind::Missing) {
        return IoError::NotFound;
    }

    std::vector<String> doomed;
    for (const ZipEntryInfo& entry : index()) {
        if (entry.name == norm || detail::isPathBelow(norm, entry.name)) {
            doomed.push_back(entry.name);
        }
    }
    for (const String& name : doomed) {
        removeEntry(name);
    }
    return IoError::Ok;
}

IoError ZipArchive::importFile(const String& path, const std::filesystem::path& real_path)
{
    if (const IoError blocked = guard(); blocked != IoError::Ok) {
        return blocked;
    }
    String        norm;
    const IoError error = detail::normalizeVfsPath(path, norm);
    if (error != IoError::Ok) {
        return error;
    }
    if (isDirectoryPath(norm)) {
        return IoError::IsADirectory; // the root, or a directory, is never replaced by a file
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

    // Nothing is buffered now; the file is read when the archive is persisted.
    return insertFileBacked(norm, real_path) ? IoError::Ok : IoError::IoFailure;
}

Result<ZipArchive> ZipArchive::openForRead(const std::filesystem::path& path)
{
    auto opened = open(path);
    if (!opened) {
        return opened.error();
    }
    ZipArchive archive = opened.take();
    archive.read_only_  = true;
    return archive;
}

Result<ZipArchive> ZipArchive::openForRead(std::vector<unsigned char> bytes)
{
    auto opened = open(std::move(bytes));
    if (!opened) {
        return opened.error();
    }
    ZipArchive archive = opened.take();
    archive.read_only_  = true;
    return archive;
}

Result<std::vector<ZipEntryInfo>> ZipArchive::entries(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return IoError::NotFound;
    }
    if (ec) {
        return IoError::IoFailure;
    }

    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return IoError::InvalidData;
    }
    Result<std::vector<ZipEntryInfo>> result = readEntries(archive);
    zip_close(archive);
    return result;
}

Result<std::vector<ZipEntryInfo>> ZipArchive::entries(const void* data, std::size_t size)
{
    if (data == nullptr) {
        return IoError::InvalidData;
    }
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* const source = zip_source_buffer_create(data, size, 0, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        zip_source_free(source);
        zip_error_fini(&error);
        return IoError::InvalidData;
    }
    Result<std::vector<ZipEntryInfo>> result = readEntries(archive);
    zip_close(archive);
    zip_error_fini(&error);
    return result;
}

bool ZipArchive::decompressFile(const std::filesystem::path& path, const std::filesystem::path& dir_path)
{
    const std::string path_utf8 = detail::toUtf8(path);
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::create_directories(dir_path, ec) && ec) {
        zip_close(archive);
        return false;
    }

    bool              ok    = true;
    const zip_int64_t count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count && ok; ++i) {
        struct zip_stat st;
        zip_stat_init(&st);
        if (zip_stat_index(archive, i, 0, &st) != 0 || st.name == nullptr) {
            ok = false;
            break;
        }
        const std::string name = st.name;
        if (!isSafeEntry(name)) {
            ok = false;
            break;
        }

        const std::filesystem::path target =
            dir_path / std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(st.name)));
        if (name.back() == '/') {
            if (!std::filesystem::create_directories(target, ec) && ec) {
                ok = false;
                break;
            }
            continue;
        }

        zip_file_t* const file = zip_fopen_index(archive, i, 0);
        if (file == nullptr) {
            ok = false;
            break;
        }
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            zip_fclose(file);
            ok = false;
            break;
        }

        std::array<char, 16 * 1024> chunk{};
        while (ok) {
            const zip_int64_t got = zip_fread(file, chunk.data(), chunk.size());
            if (got < 0) {
                ok = false;
                break;
            }
            if (got == 0) {
                break;
            }
            out.write(chunk.data(), static_cast<std::streamsize>(got));
            if (!out) {
                ok = false;
                break;
            }
        }
        zip_fclose(file);
        if (!ok) {
            break;
        }
    }
    zip_close(archive);
    return ok;
}

bool ZipArchive::readEntry(const std::filesystem::path& path, const String& name, std::vector<unsigned char>& out)
{
    const std::string path_utf8 = detail::toUtf8(path);
    const auto*       name_utf8 = reinterpret_cast<const char*>(name.data());
    int               error     = 0;
    zip_t* const      archive   = zip_open(path_utf8.c_str(), ZIP_RDONLY, &error);
    if (archive == nullptr) {
        return false;
    }
    const zip_int64_t index = zip_name_locate(archive, name_utf8, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        return false;
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, index, 0, &st) != 0) {
        zip_close(archive);
        return false;
    }
    zip_file_t* const file = zip_fopen_index(archive, index, 0);
    if (file == nullptr) {
        zip_close(archive);
        return false;
    }

    out.clear();
    out.resize(static_cast<std::size_t>(st.size));
    zip_uint64_t total = 0;
    while (total < st.size) {
        const zip_int64_t got = zip_fread(file, out.data() + total, st.size - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<zip_uint64_t>(got);
    }
    zip_fclose(file);
    zip_close(archive);
    if (total != st.size) {
        out.clear();
        return false;
    }
    return true;
}

bool ZipArchive::readEntry(const void* data, std::size_t size, const String& name, std::vector<unsigned char>& out)
{
    if (data == nullptr || name.empty()) {
        return false;
    }
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* const source = zip_source_buffer_create(data, size, 0, &error);
    if (source == nullptr) {
        zip_error_fini(&error);
        return false;
    }
    zip_t* const archive = zip_open_from_source(source, ZIP_RDONLY, &error);
    if (archive == nullptr) {
        zip_source_free(source);
        zip_error_fini(&error);
        return false;
    }

    const auto*       name_utf8 = reinterpret_cast<const char*>(name.data());
    const zip_int64_t index     = zip_name_locate(archive, name_utf8, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        zip_error_fini(&error);
        return false;
    }

    struct zip_stat st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, index, 0, &st) != 0) {
        zip_close(archive);
        zip_error_fini(&error);
        return false;
    }
    zip_file_t* const file = zip_fopen_index(archive, index, 0);
    if (file == nullptr) {
        zip_close(archive);
        zip_error_fini(&error);
        return false;
    }

    out.clear();
    out.resize(static_cast<std::size_t>(st.size));
    zip_uint64_t total = 0;
    while (total < st.size) {
        const zip_int64_t got = zip_fread(file, out.data() + total, st.size - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<zip_uint64_t>(got);
    }
    zip_fclose(file);
    zip_close(archive);
    zip_error_fini(&error);
    if (total != st.size) {
        out.clear();
        return false;
    }
    return true;
}

V_IO_NS_END
