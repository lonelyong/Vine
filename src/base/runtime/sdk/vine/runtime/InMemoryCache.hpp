#pragma once
#include "runtime_global.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

VN_RUNTIME_NS_BEGIN

/**
 * @brief Type-safe, thread-safe, TTL-aware in-memory cache.
 *
 * A header-only template that stores values by value and returns them as
 * std::optional<TVal>, so there is no type erasure or any_cast at the call
 * site. get()/contains() evict expired entries lazily on access, and
 * removeExpired() forces a sweep.
 *
 * Background reclamation: all instances of the same <TKey, TVal>
 * instantiation share a single background thread. Call start() to register
 * this instance for periodic sweeps; stop() (or destruction) unregisters it,
 * and the shared thread ends when the last registered instance stops. No
 * thread is started until the first start() call.
 *
 * The class is intentionally standalone: it does not derive from the
 * CacheManager interface. Use InMemoryManager to expose a typed cache through
 * that String/any interface (e.g. for vn::di injection).
 *
 * @tparam TKey Key type; must support operator<.
 * @tparam TVal Value type; must be copy-constructible.
 */
template <typename TKey, typename TVal>
class InMemoryCache
{
  public:
    /**
     * @brief Monotonic clock used for expiry timestamps.
     */
    using Clock = std::chrono::steady_clock;

    /**
     * @brief Constructs an empty cache.
     *
     * Background reclamation is off until start() is called.
     */
    InMemoryCache() = default;

    /**
     * @brief Unregisters this instance from background reclamation.
     */
    ~InMemoryCache();

    InMemoryCache(const InMemoryCache&)            = delete;
    InMemoryCache& operator=(const InMemoryCache&) = delete;

  public:
    /**
     * @brief Stores or updates a value under a key.
     *
     * @param key Cache key.
     * @param value Value to store; copied into the cache.
     * @param ttl Lifetime in seconds; 0 or negative keeps the entry forever.
     */
    void set(const TKey& key, const TVal& value, int ttl = -1);

    /**
     * @brief Retrieves a non-expired value.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return The stored value, or std::nullopt when absent or expired.
     */
    std::optional<TVal> get(const TKey& key);

    /**
     * @brief Retrieves a non-expired value into an output parameter.
     *
     * Avoids the copy of the optional-returning overload for callers that
     * already hold storage. Expired entries are removed on access (lazy
     * eviction).
     *
     * @param key Cache key.
     * @param out Receives the stored value on hit.
     * @return true when present and not expired; false otherwise.
     */
    bool get(const TKey& key, TVal& out);

    /**
     * @brief Returns whether a non-expired entry exists.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return true when present and not expired.
     */
    bool contains(const TKey& key);

    /**
     * @brief Removes one entry.
     *
     * @param key Cache key.
     * @return true when an entry was removed.
     */
    bool remove(const TKey& key);

    /**
     * @brief Removes all expired entries.
     *
     * @return The number of removed entries.
     */
    size_t removeExpired();

    /**
     * @brief Removes all entries.
     */
    void clear();

    /**
     * @brief Returns the number of live (non-expired) entries.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @return The number of live entries.
     */
    size_t count();

    /**
     * @brief Returns whether the cache holds no live entry.
     *
     * @return true when there is no live entry.
     */
    bool isEmpty();

    /**
     * @brief Returns the keys of all live entries.
     *
     * @return The live keys; order is unspecified.
     */
    std::vector<TKey> keys();

  public:
    /**
     * @brief Registers this instance for background reclamation.
     *
     * All instances of the same <TKey, TVal> instantiation share one
     * background thread. If the thread is already running, only the sweep
     * period is updated (last caller wins).
     *
     * @param period Sweep period.
     */
    void start(std::chrono::milliseconds period = std::chrono::seconds(1));

    /**
     * @brief Unregisters this instance from background reclamation.
     *
     * The shared thread ends when the last registered instance stops; a no-op
     * when this instance was never started.
     */
    void stop();

    /**
     * @brief Returns whether this instance participates in reclamation.
     *
     * @return true when started and not yet stopped.
     */
    bool isCleanupRunning() const;

  private:
    struct Entry
    {
        TVal                    value;
        bool                    forever{ false };
        Clock::time_point       expires_at{};
    };

    /**
     * @brief Computes the expiry time point for a lifetime.
     *
     * @param ttl Lifetime in seconds; 0 or negative means keep forever.
     * @return The expiry time, or a default (invalid) point for forever.
     */
    static Clock::time_point makeExpireAt(int ttl);

    /**
     * @brief Returns whether the entry is past its expiry.
     *
     * @param entry The entry to inspect.
     * @param now Current monotonic time.
     * @return true when the entry has a finite lifetime and expired.
     */
    bool isExpired(const Entry& entry, const Clock::time_point& now) const;

