#include <vine/graphics/LightSet.hpp>

#include <utility>

VN_GRAPHICS_NS_BEGIN

LightSet::LightSet(std::vector<LightValue> values)
  : values_(std::move(values))
{
}

std::shared_ptr<const LightSet> LightSet::create(std::vector<LightValue> values)
{
    // `make_shared` cannot see the private constructor, and the allocation is the point of this type: one block for
    // the control record and the set, made here and never again for this publication.
    return std::shared_ptr<const LightSet>(new LightSet(std::move(values)));
}

std::span<const LightValue> LightSet::values() const noexcept
{
    return std::span<const LightValue>(values_.data(), values_.size());
}

std::size_t LightSet::size() const noexcept
{
    return values_.size();
}

bool LightSet::empty() const noexcept
{
    return values_.empty();
}

VN_GRAPHICS_NS_END
