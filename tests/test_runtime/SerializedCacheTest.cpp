#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <vine/String.hpp>
#include <vine/runtime/Cache.hpp>
#include <vine/runtime/KeyValueStore.hpp>
#include <vine/runtime/SerializedCache.hpp>

using vn::String;
using vn::runtime::Cache;
using vn::runtime::CacheError;
using vn::runtime::CacheOutcome;
using vn::runtime::CacheResult;
using vn::runtime::CacheStatus;
using vn::runtime::KeyValueStore;
using vn::runtime::SerializedCache;

namespace
{

/// Encodes an int as its native bytes; wrong-length bytes are not an int.
struct IntSerializer {
    static std::vector<std::byte> encode(const int& value)
    {
        std::vector<std::byte> bytes(sizeof(int));
        std::memcpy(bytes.data(), &value, sizeof(int));
        return bytes;
    }

    static std::optional<int> decode(std::span<const std::byte> bytes)
    {
        if (bytes.size() != sizeof(int)) {
            return std::nullopt;
        }
        int value{};
        std::memcpy(&value, bytes.data(), sizeof(int));
        return value;
    }
};

/// A byte store for the tests: a map with an optional absolute expiry.
struct MemoryByteStore final : KeyValueStore {
    struct Record {
        std::vector<std::byte>                               bytes;
        std::optional<std::chrono::system_clock::time_point> expires_at;
    };

    std::map<String, Record> records;
    bool                     enumerable{ true };

    CacheOutcome<std::vector<std::byte>> load(const String& key) override
    {
        auto it = records.find(key);
        if (it == records.end()) {
            return { CacheStatus::Miss, CacheError::None, std::nullopt };
        }
        if (it->second.expires_at.has_value() && std::chrono::system_clock::now() >= *it->second.expires_at) {
            records.erase(it);
            return { CacheStatus::Miss, CacheError::None, std::nullopt };
        }
        return { CacheStatus::Ok, CacheError::None, it->second.bytes };
    }

    CacheResult store(const String& key, std::span<const std::byte> bytes, std::optional<std::chrono::system_clock::time_point> expires_at) override
    {
        records.insert_or_assign(key, Record{ std::vector<std::byte>(bytes.begin(), bytes.end()), expires_at });
        return {};
    }

    CacheOutcome<bool> erase(const String& key) override
    {
        return { CacheStatus::Ok, CacheError::None, records.erase(key) > 0 };
    }

    CacheResult clear() override
    {
        records.clear();
        return {};
    }

    CacheOutcome<std::size_t> count() override
    {
        if (!enumerable) {
            return { CacheStatus::Failure, CacheError::Unsupported, std::nullopt };
        }
        return { CacheStatus::Ok, CacheError::None, records.size() };
    }

    CacheOutcome<std::vector<String>> keys() override
    {
        if (!enumerable) {
            return { CacheStatus::Failure, CacheError::Unsupported, std::nullopt };
        }
        std::vector<String> result;
        result.reserve(records.size());
        for (const auto& entry : records) {
            result.push_back(entry.first);
        }
        return { CacheStatus::Ok, CacheError::None, std::move(result) };
    }
};

/// A store whose every operation fails, to prove errors are passed through.
struct FailingStore final : KeyValueStore {
    CacheOutcome<std::vector<std::byte>> load(const String&) override
    {
        return { CacheStatus::Failure, CacheError::Io, std::nullopt };
    }

    CacheResult store(const String&, std::span<const std::byte>, std::optional<std::chrono::system_clock::time_point>) override
    {
        return { CacheStatus::Failure, CacheError::Io };
    }

    CacheOutcome<bool> erase(const String&) override
    {
        return { CacheStatus::Failure, CacheError::Io, std::nullopt };
    }

    CacheResult clear() override
    {
        return { CacheStatus::Failure, CacheError::Io };
    }

    CacheOutcome<std::size_t> count() override
    {
        return { CacheStatus::Failure, CacheError::Io, std::nullopt };
    }

    CacheOutcome<std::vector<String>> keys() override
    {
        return { CacheStatus::Failure, CacheError::Io, std::nullopt };
    }
};

TEST(SerializedCacheTest, SetGetRoundTripThroughBytes)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    ASSERT_TRUE(cache.set(u8"k", 42, std::nullopt).ok());
    ASSERT_EQ(store.records.size(), 1u);
    EXPECT_EQ(store.records[u8"k"].bytes.size(), sizeof(int)); // it really encoded

