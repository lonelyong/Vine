#include <vine/graphics/ContentId.hpp>

#include <mutex>

VN_GRAPHICS_NS_BEGIN

ContentRegistry::ContentRegistry(std::uint8_t domain) noexcept
  : domain_(domain)
{}

ContentRegistry& ContentRegistry::instance() noexcept
{
    // Domain 0 is the process default every content object takes its id from. Function-local static: thread-safe
    // initialisation, no static-init-order dependency on a content object built in another translation unit.
    static ContentRegistry s_default{ 0 };
    return s_default;
}

Handle ContentRegistry::allocateHandle() noexcept
{
    std::lock_guard lock(mutex_);
    if (free_head_ != kNone) {
        const std::uint32_t index = free_head_;
        Slot&               slot  = slots_[index];
        free_head_                = slot.next_free;
        slot.next_free            = kNone;
        return Handle::make(index, slot.generation, domain_);
    }
    const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
    slots_.push_back(Slot{});
    return Handle::make(index, slots_.back().generation, domain_);
}

void ContentRegistry::release(Handle handle) noexcept
{
    std::lock_guard lock(mutex_);
    if (handle.domain() != domain_ || handle.index() >= slots_.size() || handle.generation() == 0U) {
        return;
    }
    Slot& slot = slots_[handle.index()];
    if (slot.generation != handle.generation()) {
        return; // already released, or the slot has moved on: a double release never moves a generation twice
    }
    if (slot.generation >= Handle::kGenerationMax) {
        // The next generation would be 0, which reads as the null handle; retire the slot instead of wrapping (ABA).
        slot.generation = 0;
        slot.next_free  = kNone;
        return;
    }
    ++slot.generation;
    slot.next_free = free_head_;
    free_head_     = handle.index();
}

bool ContentRegistry::alive(Handle handle) const noexcept
{
    std::lock_guard lock(mutex_);
    if (handle.domain() != domain_ || handle.index() >= slots_.size() || handle.generation() == 0U) {
        return false;
    }
    return slots_[handle.index()].generation == handle.generation();
}

std::size_t ContentRegistry::peakObjects() const noexcept
{
    std::lock_guard lock(mutex_);
    return slots_.size();
}

VN_GRAPHICS_NS_END
