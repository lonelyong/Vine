#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <vine/String.hpp>
#include <vine/runtime/Cache.hpp>
#include <vine/runtime/InMemoryCache.hpp>

using vn::String;
using vn::runtime::Cache;
using vn::runtime::CacheOutcome;
using vn::runtime::InMemoryCache;

namespace
{

/// Copy-constructible but neither copy- nor move-assignable: set() must not require
/// assignment, so replacing an entry cannot go through insert_or_assign.
struct CopyOnly {
    int value{ 0 };

    CopyOnly() = default;
    explicit CopyOnly(int value_) : value(value_) {}
    CopyOnly(const CopyOnly&)            = default;
    CopyOnly(CopyOnly&&)                 = delete;
    CopyOnly& operator=(const CopyOnly&) = delete;
    CopyOnly& operator=(CopyOnly&&)      = delete;
};

/// The value of an Ok outcome; asserts the status first so a miss fails loudly.
template <class TValue>
std::optional<TValue> valueOf(CacheOutcome<TValue> outcome)
{
    EXPECT_TRUE(outcome.ok());
    return outcome.value;
}

/// The answer of a boolean operation; asserts that the operation ran.
bool answerOf(CacheOutcome<bool> outcome)
{
    EXPECT_TRUE(outcome.ok());
    return outcome.value.value_or(false);
}

TEST(InMemoryCacheTest, SetGetRoundTrip)
{
    InMemoryCache<int, std::string> cache;
    EXPECT_TRUE(cache.set(1, "hello", std::nullopt).ok());

    const auto value = valueOf(cache.get(1));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "hello");
}

TEST(InMemoryCacheTest, ImplementsTheCacheInterface)
{
    InMemoryCache<int, std::string> impl;
    Cache<int, std::string>&        cache = impl;

    ASSERT_TRUE(cache.set(1, "hi", std::chrono::seconds(30)).ok());

    const auto hit = cache.get(1);
    ASSERT_TRUE(hit.ok());
    EXPECT_EQ(*hit.value, "hi");

    const auto present = cache.contains(1);
    ASSERT_TRUE(present.ok());
    EXPECT_TRUE(*present.value);

    const auto removed = cache.remove(1);
    ASSERT_TRUE(removed.ok());
    EXPECT_TRUE(*removed.value);
    EXPECT_TRUE(cache.get(1).missed());
}

TEST(InMemoryCacheTest, NonPositiveLifetimeKeepsTheEntry)
{
    // The interface's rule: an empty or non-positive lifetime means "keep".
    InMemoryCache<int, std::string> cache;
    cache.set(1, "empty", std::nullopt);
    cache.set(2, "zero", std::chrono::seconds(0));
    cache.set(3, "negative", std::chrono::seconds(-1));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 3u);
}

TEST(InMemoryCacheTest, MissingReportsMiss)
{
    InMemoryCache<int, std::string> cache;
    const auto outcome = cache.get(42);
    EXPECT_TRUE(outcome.missed());
    EXPECT_FALSE(outcome.failed());
    EXPECT_FALSE(outcome.value.has_value());

    const auto present = cache.contains(42);
    ASSERT_TRUE(present.ok());
    EXPECT_FALSE(*present.value);
}

TEST(InMemoryCacheTest, OverwriteReplacesValue)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", std::nullopt);
    cache.set(1, "b", std::nullopt);
    EXPECT_EQ(valueOf(cache.get(1)).value_or(""), "b");
}

TEST(InMemoryCacheTest, StringKeysAreTyped)
{
    InMemoryCache<String, int> cache;
    cache.set(u8"key", 7, std::nullopt);
    EXPECT_EQ(valueOf(cache.get(u8"key")).value_or(0), 7);
}

TEST(InMemoryCacheTest, RemoveAndClear)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", std::nullopt);
    cache.set(2, "b", std::nullopt);
    EXPECT_TRUE(answerOf(cache.remove(1)));
    EXPECT_FALSE(answerOf(cache.remove(1)));
    EXPECT_FALSE(answerOf(cache.contains(1)));
    EXPECT_TRUE(cache.clear().ok());
    EXPECT_FALSE(answerOf(cache.contains(2)));
}

TEST(InMemoryCacheTest, TtlExpires)
{
    // No start(): no background thread, so expiry is driven purely by access.
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", std::nullopt);
    cache.set(2, "short", std::chrono::seconds(1));
    EXPECT_TRUE(answerOf(cache.contains(2)));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_TRUE(cache.get(2).missed());
    EXPECT_FALSE(answerOf(cache.contains(2)));
    EXPECT_EQ(valueOf(cache.get(1)).value_or(""), "forever");
}

