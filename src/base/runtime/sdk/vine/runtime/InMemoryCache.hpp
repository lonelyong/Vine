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
#include <utility>
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
 * instantiation share one worker. Call start() to register this instance for
 * periodic sweeps; stop() (or destruction) unregisters it, and the worker
 * exits once the registry has stayed empty. No thread exists until the first
 * start() call.
 *
 * The shared state is allocated once and never destroyed, and the worker is
 * detached: a joinable std::thread cannot be a static object (its destructor
 * runs before the caches that registered with it and calls std::terminate),
 * and there is no join that could block a destructor while another instance
 * starts. A cache with static storage duration is therefore safe to start.
 *
 * The class is intentionally standalone: it does not derive from the
 * CacheManager interface. Use InMemoryManager to expose a typed cache through
 * that String/any interface (e.g. for vn::di injection).
 *
 * @tparam TKey Key type; must support operator<.
 * @tparam TVal Value type; must be copy-constructible. The out-parameter
 *              get() overload additionally requires it to be assignable.
 */
template <typename TKey, typename TVal>
class InMemoryCache {
  public:
    /**
     * @brief Monotonic clock used for expiry timestamps.
     */
    using Clock = std::chrono::steady_clock;

    /// Sweep period used by start() when the caller does not choose one.
    static constexpr std::chrono::milliseconds kDefaultSweepPeriod{ std::chrono::seconds(1) };

    /// Smallest accepted sweep period; a shorter (or zero) one would spin the shared worker.
    static constexpr std::chrono::milliseconds kMinSweepPeriod{ 1 };

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
     * @details
     * - A lifetime of 0 or a negative value keeps the entry forever.
     * - An overwrite is erase-then-insert: a copy that throws leaves no entry
     *   rather than the previous value.
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
     * All instances of the same <TKey, TVal> instantiation share one worker.
     * If the worker is already running, only the sweep period is updated (last
     * caller wins).
     *
     * @param period Sweep period; values below kMinSweepPeriod are raised to it,
     *               so a zero period cannot spin the worker.
     */
    void start(std::chrono::milliseconds period = kDefaultSweepPeriod);

    /**
     * @brief Unregisters this instance from background reclamation.
     *
     * The shared worker exits once the registry has stayed empty; a no-op when
     * this instance was never started.
     */
    void stop();

    /**
     * @brief Returns whether this instance participates in reclamation.
     *
     * @return true when started and not yet stopped.
     */
    bool isCleanupRunning() const;

  private:
    struct Entry {
        TVal value;
        /// True when the entry never expires; named this way because Qt's qforeach.h defines `forever` as a macro.
        bool              never_expires{ false };
        Clock::time_point expires_at{};
    };

    /**
     * @brief Shared reclaimer state for one <TKey, TVal> instantiation.
     *
     * Allocated once and never destroyed. It owns the registry and the wake-up
     * machinery the worker uses; destroying it at exit would leave the worker
     * using freed state, and keeping the worker in a static std::thread instead
     * would call std::terminate before the caches that registered with it are
     * torn down. The OS reclaims the block with the process.
     */
    struct Sweeper {
        std::mutex                         mutex;
        std::condition_variable            cv;
        std::unordered_set<InMemoryCache*> instances;
        std::chrono::milliseconds          period{ kDefaultSweepPeriod };
        bool                               worker_alive{ false };
        bool                               stop_requested{ false };
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
     * @brief Unregisters this instance from background reclamation.
     *
     * Never joins or creates a thread, so it cannot block on or throw from a
     * destructor.
     */
    void shutdown();

    /**
     * @brief Returns the process-lifetime sweeper of this instantiation.
     *
     * @return The shared sweeper state; owned by the process, never destroyed.
     */
    static Sweeper& sweeper();

    /**
     * @brief Body of the shared worker: sweeps the registered instances, sleeping
     *        while none is registered.
     */
    static void sweepLoop();

