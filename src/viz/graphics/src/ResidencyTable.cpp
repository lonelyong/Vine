#include "ResidencyTable.hpp"

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

bool ResidencyTable::touch(ContentRef ref, std::uint64_t epoch) noexcept
{
    if (ref.id.isNull()) {
        return false;
    }
    auto [entry, inserted] = last_epoch_.try_emplace(ref, epoch);
    if (!inserted) {
        entry->second = std::max(entry->second, epoch);
    }
    return true;
}

bool ResidencyTable::contains(ContentRef ref) const noexcept
{
    return last_epoch_.find(ref) != last_epoch_.end();
}

std::size_t ResidencyTable::liveCount() const noexcept
{
    return last_epoch_.size();
}

std::vector<ContentRef> ResidencyTable::collectable(std::uint64_t oldest_live) const
{
    std::vector<ContentRef> refs;
    refs.reserve(last_epoch_.size());
    for (const auto& [ref, last_epoch] : last_epoch_) {
        if (last_epoch < oldest_live) {
            refs.push_back(ref);
        }
    }
    return refs;
}

std::size_t ResidencyTable::reclaim(std::uint64_t oldest_live)
{
    std::size_t removed = 0U;
    for (auto entry = last_epoch_.begin(); entry != last_epoch_.end();) {
        if (entry->second < oldest_live) {
            entry = last_epoch_.erase(entry);
            ++removed;
        }
        else {
            ++entry;
        }
    }
    return removed;
}

void ResidencyTable::clear() noexcept
{
    last_epoch_.clear();
}

} // namespace detail

VN_GRAPHICS_NS_END
