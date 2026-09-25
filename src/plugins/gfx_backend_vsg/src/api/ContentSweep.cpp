#include <vine/vsg/api/ContentSweep.hpp>

VN_VSG_NS_BEGIN

SweepOutcome releaseAbandonedContent(ContentStore& store, MaterialImages& images, core::FrameTimeline& timeline,
                                     core::RetirementQueue& retirement)
{
    SweepOutcome outcome;

    // The store first, and it parks: the rows it drops are the ones a plan recorded in the frame that is
    // still being finished may name, and the caller advances the queue after this call (see the file note).
    outcome.objects  = store.releaseAbandoned(timeline, retirement);
    // The images need no window: what this drops is the cache's own share of objects the sampler (and any
    // retained descriptor set that names them) holds for as long as it is recorded (see the file note).
    outcome.textures = images.releaseAbandoned();
    return outcome;
}

VN_VSG_NS_END
