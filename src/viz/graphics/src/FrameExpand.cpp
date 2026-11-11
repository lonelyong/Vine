#include "FrameExpand.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

CollectView collectViewOf(const DrawDescription& draw) noexcept
{
    CollectView view;
    view.projection = draw.camera.projection;
    view.view       = draw.camera.view;
    view.eye        = draw.camera.eye;
    view.up         = draw.camera.up;
    view.viewport   = draw.collect_viewport;
    return view;
}

void expandPass(PassDraw& pass)
{
    for (DrawDescription& draw : pass.draws) {
        if (draw.kind != DrawDescription::Kind::Scene || draw.commands != nullptr) {
            continue; // a screen call, or a list the producer already had: nothing to collect
        }
        if (!draw.camera.present || draw.content == nullptr) {
            continue; // no view to build, or nobody to ask: the backend records nothing for it
        }
        draw.commands = draw.content->collectRenderCommands(collectViewOf(draw), draw.layer_mask);
    }
}

void expandFrame(FrameDescription& frame)
{
    for (FrameDescription::Pass& entry : frame.passes) {
        if (entry.draw.has_value()) {
            expandPass(*entry.draw);
        }
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