    /**
     * @brief Unregisters this instance; ends the shared thread when it is the
     *        last one.
     */
    void shutdown();

    /**
     * @brief Shared background reclaimer body; sweeps all registered
     *        instances periodically.
     */
    static void cleanupLoop();

    mutable std::mutex mutex_;
    std::map<TKey, Entry> entries_;

    // ---- Shared background-thread state (one per <TKey, TVal> instantiation) ----
    inline static std::mutex s_mutex_;
    inline static std::condition_variable s_cv_;
    inline static std::thread s_thread_;
    inline static std::unordered_set<InMemoryCache*> s_instances_;
    inline static std::chrono::milliseconds s_period_{ std::chrono::seconds(1) };
    inline static bool s_stopping_{ false };
};

template <typename TKey, typename TVal>
InMemoryCache<TKey, TVal>::~InMemoryCache()
{
    shutdown();
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::set(const TKey& key, const TVal& value, int ttl)
{
    Entry entry{ value, (ttl <= 0), makeExpireAt(ttl) };
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.insert_or_assign(key, entry);
}

template <typename TKey, typename TVal>
std::optional<TVal> InMemoryCache<TKey, TVal>::get(const TKey& key)
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return std::nullopt;
    }
    if (isExpired(it->second, now))
    {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::get(const TKey& key, TVal& out)
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }
    if (isExpired(it->second, now))
    {
        entries_.erase(it);
        return false;
    }
    out = it->second.value;
    return true;
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::contains(const TKey& key)
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return false;
    }
    if (isExpired(it->second, now))
    {
        entries_.erase(it);
        return false;
    }
    return true;
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::remove(const TKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(key) > 0;
}

template <typename TKey, typename TVal>
size_t InMemoryCache<TKey, TVal>::removeExpired()
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (isExpired(it->second, now))
        {
            it = entries_.erase(it);
            ++removed;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

template <typename TKey, typename TVal>
size_t InMemoryCache<TKey, TVal>::count()
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (isExpired(it->second, now))
        {
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return entries_.size();
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::isEmpty()
{
    return count() == 0;
}

template <typename TKey, typename TVal>
std::vector<TKey> InMemoryCache<TKey, TVal>::keys()
{
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TKey> result;
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (isExpired(it->second, now))
        {
            it = entries_.erase(it);
        }
        else
        {
            result.push_back(it->first);
            ++it;
        }
    }
    return result;
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::start(std::chrono::milliseconds period)
{
    std::lock_guard<std::mutex> lock(s_mutex_);
    s_period_ = period;
    s_instances_.insert(this);
    if (!s_thread_.joinable())
    {
        s_stopping_ = false;
        s_thread_ = std::thread(&InMemoryCache<TKey, TVal>::cleanupLoop);
    }
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::stop()
{
    shutdown();
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::isCleanupRunning() const
{
    std::lock_guard<std::mutex> lock(s_mutex_);
    return s_instances_.find(const_cast<InMemoryCache*>(this)) != s_instances_.end();
}

template <typename TKey, typename TVal>
typename InMemoryCache<TKey, TVal>::Clock::time_point
InMemoryCache<TKey, TVal>::makeExpireAt(int ttl)
{
    if (ttl > 0)
    {
        return Clock::now() + std::chrono::seconds(ttl);
    }
    return {};
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::isExpired(const Entry& entry, const Clock::time_point& now) const
{
    return !entry.forever && now >= entry.expires_at;
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::shutdown()
{
    std::thread to_join;
    bool is_last = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex_);
        s_instances_.erase(this);
        if (s_instances_.empty() && s_thread_.joinable())
        {
            s_stopping_ = true;
            // Move the thread object out so a concurrent start() can spawn a
            // fresh thread without racing this join.
            to_join = std::move(s_thread_);
            is_last = true;
        }
    }

    if (!is_last)
    {
        return;
    }

    // Join outside the lock: the loop needs the lock to re-check stopping.
    s_cv_.notify_all();
    to_join.join();

    // If new instances registered while joining, start a fresh thread.
    std::lock_guard<std::mutex> lock(s_mutex_);
    if (!s_instances_.empty() && !s_thread_.joinable())
    {
        s_stopping_ = false;
        s_thread_ = std::thread(&InMemoryCache<TKey, TVal>::cleanupLoop);
    }
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::cleanupLoop()
{
    std::unique_lock<std::mutex> lock(s_mutex_);
    while (true)
    {
        s_cv_.wait_for(lock, s_period_, [] { return s_stopping_; });
        if (s_stopping_)
        {
            return;
        }
        for (auto* instance : s_instances_)
        {
            instance->removeExpired();
        }
    }
}

VN_RUNTIME_NS_END
