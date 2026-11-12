#include <vine/vsg/internal/ContentSweep.hpp>

VN_VSG_NS_BEGIN

SweepOutcome releaseAbandonedContent(ContentStore&                          store,
                                     MaterialImages&                        images,
                                     std::vector<vn::graphics::MaterialId>& released_materials)
{
    SweepOutcome outcome;

    // The store first: it marks what the host let go and drops the entries whose rows are already gone. Nothing is
    // parked here - the rows leave by epoch at the frame boundary (see api/ContentStore).
    outcome.objects  = store.releaseAbandoned(released_materials);
    // The images need no window: what this drops is the cache's own share of objects the sampler (and any
    // retained descriptor set that names them) holds for as long as it is recorded (see the file note).
    outcome.textures = images.releaseAbandoned();
    return outcome;
}

VN_VSG_NS_END
