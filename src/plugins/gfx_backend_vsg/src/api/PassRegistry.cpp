#include <vine/vsg/api/PassRegistry.hpp>

VN_VSG_NS_BEGIN

core::PassId PassRegistry::adopt(const void* identity)
{
    if (identity == nullptr)
    {
        return 0;  // "no pass" is spelled 0 everywhere in the plan (see the header)
    }
    const auto found = ids_.find(identity);
    if (found != ids_.end())
    {
        return found->second;
    }
    const core::PassId id = next_++;
    ids_.emplace(identity, id);
    return id;
}

bool PassRegistry::release(const void* identity) noexcept
{
    if (identity == nullptr)
    {
        return false;
    }
    return ids_.erase(identity) != 0U;
}

bool PassRegistry::contains(const void* identity) const noexcept
{
    return identity != nullptr && ids_.find(identity) != ids_.end();
}

std::size_t PassRegistry::live() const noexcept
{
    return ids_.size();
}

VN_VSG_NS_END
