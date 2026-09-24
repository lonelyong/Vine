#include <vine/runtime/InMemoryManager.hpp>

#include <chrono>
#include <utility>

VN_RUNTIME_NS_BEGIN

InMemoryManager::InMemoryManager(std::chrono::milliseconds sweep_interval)
  : cache_()
{
    cache_.start(sweep_interval);
}

InMemoryManager::~InMemoryManager() = default;

void InMemoryManager::set(const String& key, std::any value, size_t timeout_seconds)
{
    cache_.set(key, std::move(value), static_cast<int>(timeout_seconds));
}

std::any InMemoryManager::get(const String& key)
{
    auto value = cache_.get(key);
    return value.has_value() ? std::move(*value) : std::any{};
}

bool InMemoryManager::contains(const String& key)
{
    return cache_.contains(key);
}

bool InMemoryManager::remove(const String& key)
{
    return cache_.remove(key);
}

size_t InMemoryManager::removeExpired()
{
    return cache_.removeExpired();
}

void InMemoryManager::clear()
{
    cache_.clear();
}

size_t InMemoryManager::count()
{
    return cache_.count();
}

bool InMemoryManager::isEmpty()
{
    return cache_.isEmpty();
}

std::vector<String> InMemoryManager::keys()
{
    return cache_.keys();
}

VN_RUNTIME_NS_END
