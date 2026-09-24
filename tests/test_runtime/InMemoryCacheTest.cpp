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
    const int shared = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&cache, shared] {
            for (int i = 0; i < 500; ++i)
            {
                cache.set(shared, "x", 0);
                (void)cache.get(shared);
                (void)cache.contains(shared);
                cache.removeExpired();
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_TRUE(cache.contains(shared));
}

} // namespace
