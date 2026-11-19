#pragma once
#include "runtime_global.hpp"

#include <algorithm>
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

#include "Cache.hpp"

VN_RUNTIME_NS_BEGIN

/**
 * @brief The in-memory implementation of Cache: values stored by value, no serialisation.
 *
 * A header-only template that returns values by value, so a hit copies the value
 * and never encodes it; the persistent implementations of Cache serialise where
 * they store. Expired entries are evicted lazily on access, and removeExpired()
 * forces a sweep.
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
 * @tparam TKey Key type; must support operator<.
 * @tparam TVal Value type; must be copy-constructible.
 */
template <typename TKey, typename TVal>
class InMemoryCache final : public Cache<TKey, TVal> {
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
     * @brief Stores or replaces a value under a key.
     *
     * @details
     * - An empty or non-positive lifetime keeps the entry until it is removed or
     *   evicted.
     * - An overwrite is erase-then-insert: a copy that throws leaves no entry
     *   rather than the previous value.
     *
     * @param key Cache key.
     * @param value Value to store; copied into the cache.
     * @param ttl Lifetime; an empty optional keeps the entry forever.
     * @return `Ok`; this implementation cannot fail (an allocation failure throws).
     */
    CacheResult set(const TKey& key, const TVal& value, std::optional<std::chrono::seconds> ttl) override;

    /**
     * @brief Returns the live value for a key.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return `Ok` with the value, or `Miss` when absent or expired.
     */
    CacheOutcome<TVal> get(const TKey& key) override;

    /**
     * @brief Returns whether a live entry exists.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @param key Cache key.
     * @return `Ok` with the answer (`false` when absent or expired).
     */
    CacheOutcome<bool> contains(const TKey& key) override;

    /**
     * @brief Removes one entry.
     *
     * @param key Cache key.
     * @return `Ok` with true when an entry was removed.
     */
    CacheOutcome<bool> remove(const TKey& key) override;

    /**
     * @brief Removes all expired entries.
     *
     * Not part of Cache: a persistent store enforces expiry on its own and has
     * no reason to promise a sweep.
     *
     * @return The number of removed entries.
     */
    std::size_t removeExpired();

    /**
     * @brief Removes all entries.
     *
     * @return `Ok`.
     */
    CacheResult clear() override;

    /**
     * @brief Returns the number of live (non-expired) entries.
     *
     * Expired entries are removed on access (lazy eviction).
     *
     * @return `Ok` with the number of live entries.
     */
    CacheOutcome<std::size_t> count() override;

    /**
     * @brief Returns the keys of all live entries.
     *
     * @return `Ok` with the live keys; order is unspecified.
     */
    CacheOutcome<std::vector<TKey>> keys() override;

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
     * @param ttl Lifetime; empty or non-positive means keep forever.
     * @return The expiry time, or a default (invalid) point for forever.
     */
    static Clock::time_point expiryFor(std::optional<std::chrono::seconds> ttl);

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
CacheResult InMemoryCache<TKey, TVal>::set(const TKey& key, const TVal& value, std::optional<std::chrono::seconds> ttl)
{
    Entry                       entry{ value, !ttl.has_value() || *ttl <= std::chrono::seconds::zero(), expiryFor(ttl) };
    std::lock_guard<std::mutex> lock(mutex_);
    // erase + emplace, not insert_or_assign: replacing an entry must not require
    // TVal to be assignable, only copy-constructible.
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        entries_.erase(it);
    }
    entries_.emplace(key, std::move_if_noexcept(entry));
    return {};
}

template <typename TKey, typename TVal>
CacheOutcome<TVal> InMemoryCache<TKey, TVal>::get(const TKey& key)
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = entries_.find(key);
    if (it == entries_.end()) {
        return { CacheStatus::Miss, CacheError::None, std::nullopt };
    }
    if (isExpired(it->second, now)) {
        entries_.erase(it);
        return { CacheStatus::Miss, CacheError::None, std::nullopt };
    }
    return { CacheStatus::Ok, CacheError::None, std::optional<TVal>(it->second.value) };
}

template <typename TKey, typename TVal>
CacheOutcome<bool> InMemoryCache<TKey, TVal>::contains(const TKey& key)
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = entries_.find(key);
    if (it == entries_.end()) {
        return { CacheStatus::Ok, CacheError::None, false };
    }
    if (isExpired(it->second, now)) {
        entries_.erase(it);
        return { CacheStatus::Ok, CacheError::None, false };
    }
    return { CacheStatus::Ok, CacheError::None, true };
}

template <typename TKey, typename TVal>
CacheOutcome<bool> InMemoryCache<TKey, TVal>::remove(const TKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return { CacheStatus::Ok, CacheError::None, entries_.erase(key) > 0 };
}

template <typename TKey, typename TVal>
std::size_t InMemoryCache<TKey, TVal>::removeExpired()
{
    const auto                  now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t                 removed = 0;
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
CacheResult InMemoryCache<TKey, TVal>::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    return {};
}

template <typename TKey, typename TVal>
CacheOutcome<std::size_t> InMemoryCache<TKey, TVal>::count()
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
    return { CacheStatus::Ok, CacheError::None, entries_.size() };
}

template <typename TKey, typename TVal>
CacheOutcome<std::vector<TKey>> InMemoryCache<TKey, TVal>::keys()
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
    return { CacheStatus::Ok, CacheError::None, std::move(result) };
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
typename InMemoryCache<TKey, TVal>::Clock::time_point InMemoryCache<TKey, TVal>::expiryFor(std::optional<std::chrono::seconds> ttl)
{
    if (!ttl.has_value() || *ttl <= std::chrono::seconds::zero()) {
        return {};
    }
    // Bound the addition so a huge lifetime cannot overflow the time point; a
    // hundred years is "forever" for a cache.
    constexpr auto kMaxLifetime = std::chrono::seconds(24LL * 60 * 60 * 365 * 100);
    return Clock::now() + std::min<std::chrono::seconds>(*ttl, kMaxLifetime);
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
