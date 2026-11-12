#include <vine/graphics/SceneSnapshotRef.hpp>

#include "SceneSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

SceneSnapshotRef::SceneSnapshotRef(std::shared_ptr<const detail::SceneSnapshot> value) noexcept
  : value_(std::move(value))
{}

bool SceneSnapshotRef::valid() const noexcept
{
    return value_ != nullptr;
}

SceneSnapshotRef::operator bool() const noexcept
{
    return valid();
}

std::uint64_t SceneSnapshotRef::revision() const noexcept
{
    return value_ != nullptr ? value_->revision : 0U;
}

const std::shared_ptr<const detail::SceneSnapshot>& SceneSnapshotRef::published() const noexcept
{
    return value_;
}

VN_GRAPHICS_NS_END