TEST(InMemoryCacheTest, RemoveExpiredCleansOnlyExpired)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", std::nullopt);
    cache.set(2, "short", std::chrono::seconds(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(cache.removeExpired(), 1u);
    EXPECT_FALSE(answerOf(cache.contains(2)));
    EXPECT_TRUE(answerOf(cache.contains(1)));
}

TEST(InMemoryCacheTest, CountAndEmpty)
{
    InMemoryCache<int, std::string> cache;
    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 0u);

    cache.set(1, "a", std::nullopt);
    cache.set(2, "b", std::nullopt);
    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 2u);

    cache.remove(1);
    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 1u);

    cache.clear();
    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 0u);
}

TEST(InMemoryCacheTest, CountIgnoresExpired)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", std::nullopt);
    cache.set(2, "short", std::chrono::seconds(1));
    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 2u);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(valueOf(cache.count()).value_or(99u), 1u);
    EXPECT_FALSE(answerOf(cache.contains(2)));
    EXPECT_TRUE(answerOf(cache.contains(1)));
}

TEST(InMemoryCacheTest, KeysReturnsLiveKeys)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", std::nullopt);
    cache.set(2, "b", std::nullopt);
    cache.set(3, "gone", std::chrono::seconds(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    const auto keys = valueOf(cache.keys()).value_or(std::vector<int>{});
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_NE(std::find(keys.begin(), keys.end(), 1), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), 2), keys.end());
    EXPECT_EQ(std::find(keys.begin(), keys.end(), 3), keys.end());
}

TEST(InMemoryCacheTest, StartStopLifecycle)
{
    InMemoryCache<int, std::string> cache;
    EXPECT_FALSE(cache.isCleanupRunning());

    cache.start();
    EXPECT_TRUE(cache.isCleanupRunning());

    cache.stop();
    EXPECT_FALSE(cache.isCleanupRunning());
}

TEST(InMemoryCacheTest, BackgroundSweepAutoRemovesExpired)
{
    // A short sweep period lets the shared background thread reclaim the
    // expired entry on its own, with no explicit removeExpired() call.
    InMemoryCache<int, std::string> cache;
    cache.start(std::chrono::milliseconds(100));
    cache.set(1, "forever", std::nullopt);
    cache.set(2, "short", std::chrono::seconds(1));

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_FALSE(answerOf(cache.contains(2)));
    EXPECT_TRUE(answerOf(cache.contains(1)));
    cache.stop();
}

TEST(InMemoryCacheTest, ConcurrentAccessIsSafe)
{
    InMemoryCache<int, std::string> cache;
    const int                       shared = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&cache, shared] {
            for (int i = 0; i < 500; ++i) {
                (void)cache.set(shared, "x", std::nullopt);
                (void)cache.get(shared);
                (void)cache.contains(shared);
                cache.removeExpired();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(answerOf(cache.contains(shared)));
}

TEST(InMemoryCacheTest, OverwriteNeedsCopyConstructionOnly)
{
    InMemoryCache<int, CopyOnly> cache;
    cache.set(1, CopyOnly(1), std::nullopt);
    cache.set(1, CopyOnly(2), std::nullopt); // compiling this line is half the assertion

    const auto value = valueOf(cache.get(1));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->value, 2);
}

TEST(InMemoryCacheTest, OneWorkerSweepsEveryStartedInstance)
{
    InMemoryCache<int, std::string> first;
    InMemoryCache<int, std::string> second;
    first.start(std::chrono::milliseconds(50));
    second.start(std::chrono::milliseconds(50));
    first.set(1, "short", std::chrono::seconds(1));
    second.set(1, "short", std::chrono::seconds(1));

    // No explicit removeExpired(): the shared worker serves both instances.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_FALSE(answerOf(first.contains(1)));
    EXPECT_FALSE(answerOf(second.contains(1)));
    first.stop();
    second.stop();
}

TEST(InMemoryCacheTest, StartStopChurnFromSeveralThreadsLeavesAWorkingWorker)
{
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < 50; ++i) {
                InMemoryCache<int, std::string> cache;
                cache.start(std::chrono::milliseconds(5));
                cache.set(i, "v", std::nullopt);
                // Destruction unregisters; the worker may exit and be started
                // again by the next iteration or by another thread.
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    // After the churn a new instance must still be served.
    InMemoryCache<int, std::string> cache;
    cache.start(std::chrono::milliseconds(50));
    cache.set(1, "short", std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_FALSE(answerOf(cache.contains(1)));
    cache.stop();
}

} // namespace
