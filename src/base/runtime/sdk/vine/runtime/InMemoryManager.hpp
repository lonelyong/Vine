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
 * @brief Bridge exposing a typed in-memory cache through the CacheManager
 *        interface.
 *
 * The type-safe InMemoryCache<TKey, TVal> template is intentionally
 * standalone and does not derive from CacheManager. InMemoryManager is the
 * non-template adapter that implements the String/any interface by delegating
 * to InMemoryCache<String, std::any>, so a typed cache can be
 * registered with vn::di (or swapped) as a single service while the call
 * sites keep std::any_cast.
 *
 * It keeps the same thread-safe TTL semantics as the template, including the
 * background expiry sweeper (see the sweep_interval constructor argument).
 */
class VN_RUNTIME_API InMemoryManager final : public CacheManager {
  public:
    /**
     * @brief Constructs an empty cache and starts its background sweeper.
     *
     * @param sweep_interval Interval between automatic expiry sweeps.
     */
    explicit InMemoryManager(std::chrono::milliseconds sweep_interval = std::chrono::seconds(1));

    /**
     * @brief Stops the background sweeper and destroys all stored values.
     */
    ~InMemoryManager() override;

    InMemoryManager(const InMemoryManager&)            = delete;
    InMemoryManager& operator=(const InMemoryManager&) = delete;

  public:
    void set(const String& key, std::any value, size_t timeout_seconds) override;
    std::any get(const String& key) override;
    bool contains(const String& key) override;
    bool remove(const String& key) override;
    size_t removeExpired() override;
    void clear() override;
    size_t count() override;
    bool isEmpty() override;
    std::vector<String> keys() override;

  private:
    InMemoryCache<String, std::any> cache_;
};

VN_RUNTIME_NS_END