    const auto value = cache.get(u8"k");
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(*value.value, 42);
}

TEST(SerializedCacheTest, WorksThroughTheBaseInterface)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> impl(store);
    Cache<String, int>&                 cache = impl;

    ASSERT_TRUE(cache.set(u8"k", 7, std::chrono::seconds(30)).ok());

    const auto value = cache.get(u8"k");
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(*value.value, 7);

    const auto present = cache.contains(u8"k");
    ASSERT_TRUE(present.ok());
    EXPECT_TRUE(*present.value);
}

TEST(SerializedCacheTest, MissIsAMiss)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    const auto value = cache.get(u8"absent");
    EXPECT_TRUE(value.missed());
    EXPECT_FALSE(value.failed());

    const auto present = cache.contains(u8"absent");
    ASSERT_TRUE(present.ok());
    EXPECT_FALSE(*present.value);
}

TEST(SerializedCacheTest, UndecodableBytesAreCorrupt)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    // A record the serializer cannot turn back into an int (one byte, not four).
    store.records[u8"k"] = MemoryByteStore::Record{ { std::byte{ 1 } }, std::nullopt };

    const auto value = cache.get(u8"k");
    EXPECT_TRUE(value.failed());
    EXPECT_EQ(value.error, CacheError::Corrupt);
}

TEST(SerializedCacheTest, StoreFailureIsPassedThrough)
{
    FailingStore                        store;
    SerializedCache<int, IntSerializer> cache(store);

    const auto value = cache.get(u8"k");
    EXPECT_TRUE(value.failed());
    EXPECT_EQ(value.error, CacheError::Io);

    EXPECT_TRUE(cache.set(u8"k", 1, std::nullopt).failed());
    EXPECT_EQ(cache.set(u8"k", 1, std::nullopt).error, CacheError::Io);
}

TEST(SerializedCacheTest, LifetimeBecomesAnAbsoluteExpiry)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    ASSERT_TRUE(cache.set(u8"k", 7, std::chrono::seconds(60)).ok());
    ASSERT_TRUE(store.records[u8"k"].expires_at.has_value());
    EXPECT_GT(*store.records[u8"k"].expires_at, std::chrono::system_clock::now());
}

TEST(SerializedCacheTest, NonPositiveLifetimeStoresNoExpiry)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    ASSERT_TRUE(cache.set(u8"a", 1, std::nullopt).ok());
    ASSERT_TRUE(cache.set(u8"b", 2, std::chrono::seconds(0)).ok());
    ASSERT_TRUE(cache.set(u8"c", 3, std::chrono::seconds(-1)).ok());

    EXPECT_FALSE(store.records[u8"a"].expires_at.has_value());
    EXPECT_FALSE(store.records[u8"b"].expires_at.has_value());
    EXPECT_FALSE(store.records[u8"c"].expires_at.has_value());
}

TEST(SerializedCacheTest, ExpiredRecordIsAMiss)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);

    ASSERT_TRUE(cache.set(u8"k", 7, std::chrono::seconds(1)).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_TRUE(cache.get(u8"k").missed());
}

TEST(SerializedCacheTest, EnumerationIsOptional)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);
    ASSERT_TRUE(cache.set(u8"k", 7, std::nullopt).ok());

    store.enumerable = false;

    const auto count = cache.count();
    EXPECT_TRUE(count.failed());
    EXPECT_EQ(count.error, CacheError::Unsupported);

    const auto keys = cache.keys();
    EXPECT_TRUE(keys.failed());
    EXPECT_EQ(keys.error, CacheError::Unsupported);
}

TEST(SerializedCacheTest, RemoveAndClear)
{
    MemoryByteStore                     store;
    SerializedCache<int, IntSerializer> cache(store);
    ASSERT_TRUE(cache.set(u8"a", 1, std::nullopt).ok());
    ASSERT_TRUE(cache.set(u8"b", 2, std::nullopt).ok());

    const auto removed = cache.remove(u8"a");
    ASSERT_TRUE(removed.ok());
    EXPECT_TRUE(*removed.value);
    EXPECT_FALSE(*cache.remove(u8"a").value);

    EXPECT_TRUE(cache.clear().ok());
    EXPECT_EQ(*cache.count().value, 0u);
}

} // namespace
