#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <vine/String.hpp>
#include <vine/runtime/InMemoryCache.hpp>

using vn::String;
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

TEST(InMemoryCacheTest, SetGetRoundTrip)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "hello", 0);
    const auto value = cache.get(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "hello");
}

TEST(InMemoryCacheTest, GetIntoOutput)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "hello", 0);

    std::string out;
    EXPECT_TRUE(cache.get(1, out));
    EXPECT_EQ(out, "hello");

    std::string missing;
    EXPECT_FALSE(cache.get(999, missing));
}

TEST(InMemoryCacheTest, NegativeTtlMeansForever)
{
    // Reference convention: ttl <= 0 keeps the entry forever. A negative
    // literal must not overflow into an immediate expiry.
    InMemoryCache<int, std::string> cache;
    cache.set(1, "keep", -1);
    EXPECT_TRUE(cache.contains(1));
    EXPECT_EQ(*cache.get(1), "keep");
}

TEST(InMemoryCacheTest, MissingReturnsNullopt)
{
    InMemoryCache<int, std::string> cache;
    EXPECT_FALSE(cache.get(42).has_value());
    EXPECT_FALSE(cache.contains(42));
}

TEST(InMemoryCacheTest, OverwriteReplacesValue)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", 0);
    cache.set(1, "b", 0);
    const auto value = cache.get(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "b");
}

TEST(InMemoryCacheTest, StringKeysAreTyped)
{
    InMemoryCache<String, int> cache;
    cache.set(u8"key", 7, 0);
    const auto value = cache.get(u8"key");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 7);
}

TEST(InMemoryCacheTest, RemoveAndClear)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", 0);
    cache.set(2, "b", 0);
    EXPECT_TRUE(cache.remove(1));
    EXPECT_FALSE(cache.remove(1));
    EXPECT_FALSE(cache.contains(1));
    cache.clear();
    EXPECT_FALSE(cache.contains(2));
}

TEST(InMemoryCacheTest, TtlExpires)
{
    // No start(): no background thread, so expiry is driven purely by access.
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", 0);
    cache.set(2, "short", 1);
    EXPECT_TRUE(cache.contains(2));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_FALSE(cache.contains(2));
    EXPECT_FALSE(cache.get(2).has_value());
    EXPECT_TRUE(cache.contains(1));
    EXPECT_EQ(*cache.get(1), "forever");
}

TEST(InMemoryCacheTest, RemoveExpiredCleansOnlyExpired)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", 0);
    cache.set(2, "short", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(cache.removeExpired(), 1u);
    EXPECT_FALSE(cache.contains(2));
    EXPECT_TRUE(cache.contains(1));
}

TEST(InMemoryCacheTest, CountAndEmpty)
{
    InMemoryCache<int, std::string> cache;
    EXPECT_TRUE(cache.isEmpty());
    EXPECT_EQ(cache.count(), 0u);

    cache.set(1, "a", 0);
    cache.set(2, "b", 0);
    EXPECT_FALSE(cache.isEmpty());
    EXPECT_EQ(cache.count(), 2u);

    cache.remove(1);
    EXPECT_EQ(cache.count(), 1u);

    cache.clear();
    EXPECT_TRUE(cache.isEmpty());
    EXPECT_EQ(cache.count(), 0u);
}

TEST(InMemoryCacheTest, CountIgnoresExpired)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "forever", 0);
    cache.set(2, "short", 1);
    EXPECT_EQ(cache.count(), 2u);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(cache.count(), 1u);
    EXPECT_FALSE(cache.contains(2));
    EXPECT_TRUE(cache.contains(1));
}

TEST(InMemoryCacheTest, KeysReturnsLiveKeys)
{
    InMemoryCache<int, std::string> cache;
    cache.set(1, "a", 0);
    cache.set(2, "b", 0);
    cache.set(3, "gone", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    const auto keys = cache.keys();
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
    cache.set(1, "forever", 0);
    cache.set(2, "short", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_FALSE(cache.contains(2));
    EXPECT_TRUE(cache.contains(1));
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
                cache.set(shared, "x", 0);
                (void)cache.get(shared);
                (void)cache.contains(shared);
                cache.removeExpired();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(cache.contains(shared));
}

TEST(InMemoryCacheTest, OverwriteNeedsCopyConstructionOnly)
{
    InMemoryCache<int, CopyOnly> cache;
    cache.set(1, CopyOnly(1), 0);
    cache.set(1, CopyOnly(2), 0); // compiling this line is half the assertion

    const auto value = cache.get(1);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->value, 2);
}

TEST(InMemoryCacheTest, OneWorkerSweepsEveryStartedInstance)
{
    InMemoryCache<int, std::string> first;
    InMemoryCache<int, std::string> second;
    first.start(std::chrono::milliseconds(50));
    second.start(std::chrono::milliseconds(50));
    first.set(1, "short", 1);
    second.set(1, "short", 1);

    // No explicit removeExpired(): the shared worker serves both instances.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_FALSE(first.contains(1));
    EXPECT_FALSE(second.contains(1));
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
                cache.set(i, "v", 0);
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
    cache.set(1, "short", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_FALSE(cache.contains(1));
    cache.stop();
}

} // namespace
