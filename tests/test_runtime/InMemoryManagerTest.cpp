#include <any>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <vine/runtime/CacheManager.hpp>
#include <vine/runtime/InMemoryManager.hpp>

using vn::String;
using vn::runtime::CacheManager;
using vn::runtime::InMemoryManager;

namespace
{

TEST(InMemoryManagerTest, SetGetRoundTrip)
{
    InMemoryManager cache;
    cache.set(u8"k", 42, 0);
    const auto value = cache.get(u8"k");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::any_cast<int>(value), 42);
}

TEST(InMemoryManagerTest, WorksThroughInterface)
{
    // The bridge implements the CacheManager interface, so it can be held and
    // driven through the abstraction (e.g. registered in a DI container).
    InMemoryManager impl;
    CacheManager*   cache = &impl;
    cache->set(u8"k", std::string("hi"), 0);

    const auto value = cache->get(u8"k");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(std::any_cast<std::string>(value), "hi");
    EXPECT_TRUE(cache->contains(u8"k"));
    EXPECT_EQ(cache->count(), 1u);

    const auto keys = cache->keys();
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], u8"k");

    EXPECT_TRUE(cache->remove(u8"k"));
    EXPECT_TRUE(cache->isEmpty());
}

TEST(InMemoryManagerTest, MissingReturnsEmpty)
{
    InMemoryManager cache;
    EXPECT_FALSE(cache.get(u8"nope").has_value());
    EXPECT_FALSE(cache.contains(u8"nope"));
}

TEST(InMemoryManagerTest, TtlExpiresThroughInterface)
{
    // A long sweep interval so expiry is driven by access here.
    InMemoryManager cache(std::chrono::seconds(30));
    cache.set(u8"forever", 1, 0);
    cache.set(u8"short", 2, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_FALSE(cache.contains(u8"short"));
    EXPECT_TRUE(cache.contains(u8"forever"));
}

TEST(InMemoryManagerTest, BackgroundSweepAutoRemovesExpired)
{
    InMemoryManager cache(std::chrono::milliseconds(100));
    cache.set(u8"forever", 1, 0);
    cache.set(u8"short", 2, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_FALSE(cache.contains(u8"short"));
    EXPECT_TRUE(cache.contains(u8"forever"));
}

} // namespace
