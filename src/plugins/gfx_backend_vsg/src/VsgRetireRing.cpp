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
    ring[head].emplace_back(std::move(object));
}

void VsgRetireRing::advance()
{
    // Move to the next bucket and release it: it was filled kRetireRingDepth
    // advances ago, so every command-buffer slot that could have recorded one of
    // its objects has been re-recorded since (and start() waited on that slot's
    // fence before re-recording it), and the GPU no longer executes them.
    head = (head + 1u) % kRetireRingDepth;
    released += ring[head].size();
    ring[head].clear();
}

void VsgRetireRing::waitForIdle(::vsg::ref_ptr<::vsg::Viewer> viewer)
{
    if (viewer == nullptr) {
        return;
    }
    ++waits;
    viewer->deviceWaitIdle();
}

V_VSG_NS_END
