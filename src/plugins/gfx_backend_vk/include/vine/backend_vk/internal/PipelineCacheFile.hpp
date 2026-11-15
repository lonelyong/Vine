#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vine/backend_vk/backend_vk_global.hpp>

#include <volk.h>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief Which DRIVER AND DEVICE a persisted pipeline cache came from.
 *
 * @details
 * - A PIPELINE CACHE IS NOT PORTABLE, AND VULKAN DOES NOT CHECK FOR US. The spec says `pInitialData` must have been
 *   produced by the same implementation: handing a cache from another driver (or another device of the same model) to
 *   `vkCreatePipelineCache` is undefined behaviour, not a slower start. So the file carries this key and the backend
 *   refuses a file whose key is not the one the device states NOW.
 * - `uuid` IS `VkPhysicalDeviceProperties::pipelineCacheUUID`, the field Vulkan defines for exactly this question ("the
 *   implementation changes it when the cache is no longer valid"); the other four fields make a mismatch readable in a
 *   report instead of silent.
 * - IT IS A VALUE: two keys compare with `==`, and `of()` is the only place the fields are read from a device.
 */
struct PipelineCacheKey {
    std::uint32_t                          vendor_id{ 0 };      ///< `VkPhysicalDeviceProperties::vendorID`.
    std::uint32_t                          device_id{ 0 };      ///< `VkPhysicalDeviceProperties::deviceID`.
    std::uint32_t                          driver_version{ 0 }; ///< `VkPhysicalDeviceProperties::driverVersion`.
    std::uint32_t                          api_version{ 0 };    ///< `VkPhysicalDeviceProperties::apiVersion`.
    std::array<std::uint8_t, VK_UUID_SIZE> uuid{};              ///< `pipelineCacheUUID` (the driver's own cache identity).

    /**
     * @brief Reads the key one device states.
     *
     * @param properties The physical device's properties (the cache is keyed by that device, not by the session).
     * @return The key.
     */
    [[nodiscard]] static PipelineCacheKey of(const VkPhysicalDeviceProperties& properties) noexcept;

    /**
     * @brief Compares two keys field by field (identity, not "close enough").
     *
     * @param other Key to compare against.
     * @return true when both name the same device and driver cache identity.
     */
    [[nodiscard]] bool operator==(const PipelineCacheKey& other) const noexcept;

    /**
     * @brief The negation of @ref operator==.
     *
     * @param other Key to compare against.
     * @return true when the keys differ in any field.
     */
    [[nodiscard]] bool operator!=(const PipelineCacheKey& other) const noexcept
    {
        return !(*this == other);
    }
};

/** @brief What a persisted pipeline cache file answered when the backend tried to read it. */
enum class PipelineCacheRefusal : std::uint8_t
{
    Ok,            ///< The blob was accepted and handed to `vkCreatePipelineCache`.
    Missing,       ///< There is no file - the ordinary first run, not a problem.
    Unreadable,    ///< The file exists and could not be read (permissions, a directory, I/O).
    NotACache,     ///< Shorter than a header, or its magic/format belongs to something else.
    ForeignDevice, ///< It is our format, but written by another device or driver (see PipelineCacheKey).
    Empty,         ///< It is our file, but it held no blob - nothing to reuse.
};

/**
 * @brief The persisted pipeline cache file: a device-keyed wrapper around the blob `vkGetPipelineCacheData` answers.
 *
 * @details
 * - WHAT IT IS FOR. A `VkPipelineCache` lives from `vkCreatePipelineCache` to `vkDestroyPipelineCache`; without a file
 *   every new session recompiles every pipeline's SPIR-V, which is the cold-start cost the review's F4 named. This unit
 *   is the only place that knows the bytes on disk; the Vulkan object stays `ContentDraw`'s.
 * - THE LOCATION IS NOT THIS UNIT'S DECISION. The host names a directory (`DeviceDesc::cache_directory`, defaulted by
 *   the plugin to its application data directory when a host loaded it through one); the backend puts
 *   @ref kFileName inside it. A backend has no business inventing where a user's files go.
 * - THE FORMAT IS [magic][format version][key][blob size][blob], written in native byte order. A foreign byte order or
 *   a foreign device is REFUSED by the key, never trusted; the strict reading below means a corrupt or truncated file
 *   is refused rather than half-parsed.
 * - WRITING IS ATOMIC: the bytes go to a sibling temporary file which is then renamed over the target, so a session
 *   that dies mid-write leaves the previous cache intact instead of a truncated one.
 * - IT DECIDES NOTHING AND REPORTS NOTHING: the refusal is a value the device classifies and reports once per episode;
 *   a cache that cannot be read is a slower start, never a failed session.
 *
 * @warning Not thread-safe, like the rest of the backend: one session at a time, on the thread that drives the device.
 */
struct VN_BACKEND_VK_API PipelineCacheFile {
    /// The file name inside the host's directory (one cache per directory, because the directory is the host's unit).
    static constexpr const char* kFileName = "vk_pipeline_cache.bin";

    /// The format this build writes; a file with another version is refused (`NotACache`), never guessed at.
    static constexpr std::uint32_t kFormatVersion = 1U;

    /**
     * @brief Encodes @p key and @p blob into the bytes a cache file holds.
     *
     * @details
     * - THE ONE ENCODER: `write` uses it, and a test that fakes a file for another device uses the same function -
     *   so the format has one definition and a fake cannot drift away from what the reader expects.
     *
     * @param key  The device the blob came from.
     * @param blob The bytes `vkGetPipelineCacheData` answered (may be empty, which the reader refuses).
     * @return The file's bytes.
     */
    [[nodiscard]] static std::vector<std::byte> encode(const PipelineCacheKey& key, std::span<const std::byte> blob);

    /**
     * @brief Decodes a cache file's bytes against the key the device states NOW.
     *
     * @details
     * - STRICT, IN THIS ORDER: long enough for a header; magic; format version; key equality; a blob size that is
     *   exactly the remaining bytes. Every failure names its refusal, which is what makes a "the cache never loads"
     *   report answerable instead of a mystery.
     *
     * @param file    The bytes read from disk (may be empty).
     * @param key     The key this device states now.
     * @param refusal Receives why the blob was refused (Ok when it was not).
     * @return The blob to hand to `vkCreatePipelineCache`, or empty when @p refusal is not Ok.
     */
    [[nodiscard]] static std::vector<std::byte> decode(std::span<const std::byte> file,
                                                        const PipelineCacheKey&    key,
                                                        PipelineCacheRefusal&      refusal);

    /**
     * @brief Reads `<directory>/kFileName` and decodes it against @p key.
     *
     * @param directory The host's directory (may not exist: that is @ref PipelineCacheRefusal::Missing).
     * @param key       The key this device states now.
     * @param refusal   Receives why the blob was refused (Ok when it was not).
     * @return The blob, or empty when nothing usable was there.
     */
    [[nodiscard]] static std::vector<std::byte> read(const std::filesystem::path& directory,
                                                      const PipelineCacheKey&      key,
                                                      PipelineCacheRefusal&        refusal);

    /**
     * @brief Writes @p blob keyed by @p key to `<directory>/kFileName`, creating the directory when needed.
     *
     * @details
     * - TEMPORARY FILE, THEN RENAME (see the class note). A directory that cannot be created, or a write that fails
     *   part way, answers false and leaves whatever was there before - the cache is an optimisation, never a
     *   correctness dependency.
     * - AN EMPTY BLOB IS NOT WRITTEN: a session that built no pipeline has nothing to reuse, and a file that would be
     *   refused on read is worse than no file.
     *
     * @param directory The host's directory (created when missing).
     * @param key       The key of the device that produced @p blob.
     * @param blob      The bytes `vkGetPipelineCacheData` answered.
     * @return true when the file now holds this session's cache.
     */
    [[nodiscard]] static bool write(const std::filesystem::path& directory,
                                    const PipelineCacheKey&      key,
                                    std::span<const std::byte>   blob);
};

VN_BACKEND_VK_NS_END
