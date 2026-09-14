#include <vine/vsg/VsgRetireRing.hpp>

#include <utility>

V_VSG_NS_BEGIN

// The ring's own unit: parking replaced GPU objects is a concept of its own, and both the
// policy and its two diagnostics are documented in VsgRetireRing.hpp. Leaving it inside the
// renderer's target bookkeeping is what made that TU the place everything ended up.

void VsgRetireRing::park(::vsg::ref_ptr<::vsg::Object> object)
{
    if (object == nullptr) {
        return;
    }
    parked.park(std::move(object));
}

void VsgRetireRing::advance()
{
    // Hand the bucket that was filled kRetireRingDepth advances ago to the clock, which drops it:
    // every command-buffer slot that could have recorded one of its objects has been re-recorded
    // since (and start() waited on that slot's fence before re-recording it), so the GPU no longer
    // executes them.
    released_ += parked.advance();
}

void VsgRetireRing::waitForIdle(::vsg::ref_ptr<::vsg::Viewer> viewer)
{
    if (viewer == nullptr) {
        return;
    }
    ++waits_;
    viewer->deviceWaitIdle();
}

V_VSG_NS_END
