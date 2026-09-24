#pragma once
#include "runtime_global.hpp"

#include <any>
#include <cstddef>
#include <vector>

#include <vine/String.hpp>

VN_RUNTIME_NS_BEGIN

/**
 * @brief Type-erased, thread-safe, TTL-aware in-memory cache abstraction.
 *
 * Callers depend on this interface so the backing store can be swapped (e.g.
 * a plain in-memory map vs. a shared/distributed cache). Values are stored by
 * value as std::any and copied out on get(); callers cast them back to the
 * concrete type with std::any_cast<T>. A lifetime of 0 keeps an entry forever.
 *
 * Implementations must be safe for concurrent use. Access may evict expired
 * entries lazily, so methods are non-const.
 */
class VN_RUNTIME_API CacheManager {
  public:
    /**
     * @brief Destroys the cache abstraction.
     */
    virtual ~CacheManager() = default;

    /**
     * @brief Stores a value under a key.
     *
     * @param key Cache key.
     * @param value Value to store; copied into the cache.
     * @param timeout_seconds Lifetime in seconds; 0 keeps the entry forever.
     */
    virtual void set(const String& key, std::any value, size_t timeout_seconds) = 0;

    /**
     * @brief Retrieves a non-expired value.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return The stored value, or an empty std::any when absent or expired.
     */
    virtual std::any get(const String& key) = 0;

    /**
     * @brief Returns whether a non-expired entry exists.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return true when present and not expired.
     */
    virtual bool contains(const String& key) = 0;

    /**
     * @brief Removes one entry.
     *
     * @param key Cache key.
     * @return true when an entry was removed.
     */
    virtual bool remove(const String& key) = 0;

    /**
     * @brief Removes all expired entries.
     *
     * @return The number of removed entries.
     */
    virtual size_t removeExpired() = 0;

    /**
     * @brief Removes all entries.
     */
    virtual void clear() = 0;

    /**
     * @brief Returns the number of live (non-expired) entries.
     *
     * Expired entries are removed on access (lazy eviction), so the result
     * reflects only entries that are still alive.
     *
     * @return The number of live entries.
     */
    virtual size_t count() = 0;

    /**
     * @brief Returns whether the cache holds no live entry.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @return true when there is no live entry.
     */
    virtual bool isEmpty() = 0;

    /**
     * @brief Returns the keys of all live entries.
     *
     * Expired entries are removed on access (lazy eviction); the order of the
     * returned keys is unspecified.
     *
     * @return The live keys.
     */
    virtual std::vector<String> keys() = 0;
};

VN_RUNTIME_NS_END
