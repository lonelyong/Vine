#include <vine/graphics/RenderBackend.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderBackend, vn::Object);

FrameDescription singlePassFrame(raw_ptr<const RenderPass> pass, int order,
                                 std::vector<ImageSource> inputs,
                                 std::optional<PassDraw> draw)
{
    // The ONE place a frame value is assembled outside the engine's compile, so a host that drives one pass
    // (RenderPass::execute) cannot build a shape of its own - there is one value in this SDK (see
    // RenderFrameExecutor::executeFrame).
    FrameDescription frame;
    FrameDescription::Pass entry;
    entry.pass   = pass;
    entry.order  = order;
    entry.inputs = std::move(inputs);
    entry.draw   = std::move(draw);
    frame.passes.push_back(std::move(entry));
    return frame;
}

VN_GRAPHICS_NS_END
