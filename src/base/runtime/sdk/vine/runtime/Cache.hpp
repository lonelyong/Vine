#pragma once
#include "runtime_global.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

VN_RUNTIME_NS_BEGIN

/**
 * @brief How a cache operation ended.
 *
 * @details
 * - `Ok` means the operation ran; a lookup's hit is `Ok`, and an answer that is
 *   `false` (absent, not removed) is still `Ok`.
 * - `Miss` means the requested key has no live entry. It is an answer, not an
 *   error, and only operations that cannot answer without an entry use it.
 * - `Failure` means the backing store could not complete the operation; `error`
 *   says why and the value is empty.
 */
enum class CacheStatus : std::uint8_t {
    Ok,
    Miss,
    Failure,
};

/**
 * @brief Why a cache operation failed.
 */
enum class CacheError : std::uint8_t {
    /// No error.
    None,
    /// The backing store could not be read or written.
    Io,
    /// The stored representation could not be decoded into the value type.
    Corrupt,
    /// The implementation does not provide this operation.
    Unsupported,
};

/**
 * @brief The result of an operation that has no value to return.
 *
 * @details
 * - `status` is `Ok` or `Failure`; `error` is `None` on success.
 * - Every operation returns a result and every result should be observed: a
 *   cache that fails silently is indistinguishable from one that misses.
 */
struct CacheResult {
    CacheStatus status{ CacheStatus::Ok };
    CacheError  error{ CacheError::None };

    /**
     * @brief Returns whether the operation completed.
     *
     * @return true when `status` is `Ok`.
     */
    bool ok() const noexcept;

    /**
     * @brief Returns whether the operation failed.
     *
     * @return true when `status` is `Failure`.
     */
    bool failed() const noexcept;
};

/**
 * @brief The result of an operation that answers with a value.
 *
 * @details
 * - `Ok` carries a value; `Miss` is an answer with no value; `Failure` carries
 *   the reason and no value.
 *
 * @tparam TValue The answer type.
 */
template <class TValue>
struct CacheOutcome {
    CacheStatus           status{ CacheStatus::Miss };
    CacheError            error{ CacheError::None };
    std::optional<TValue> value;

    /**
     * @brief Returns whether an answer with a value is present.
     *
     * @return true when `status` is `Ok` and a value was returned.
     */
    bool ok() const noexcept;

    /**
     * @brief Returns whether the key had no live entry.
     *
     * @return true when `status` is `Miss`.
     */
    bool missed() const noexcept;

    /**
     * @brief Returns whether the backing store failed.
     *
     * @return true when `status` is `Failure`.
     */
    bool failed() const noexcept;
};

/**
 * @brief The cache seam: typed keys and values, with an explicit failure channel.
 *
 * @details
 * - One interface for every backing store: InMemoryCache is the in-memory
 *   implementation; a persistent implementation serialises where it stores.
 *   Serialisation is an implementation detail, never part of this contract.
 * - The lifetime is an optional duration: an empty optional (or a non-positive
 *   one) keeps the entry until it is removed or evicted.
 * - Implementations must be safe for concurrent use. Reads may evict expired
 *   entries lazily, so the methods are non-const.
 * - The interface is deliberately typed: an erased (String / std::any) cache
 *   would force a cast and a copy at every call site and would hide the value
 *   type from the store that has to serialise it.
 *
 * @tparam TKey Key type; must be hashable or ordered as the implementation
 *              requires (the in-memory one needs `operator<`).
 * @tparam TValue Value type; must be copy-constructible.
 */
template <class TKey, class TValue>
class Cache {
  public:
    /**
     * @brief Destroys the cache instance.
     */
    virtual ~Cache() = default;

    /**
     * @brief Returns the live value for a key.
     *
     * @param key Cache key.
     * @return `Ok` with the value, `Miss` when absent or expired, or `Failure`
     *         with the reason.
     */
    virtual CacheOutcome<TValue> get(const TKey& key) = 0;

    /**
     * @brief Stores or replaces a value under a key.
     *
     * @param key Cache key.
     * @param value Value to store; copied into the cache.
     * @param ttl Lifetime; an empty optional or a non-positive duration keeps
     *            the entry until it is removed or evicted.
     * @return `Ok`, or `Failure` with the reason.
     */
    virtual CacheResult set(const TKey& key, const TValue& value, std::optional<std::chrono::seconds> ttl) = 0;

    /**
     * @brief Returns whether a live entry exists.
     *
     * @param key Cache key.
     * @return `Ok` with the answer (`false` when absent), or `Failure`.
     */
    virtual CacheOutcome<bool> contains(const TKey& key) = 0;

    /**
     * @brief Removes one entry.
     *
     * @param key Cache key.
     * @return `Ok` with true when an entry was removed, or `Failure`.
     */
    virtual CacheOutcome<bool> remove(const TKey& key) = 0;

    /**
     * @brief Removes every entry.
     *
     * @return `Ok`, or `Failure` with the reason.
     */
    virtual CacheResult clear() = 0;

    /**
     * @brief Returns the number of live (non-expired) entries.
     *
     * @return `Ok` with the count, or `Failure`.
     */
    virtual CacheOutcome<std::size_t> count() = 0;

    /**
     * @brief Returns the keys of all live entries.
     *
     * @return `Ok` with the keys (order unspecified), or `Failure`.
     */
    virtual CacheOutcome<std::vector<TKey>> keys() = 0;
};

template <class TValue>
bool CacheOutcome<TValue>::ok() const noexcept
{
    return status == CacheStatus::Ok && value.has_value();
}

template <class TValue>
bool CacheOutcome<TValue>::missed() const noexcept
{
    return status == CacheStatus::Miss;
}

template <class TValue>
bool CacheOutcome<TValue>::failed() const noexcept
{
    return status == CacheStatus::Failure;
}

inline bool CacheResult::ok() const noexcept
{
    return status == CacheStatus::Ok;
}

inline bool CacheResult::failed() const noexcept
{
    return status == CacheStatus::Failure;
}

VN_RUNTIME_NS_END
