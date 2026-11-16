#include <vine/graphics/RenderPass.hpp>

#include <utility>

#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/Scene.hpp>

#include "FrameExpand.hpp"
#include "PublicationStore.hpp"

VN_GRAPHICS_NS_BEGIN

void RenderPass::execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend, raw_ptr<PresentTarget> present_to)
{
    if (backend == nullptr) {
        return;
    }
    // Describe, EXPAND (the one step that reads drawables), then hand the device's frame half the frame that says so -
    // there is no interleaving and no second wire: what this pass issues is a value, and a pass a host drives itself is
    // a frame with one pass in it (see singlePassFrame), which is the same shape the engine builds for a whole frame.
    // The target is what its default framebuffer row resolves to, and the EXECUTOR presents through it at the tail.
    PassDraw description;
    if (!describe(scene, description)) {
        return;
    }
    // A PASS DRIVEN BY ITS OWNER PUBLISHES TOO: the picture it is about to hand the backend is the one this call just
    // posted (see Scene::publish), bound by the same step a frame's content half performs - inside an edit batch it is
    // the batch's starting picture, so a pass driven mid-batch draws what a frame would. The store lives until the
    // expansion below has collected, because the draw names a source the store owns.
    detail::PublicationStore publications;
    if (scene != nullptr) {
        const ScenePublishResult published = scene->publish();
        detail::bindSceneDraws(description, publications.sourceFor(*scene, published.snapshot));
    }
    detail::expandPass(description);
    backend->executor().executeFrame(singlePassFrame(this, /*order*/ 0, /*inputs*/ {}, std::move(description)), present_to);
}

VN_GRAPHICS_NS_END
