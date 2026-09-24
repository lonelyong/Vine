#include <vine/deprecated/RefObject.hpp>

VN_CORE_NS_BEGIN

VN_OBJECT_META_IMPL(RefObject, Object)

RefObject::RefObject() noexcept
{
    cb_ = new PtrControlBlock();
    // The object holds an implicit weak reference to the control block
    // so that the control block can outlive the object until all weak
    // references are released. Initialize weak_refs to 1 for that owner.
    cb_->weak_refs.store(1, std::memory_order_relaxed);
}

RefObject::~RefObject() noexcept
{
    if (cb_) {
        // Release the implicit weak reference the object holds to the
        // control block. If this was the last weak reference, reclaim
        // the control block immediately — no one else can reach it.
        if (cb_->weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete cb_;
    }
}

// Note: strong/weak ref management moved to SPtr/WPtr in Ptr.hpp.

VN_CORE_NS_END
