#include <vine/backend_vk/internal/PipelineCacheFile.hpp>

#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <type_traits>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief Identifies this backend's cache file. A file that does not start with it is not ours to guess about.
constexpr std::array<char, 8> kMagic{ 'V', 'N', 'K', 'P', 'C', 'A', 'C', 'H' };

/// @brief Bytes every cache file spends before its blob: magic, format, key, blob size.
constexpr std::size_t kHeaderBytes = kMagic.size() +                          // magic
                                    sizeof(std::uint32_t) +                   // format version
                                    (4U * sizeof(std::uint32_t) + VK_UUID_SIZE) + // the key
                                    sizeof(std::uint64_t);                    // blob size

/**
 * @brief Appends the object representation of @p value to @p out.
 *
 * @details
 * - NATIVE BYTE ORDER ON PURPOSE: a file read on a machine of another byte order fails the magic (or the key) and is
 *   refused, which is the only safe answer - the blob itself is the driver's, and the driver's own rules apply.
 *
 * @param out   The buffer to append to.
 * @param value The value to append.
 */
template <typename T>
void append(std::vector<std::byte>& out, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>, "a cache file holds plain bytes");
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), first, first + sizeof(T));
}

/**
 * @brief Reads @p value from @p data at @p cursor, advancing the cursor.
 *
 * @param data   The bytes to read from (the caller checked the size).
 * @param cursor Read position, advanced past the value.
 * @param value  Receives the value.
 */
template <typename T>
void take(const std::byte* data, std::size_t& cursor, T& value)
{
    std::memcpy(&value, data + cursor, sizeof(T));
    cursor += sizeof(T);
}

} // namespace

PipelineCacheKey PipelineCacheKey::of(const VkPhysicalDeviceProperties& properties) noexcept
{
    PipelineCacheKey key;
    key.vendor_id      = properties.vendorID;
    key.device_id      = properties.deviceID;
    key.driver_version = properties.driverVersion;
    key.api_version    = properties.apiVersion;
    for (std::size_t index = 0; index < VK_UUID_SIZE; ++index) {
        key.uuid[index] = properties.pipelineCacheUUID[index];
    }
    return key;
}

bool PipelineCacheKey::operator==(const PipelineCacheKey& other) const noexcept
{
    return vendor_id == other.vendor_id && device_id == other.device_id && driver_version == other.driver_version &&
           api_version == other.api_version && uuid == other.uuid;
}

std::vector<std::byte> PipelineCacheFile::encode(const PipelineCacheKey& key, const std::span<const std::byte> blob)
{
    std::vector<std::byte> out;
    out.reserve(kHeaderBytes + blob.size());

    const auto* magic = reinterpret_cast<const std::byte*>(kMagic.data());
    out.insert(out.end(), magic, magic + kMagic.size());
    append(out, kFormatVersion);
    append(out, key.vendor_id);
    append(out, key.device_id);
    append(out, key.driver_version);
    append(out, key.api_version);
    const auto* uuid = key.uuid.data();
    out.insert(out.end(), reinterpret_cast<const std::byte*>(uuid), reinterpret_cast<const std::byte*>(uuid) + VK_UUID_SIZE);
    append(out, static_cast<std::uint64_t>(blob.size()));
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

std::vector<std::byte> PipelineCacheFile::decode(const std::span<const std::byte> file,
                                                 const PipelineCacheKey&        key,
                                                 PipelineCacheRefusal&          refusal)
{
    refusal = PipelineCacheRefusal::NotACache;
    if (file.size() < kHeaderBytes) {
        return {};
    }
    if (std::memcmp(file.data(), kMagic.data(), kMagic.size()) != 0) {
        return {};
    }

    std::size_t cursor = kMagic.size();
    std::uint32_t format = 0;
    take(file.data(), cursor, format);
    if (format != kFormatVersion) {
        return {};
    }

    PipelineCacheKey stored;
    take(file.data(), cursor, stored.vendor_id);
    take(file.data(), cursor, stored.device_id);
    take(file.data(), cursor, stored.driver_version);
    take(file.data(), cursor, stored.api_version);
    std::memcpy(stored.uuid.data(), file.data() + cursor, VK_UUID_SIZE);
    cursor += VK_UUID_SIZE;
    if (stored != key) {
        refusal = PipelineCacheRefusal::ForeignDevice;
        return {};
    }

    std::uint64_t size = 0;
    take(file.data(), cursor, size);
    if (size == 0U) {
        refusal = PipelineCacheRefusal::Empty;
        return {};
    }
    // STRICT: exactly the blob follows. Trailing bytes mean the file was not written by this format, and a shorter one
    // means the write was cut off - either way the driver must not be handed a half cache.
    if (size != static_cast<std::uint64_t>(file.size() - kHeaderBytes)) {
        return {};
    }

    refusal = PipelineCacheRefusal::Ok;
    return std::vector<std::byte>(file.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes), file.end());
}

std::vector<std::byte> PipelineCacheFile::read(const std::filesystem::path& directory,
                                               const PipelineCacheKey&      key,
                                               PipelineCacheRefusal&        refusal)
{
    const std::filesystem::path path = directory / kFileName;
    std::error_code             error;
    if (!std::filesystem::exists(path, error)) {
        refusal = PipelineCacheRefusal::Missing;
        return {};
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        refusal = PipelineCacheRefusal::Unreadable;
        return {};
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        refusal = PipelineCacheRefusal::Unreadable;
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        refusal = PipelineCacheRefusal::Unreadable;
        return {};
    }
    return decode(bytes, key, refusal);
}

bool PipelineCacheFile::write(const std::filesystem::path& directory,
                              const PipelineCacheKey&      key,
                              const std::span<const std::byte> blob)
{
    if (blob.empty()) {
        return false; // nothing to reuse: a file that would be refused on read is worse than no file
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return false;
    }

    const std::filesystem::path path    = directory / kFileName;
    const std::filesystem::path partial = directory / (std::string(kFileName) + ".tmp");
    {
        std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        const std::vector<std::byte> bytes = encode(key, blob);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(partial, error);
            return false;
        }
    }

    // ATOMIC ON POSIX; WINDOWS REFUSES A RENAME ONTO AN EXISTING FILE, so the target goes first. A crash between the
    // two loses the cache, never correctness - the next session starts cold and writes it again.
    std::filesystem::rename(partial, path, error);
    if (error) {
        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(partial, path, error);
    }
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    return true;
}

VN_BACKEND_VK_NS_END
