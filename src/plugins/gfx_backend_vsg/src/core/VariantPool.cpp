#include <vine/vsg/core/VariantPool.hpp>

#include <algorithm>

V_VSG_NS_BEGIN

namespace core
{

VariantPool::VariantPool(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1U))
{
}

VariantPool::Lookup VariantPool::acquire(const PipelineKey& key)
{
    const auto found = index_.find(key);
    if (found != index_.end()) {
        ++reused_;
        return {Action::Reused, found->second->id};
    }

    Entry entry{key, next_id_++};
    order_.push_back(std::move(entry));
    const auto position = std::prev(order_.end());
    index_[position->key] = position;
    by_id_[position->id]  = position;
    ++created_;
    const std::uint64_t id = position->id;

    // Past the capacity the OLDEST entry leaves the map: an application that keeps generating programs must
    // not grow this for ever, and the holder of the evicted variant still owns its GPU object (see the file
    // note) - only the lookup is gone.
    if (order_.size() > capacity_) {
        const Entry& oldest = order_.front();
        by_id_.erase(oldest.id);
        index_.erase(oldest.key);
        order_.pop_front();
        ++evictions_;
    }

    return {Action::Created, id};
}

bool VariantPool::contains(std::uint64_t id) const noexcept
{
    return by_id_.find(id) != by_id_.end();
}

const PipelineKey* VariantPool::keyOf(std::uint64_t id) const noexcept
{
    const auto found = by_id_.find(id);
    return found == by_id_.end() ? nullptr : &found->second->key;
}

std::size_t VariantPool::variants() const noexcept
{
    return index_.size();
}

std::uint64_t VariantPool::created() const noexcept
{
    return created_;
}

std::uint64_t VariantPool::reused() const noexcept
{
    return reused_;
}

std::uint64_t VariantPool::evictions() const noexcept
{
    return evictions_;
}

void VariantPool::clear() noexcept
{
    order_.clear();
    index_.clear();
    by_id_.clear();
}

}  // namespace core

V_VSG_NS_END
