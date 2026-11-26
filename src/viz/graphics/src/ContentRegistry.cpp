#include <vine/graphics/ContentId.hpp>

#include <mutex>

VN_GRAPHICS_NS_BEGIN

ContentRegistry::ContentRegistry(std::uint8_t domain) noexcept
  : domain_(domain)
{}

ContentRegistry& ContentRegistry::instance() noexcept
{
    // THE DEFAULT REGISTRY IS NEVER DESTROYED, and that is the point (see the header: "it lives as long as the
    // process"). Content objects are held by OTHER translation units' statics - AxesMarker's prototype cache is one -
    // whose destructors run at library finalisation, after a function-local static constructed at first use has already
    // been destroyed (both are registered in the same finalisation pass, and the later registration runs first). The
    // registry would then be read after its own destruction while the last handle of such an object is released, which
    // is a SEGV, not a leak check finding (measured: `test_graphics` under TSan, `__cxa_finalize` ->
    // `AxesMarker::Prototype::~Prototype` -> `Material::~Material` -> `ContentRegistry::release`; see
    // `.ai/bugs/graphics-default-content-registry-outlived-by-statics.md`).
    //
    // A LEAKED SINGLETON IS THE ONLY SPELLING THAT KEEPS THE PROMISE: a static object is destroyed in an order this
    // process cannot control, and every alternative (a static buffer, `no_destroy`) is the same leak without the
    // allocator's bookkeeping. It is reachable from this static pointer, so LeakSanitizer does not report it, and it is
    // one object per process.
    static ContentRegistry* const s_default = new ContentRegistry{ 0 };
    return *s_default;
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
