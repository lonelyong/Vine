#pragma once
#include "runtime_global.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vine/String.hpp>

#include "Cache.hpp"
#include "KeyValueStore.hpp"

VN_RUNTIME_NS_BEGIN

/**
 * @brief A value type that can cross a byte store.
 *
 * @details
 * - `encode` produces the bytes for a value; it may throw only for an allocation
 *   failure (a value that cannot be represented is a compile-time concern).
 * - `decode` returns an empty optional for bytes that are not a `TValue`; the
 *   cache reports that as `CacheError::Corrupt`.
 *
 * @tparam TSerializer The serializer type.
 * @tparam TValue The value type.
 */
template <class TSerializer, class TValue>
concept ValueSerializer = requires(const TSerializer& serializer, const TValue& value, std::span<const std::byte> bytes) {
    { serializer.encode(value) } -> std::same_as<std::vector<std::byte>>;
    { serializer.decode(bytes) } -> std::same_as<std::optional<TValue>>;
};

/**
 * @brief A `Cache<String, TValue>` over a byte store: every value is encoded and decoded.
 *
 * @details
 * - The serializer is injected here, never in the `Cache` interface: an in-memory
 *   cache has no byte representation and must not pay for one.
 * - `set()` encodes the value and hands the bytes to the store together with an
 *   absolute expiry; `get()` asks the store for bytes and decodes them. An absent
 *   record is `Miss`; undecodable bytes are `Failure` with
 *   `CacheError::Corrupt`; a store error is passed through unchanged.
 * - `contains()` asks the store without decoding the value.
 * - `count()` and `keys()` are supported only when the store can enumerate.
 * - The store is borrowed and must outlive the cache. Both are expected to be
 *   safe for concurrent use, as `Cache` requires.
 *
 * @tparam TValue Value type; must be copy-constructible.
 * @tparam TSerializer Encoder/decoder satisfying ValueSerializer; stored by value.
 */
template <class TValue, class TSerializer>
    requires ValueSerializer<TSerializer, TValue>
class SerializedCache final : public Cache<String, TValue> {
  public:
    /**
     * @brief Wraps a byte store with a serializer.
     *
     * @param store Byte store the serialized values live in; borrowed and must
     *              outlive this cache.
     * @param serializer Encoder/decoder for TValue; stored by value.
     */
    explicit SerializedCache(KeyValueStore& store, TSerializer serializer = {})
      : store_(store)
      , serializer_(std::move(serializer))
    {}

    /**
     * @brief Returns the decoded value for a key.
     *
     * @param key UTF-8 key.
     * @return `Ok` with the value, `Miss` when absent or expired, or `Failure`
     *         with the store's error or `CacheError::Corrupt`.
     */
    CacheOutcome<TValue> get(const String& key) override
    {
        auto stored = store_.load(key);
        if (stored.ok()) {
            auto value = serializer_.decode(*stored.value);
            if (!value.has_value()) {
                return { CacheStatus::Failure, CacheError::Corrupt, std::nullopt };
            }
            return { CacheStatus::Ok, CacheError::None, std::move_if_noexcept(value) };
        }
        if (stored.missed()) {
            return { CacheStatus::Miss, CacheError::None, std::nullopt };
        }
        return { CacheStatus::Failure, stored.error, std::nullopt };
    }

    /**
     * @brief Encodes a value and stores it under a key.
     *
     * @param key UTF-8 key.
     * @param value Value to encode and store.
     * @param ttl Lifetime; empty or non-positive means no expiry.
     * @return `Ok`, or `Failure` with the store's reason.
     */
    CacheResult set(const String& key, const TValue& value, std::optional<std::chrono::seconds> ttl) override
    {
        return store_.store(key, serializer_.encode(value), expiryOf(ttl));
    }

    /**
     * @brief Returns whether the store holds a live record for a key.
     *
     * @param key UTF-8 key.
     * @return `Ok` with the answer, or `Failure` with the store's reason.
     */
    CacheOutcome<bool> contains(const String& key) override
    {
        auto stored = store_.load(key);
        if (stored.ok()) {
            return { CacheStatus::Ok, CacheError::None, true };
        }
        if (stored.missed()) {
            return { CacheStatus::Ok, CacheError::None, false };
        }
        return { CacheStatus::Failure, stored.error, std::nullopt };
    }

    /**
     * @brief Removes one key.
     *
     * @param key UTF-8 key.
     * @return `Ok` with true when a record was removed, or `Failure`.
     */
    CacheOutcome<bool> remove(const String& key) override
    {
        return store_.erase(key);
    }

    /**
     * @brief Removes every record.
     *
     * @return `Ok`, or `Failure` with the store's reason.
     */
    CacheResult clear() override
    {
        return store_.clear();
    }

    /**
     * @brief Returns the number of live records.
     *
     * @return `Ok` with the count, or `Failure` (`CacheError::Unsupported` when
     *         the store cannot enumerate).
     */
    CacheOutcome<std::size_t> count() override
    {
        return store_.count();
    }

    /**
     * @brief Returns the live keys.
     *
     * @return `Ok` with the keys, or `Failure` (`CacheError::Unsupported` when
     *         the store cannot enumerate).
     */
    CacheOutcome<std::vector<String>> keys() override
    {
        return store_.keys();
    }

  private:
    /**
     * @brief Translates a cache lifetime into the store's absolute expiry.
     *
     * @param ttl Lifetime from set(); empty or non-positive means no expiry.
     * @return The absolute expiry, or an empty optional for "no expiry".
     */
    static std::optional<std::chrono::system_clock::time_point> expiryOf(std::optional<std::chrono::seconds> ttl)
    {
        if (!ttl.has_value() || *ttl <= std::chrono::seconds::zero()) {
            return std::nullopt;
        }
        // Bound the addition so a huge lifetime cannot overflow the time point; a
        // hundred years is "forever" for a cache.
        constexpr auto kMaxLifetime = std::chrono::seconds(24LL * 60 * 60 * 365 * 100);
        return std::chrono::system_clock::now() + std::min<std::chrono::seconds>(*ttl, kMaxLifetime);
    }

    KeyValueStore& store_;
    TSerializer    serializer_;
};

VN_RUNTIME_NS_END
