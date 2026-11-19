#pragma once
#include "runtime_global.hpp"

#include <any>
#include <chrono>
#include <cstddef>
#include <vector>

#include <vine/String.hpp>
#include <vine/runtime/CacheManager.hpp>
#include <vine/runtime/InMemoryCache.hpp>

VN_RUNTIME_NS_BEGIN

/**
 * @brief Bridge exposing the in-memory cache through the CacheManager interface.
 *
 * InMemoryCache<TKey, TVal> is intentionally standalone and does not derive from
 * CacheManager. InMemoryManager is the non-template adapter that implements the
 * String/any interface by delegating to InMemoryCache<String, std::any>, so a
 * host can register one cache service (vn::di) and later swap the backing store
 * for a shared or distributed one without changing the call sites, which keep
 * std::any_cast.
 *
 * It keeps the same thread-safe TTL semantics as the template, including the
 * shared, process-lifetime background sweeper (see the sweep_interval argument).
 *
 * @note
 * - The constructor always registers the cache for sweeping, even when every
 *   entry uses a zero lifetime; the worker is shared per stored type and sleeps
 *   while no cache is registered.
 * - Values are stored by value as std::any and copied out on get(); the caller
 *   casts them back with std::any_cast.
 */
class VN_RUNTIME_API InMemoryManager final : public CacheManager {
  public:
    /**
     * @brief Constructs an empty cache and registers it for background sweeps.
     *
     * @param sweep_interval Interval between automatic expiry sweeps; a value
     *                        below the cache's minimum period is raised to it.
     */
    explicit InMemoryManager(std::chrono::milliseconds sweep_interval = std::chrono::seconds(1));

    /**
     * @brief Unregisters the cache from the sweeper and destroys all stored values.
     */
    ~InMemoryManager() override;

    InMemoryManager(const InMemoryManager&)            = delete;
    InMemoryManager& operator=(const InMemoryManager&) = delete;

  public:
    /**
     * @brief Stores a value under a key.
     *
     * @details
     * - A timeout of 0 keeps the entry forever; a timeout beyond what the
     *   underlying cache can represent is clamped to its maximum lifetime
     *   rather than wrapping to a negative (forever) value.
     *
     * @param key Cache key.
     * @param value Value to store; copied into the cache as std::any.
     * @param timeout_seconds Lifetime in seconds; 0 keeps the entry forever.
     */
    void set(const String& key, std::any value, size_t timeout_seconds) override;

    /**
     * @brief Retrieves a non-expired value.
     *
     * @param key Cache key.
     * @return The stored value, or an empty std::any when absent or expired.
     */
    std::any get(const String& key) override;

    /**
     * @brief Returns whether a non-expired entry exists.
     *
     * @param key Cache key.
     * @return true when present and not expired.
     */
    bool contains(const String& key) override;

    /**
     * @brief Removes one entry.
     *
     * @param key Cache key.
     * @return true when an entry was removed.
     */
    bool remove(const String& key) override;

    /**
     * @brief Removes all expired entries.
     *
     * @return The number of removed entries.
     */
    size_t removeExpired() override;

    /**
     * @brief Removes all entries.
     */
    void clear() override;

    /**
     * @brief Returns the number of live (non-expired) entries.
     *
     * @return The number of live entries.
     */
    size_t count() override;

    /**
     * @brief Returns whether the cache holds no live entry.
     *
     * @return true when there is no live entry.
     */
    bool isEmpty() override;

    /**
     * @brief Returns the keys of all live entries.
     *
     * @return The live keys; order is unspecified.
     */
    std::vector<String> keys() override;

  private:
    InMemoryCache<String, std::any> cache_;
};

VN_RUNTIME_NS_END
