#pragma once
#include "runtime_global.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <vine/String.hpp>

#include "Cache.hpp"

VN_RUNTIME_NS_BEGIN

/**
 * @brief The byte store a SerializedCache keeps its values in.
 *
 * @details
 * - This is the seam for persistent mechanisms. A file store, a database store
 *   and a network store implement it in the module that owns that mechanism,
 *   because each needs it (`vn::io`, a database driver, a socket). `runtime`
 *   defines the contract and stays free of those dependencies.
 * - Keys are UTF-8 strings; a value is opaque bytes.
 * - Expiry is an absolute `system_clock` time point, not a duration: a stored
 *   record must still expire after the process restarts. An empty optional means
 *   "no expiry". A store reports an expired record as `Miss`, never as a value.
 * - Implementations must be safe for concurrent use; the store owns whatever
 *   locking its mechanism needs.
 * - `count()` and `keys()` are optional capabilities. A store that cannot
 *   enumerate answers `Failure` with `CacheError::Unsupported`; it must not
 *   invent an empty answer.
 */
class VN_RUNTIME_API KeyValueStore {
  public:
    /**
     * @brief Creates the store.
     */
    KeyValueStore();

    /**
     * @brief Destroys the store.
     */
    virtual ~KeyValueStore();

    /**
     * @brief Returns the stored bytes for a key.
     *
     * @param key UTF-8 key.
     * @return `Ok` with the bytes, `Miss` when the key is absent or expired, or
     *         `Failure` with the reason.
     */
    virtual CacheOutcome<std::vector<std::byte>> load(const String& key) = 0;

    /**
     * @brief Stores or replaces the bytes for a key.
     *
     * @param key UTF-8 key.
     * @param bytes Bytes to store; copied by the store.
     * @param expires_at Absolute expiry, or an empty optional for no expiry.
     * @return `Ok`, or `Failure` with the reason.
     */
    virtual CacheResult store(const String& key, std::span<const std::byte> bytes, std::optional<std::chrono::system_clock::time_point> expires_at) = 0;

    /**
     * @brief Removes one key.
     *
     * @param key UTF-8 key.
     * @return `Ok` with true when a record was removed, or `Failure`.
     */
    virtual CacheOutcome<bool> erase(const String& key) = 0;

    /**
     * @brief Removes every record.
     *
     * @return `Ok`, or `Failure` with the reason.
     */
    virtual CacheResult clear() = 0;

    /**
     * @brief Returns the number of live records.
     *
     * @return `Ok` with the count, `Failure` with `CacheError::Unsupported` when
     *         the store cannot enumerate, or `Failure` with another reason.
     */
    virtual CacheOutcome<std::size_t> count() = 0;

    /**
     * @brief Returns the live keys.
     *
     * @return `Ok` with the keys (order unspecified), `Failure` with
     *         `CacheError::Unsupported` when the store cannot enumerate, or
     *         `Failure` with another reason.
     */
    virtual CacheOutcome<std::vector<String>> keys() = 0;
};

VN_RUNTIME_NS_END
