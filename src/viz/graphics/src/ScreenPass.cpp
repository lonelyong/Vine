#include <vine/graphics/ScreenPass.hpp>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(ScreenPass, vine::graphics::RenderPass);

ScreenPass::ScreenPass()
{
    // A screen-space pass composites over previously rendered content:
    // clearing would wipe the main scene before the textured triangle draws.
    setClearEnabled(false);
    setShouldClearDepth(false);
}

ScreenPass::~ScreenPass() = default;

raw_ptr<RenderTarget> ScreenPass::sourceTarget() const
{
    return source_;
}

raw_ptr<ShaderProgram> ScreenPass::program() const
{
    return program_.get();
}

void ScreenPass::setProgram(intrusive_ptr<ShaderProgram> program)
{
    program_ = std::move(program);
    // A program is a wiring declaration here (the engine reports a ScreenPass that has none, because such
    // a pass draws nothing): the base class cannot see this setter, so the subclass announces it.
    bumpWiringRevision();
}

void ScreenPass::resolveInputTextures(const std::vector<raw_ptr<RenderTarget>>& inputs)
{
    source_ = nullptr;
    for (const raw_ptr<RenderTarget> input : inputs) {
        if (input != nullptr) {
            source_ = input;
            break;
        }
    }
}

void ScreenPass::execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend)
{
    if (backend == nullptr || source_ == nullptr) {
        return;
    }
    backend->setRenderTarget(renderTarget());
    if (hasViewport()) {
        const Viewport vp = viewport();
        backend->setViewport(vp.x, vp.y, vp.width, vp.height);
    }
    if (clearEnabled()) {
        backend->clear(clearColor(), shouldClearDepth());
    }
    if (program_ == nullptr || camera() == nullptr) {
        // Nothing to draw with, or no view to build: the ENGINE reports both at wiring time (see
        // RenderEngine::validateWiring), so this path stays silent and draws nothing.
        return;
    }
    // Forward the content scene's lights so the backend can push them to the
    // fullscreen fragment program (mirrors how scene passes feed their lights).
    if (scene != nullptr) {
        std::vector<raw_ptr<const Light>> light_ptrs;
        light_ptrs.reserve(scene->lights().size());
        for (const auto& light : scene->lights()) {
            light_ptrs.push_back(light.get());
        }
        backend->setLights(light_ptrs);
    }
    backend->drawScreenProgram(source_, program_.get(), camera());
}

V_GRAPHICS_NS_END
