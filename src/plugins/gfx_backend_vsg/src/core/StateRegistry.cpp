#include <vine/vsg/core/StateRegistry.hpp>

V_VSG_NS_BEGIN

namespace core
{

StateRegistry::StateRegistry(VariantPool& pool) noexcept : pool_(&pool)
{
}

StateRegistry::Resolution StateRegistry::resolve(const PipelineKey& key, const DynamicState& state)
{
    const VariantPool::Lookup lookup = pool_->acquire(key);

    Resolution resolution;
    resolution.variant = lookup.id;
    // "Nothing is bound yet" counts as a switch: the pass has to bind before it draws, and a caller that
    // skipped that because "the variant did not change" would draw with whatever was bound last.
    resolution.variant_switched = !bound_ || lookup.id != current_variant_;
    // The dynamic values live in the recording, so a fresh recording has none: only a value that is equal to
    // what this pass ALREADY issued can be skipped.
    resolution.dynamic_issued = !bound_ || !(state == current_state_);

    ++resolutions_;
    if (resolution.variant_switched) {
        ++variant_switches_;
    }
    if (resolution.dynamic_issued) {
        ++dynamic_issued_;
    }
    else {
        ++dynamic_skipped_;
    }

    bound_           = true;
    current_variant_ = lookup.id;
    current_state_   = state;
    return resolution;
}

void StateRegistry::reset() noexcept
{
    bound_           = false;
    current_variant_ = 0;
}

std::uint64_t StateRegistry::currentVariant() const noexcept
{
    return bound_ ? current_variant_ : 0U;
}

const DynamicState& StateRegistry::currentState() const noexcept
{
    return current_state_;
}

std::uint64_t StateRegistry::resolutions() const noexcept
{
    return resolutions_;
}

std::uint64_t StateRegistry::variant_switches() const noexcept
{
    return variant_switches_;
}

std::uint64_t StateRegistry::dynamic_issued() const noexcept
{
    return dynamic_issued_;
}

std::uint64_t StateRegistry::dynamic_skipped() const noexcept
{
    return dynamic_skipped_;
}

}  // namespace core

V_VSG_NS_END