    mutable std::mutex    mutex_;
    std::map<TKey, Entry> entries_;
};

template <typename TKey, typename TVal>
InMemoryCache<TKey, TVal>::~InMemoryCache()
{
    shutdown();
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::set(const TKey& key, const TVal& value, int ttl)
{
    Entry                       entry{ value, (ttl <= 0), makeExpireAt(ttl) };
    std::lock_guard<std::mutex> lock(mutex_);
    // erase + emplace, not insert_or_assign: replacing an entry must not require
    // TVal to be assignable, only copy-constructible.
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        entries_.erase(it);
    }
    entries_.emplace(key, std::move_if_noexcept(entry));
}

template <typename TKey, typename TVal>
std::optional<TVal> InMemoryCache<TKey, TVal>::get(const TKey& key)
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    if (isExpired(it->second, now)) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::get(const TKey& key, TVal& out)
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (isExpired(it->second, now)) {
        entries_.erase(it);
        return false;
    }
    out = it->second.value;
    return true;
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::contains(const TKey& key)
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (isExpired(it->second, now)) {
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
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    size_t                      removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (isExpired(it->second, now)) {
            it = entries_.erase(it);
            ++removed;
        }
        else {
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
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (isExpired(it->second, now)) {
            it = entries_.erase(it);
        }
        else {
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
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TKey>           result;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (isExpired(it->second, now)) {
            it = entries_.erase(it);
        }
        else {
            result.push_back(it->first);
            ++it;
        }
    }
    return result;
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::start(std::chrono::milliseconds period)
{
    Sweeper&                    sweeper_state = sweeper();
    std::lock_guard<std::mutex> lock(sweeper_state.mutex);
    sweeper_state.period         = period < kMinSweepPeriod ? kMinSweepPeriod : period;
    sweeper_state.stop_requested = false;
    sweeper_state.instances.insert(this);
    if (!sweeper_state.worker_alive) {
        // Detached: there is no thread object to join or destroy, so neither the
        // process exit order nor a concurrent stop() can block or throw here. The
        // flag is set only after a worker exists, so a failed creation is retried
        // by the next start() instead of leaving a registry without a worker.
        std::thread worker(&InMemoryCache<TKey, TVal>::sweepLoop);
        worker.detach();
        sweeper_state.worker_alive = true;
    }
    sweeper_state.cv.notify_all();
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::stop()
{
    shutdown();
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::isCleanupRunning() const
{
    Sweeper&                    sweeper_state = sweeper();
    std::lock_guard<std::mutex> lock(sweeper_state.mutex);
    return sweeper_state.instances.find(const_cast<InMemoryCache*>(this)) != sweeper_state.instances.end();
}

template <typename TKey, typename TVal>
typename InMemoryCache<TKey, TVal>::Clock::time_point InMemoryCache<TKey, TVal>::makeExpireAt(int ttl)
{
    if (ttl > 0) {
        return Clock::now() + std::chrono::seconds(ttl);
    }
    return {};
}

template <typename TKey, typename TVal>
bool InMemoryCache<TKey, TVal>::isExpired(const Entry& entry, const Clock::time_point& now) const
{
    return !entry.never_expires && now >= entry.expires_at;
}

template <typename TKey, typename TVal>
typename InMemoryCache<TKey, TVal>::Sweeper& InMemoryCache<TKey, TVal>::sweeper()
{
    // Immortal by design (see Sweeper): a local static unique_ptr would destroy
    // the state - and with it the worker's mutex and condition variable - at
    // exit, while the worker may still be using them.
    static Sweeper* const state = new Sweeper;
    return *state;
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::shutdown()
{
    Sweeper&                    sweeper_state = sweeper();
    std::lock_guard<std::mutex> lock(sweeper_state.mutex);
    sweeper_state.instances.erase(this);
    if (sweeper_state.instances.empty() && sweeper_state.worker_alive) {
        // No join: the worker wakes, sees the empty registry and exits. A start()
        // that re-registers first clears this request, so the worker keeps
        // serving instead of exiting under the new instance.
        sweeper_state.stop_requested = true;
        sweeper_state.cv.notify_all();
    }
}

template <typename TKey, typename TVal>
void InMemoryCache<TKey, TVal>::sweepLoop()
{
    Sweeper&                     sweeper_state = sweeper();
    std::unique_lock<std::mutex> lock(sweeper_state.mutex);
    for (;;) {
        if (sweeper_state.instances.empty()) {
            // Sleep until an instance registers or a stop request is pending.
            sweeper_state.cv.wait(lock, [&sweeper_state] { return sweeper_state.stop_requested || !sweeper_state.instances.empty(); });
        }
        else {
            sweeper_state.cv.wait_for(lock, sweeper_state.period, [&sweeper_state] { return sweeper_state.stop_requested || sweeper_state.instances.empty(); });
        }

        if (sweeper_state.stop_requested) {
            if (!sweeper_state.instances.empty()) {
                sweeper_state.stop_requested = false; // re-registered while stopping
                continue;
            }
            sweeper_state.worker_alive = false;
            return;
        }

        // Swept under the registry lock: an instance unregisters (and is then
        // destroyed) only through that same lock, so no entry can disappear
        // under the loop. A TVal destructor that calls back into a cache of this
        // instantiation would deadlock against this lock.
        for (auto* instance : sweeper_state.instances) {
            instance->removeExpired();
        }
    }
}

VN_RUNTIME_NS_END
