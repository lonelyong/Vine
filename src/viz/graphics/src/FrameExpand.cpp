#include "FrameExpand.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

void bindSceneDraws(PassDraw& pass, const vn::graphics::DrawListSource* source)
{
    if (source == nullptr) {
        return;
    }
    for (DrawCall& draw : pass.draws) {
        if (draw.kind == DrawCall::Kind::Scene && draw.content == nullptr) {
            draw.content = source;
        }
    }
}

void expandPass(PassDraw& pass)
{
    for (DrawCall& draw : pass.draws) {
        if (draw.kind != DrawCall::Kind::Scene || draw.commands != nullptr) {
            continue; // a screen call, or a list the producer already had: nothing to collect
        }
        if (!draw.view.present() || draw.content == nullptr) {
            continue; // no view to build, or nobody to ask: the backend records nothing for it
        }
        draw.commands = draw.content->collectDrawItems(draw.view);
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
