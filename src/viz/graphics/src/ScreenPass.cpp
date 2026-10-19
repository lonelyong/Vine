#include <vine/graphics/ScreenPass.hpp>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(ScreenPass, vn::graphics::RenderPass);

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
}

void ScreenPass::resolveInputTextures(std::span<const ImageSource> inputs)
{
    source_ = nullptr;
    for (const ImageSource& input : inputs) {
        if (input.isProduced() && input.target != nullptr) {
            source_ = input.target;
            break;
        }
    }
}

bool ScreenPass::describe(raw_ptr<Scene> scene, PassDescription& out)
{
    if (source_ == nullptr) {
        // Nothing was resolved to sample: this pass issues nothing at all, not even a target - which is what
        // keeps a screen pass whose input never resolved out of the frame entirely (the engine reports it).
        return false;
    }
    // A screen pass is content-agnostic, so a null scene is not its "nothing to issue" - the gate above is. The
    // output target, the sub-viewport and the clear are the inherited ones (one mapping for both kinds of pass).
    (void)scene;
    fillPassFacts(out);
    if (program_ == nullptr || camera() == nullptr) {
        // Nothing to draw with, or no view to build: the ENGINE reports both while it compiles the frame's
        // declarations (RenderEngine::collectPlanProblems), so this path stays silent and draws nothing.
        return true;
    }
    DrawDescription draw;
    draw.kind    = DrawDescription::Kind::Screen;
    draw.camera  = camera();
    draw.source  = source_;
    draw.program = program_;
    // Forward the content scene's lights so the backend can push them to the
    // fullscreen fragment program (mirrors how scene passes feed their lights).
    // A pass with no scene announces NONE rather than an empty list: a screen pass is content-agnostic, and
    // "there is no scene" is not a statement about the lighting.
    if (scene != nullptr) {
        std::vector<raw_ptr<const Light>> light_ptrs;
        light_ptrs.reserve(scene->lights().size());
        for (const auto& light : scene->lights()) {
            light_ptrs.push_back(light.get());
        }
        draw.lights = std::move(light_ptrs);
    }
    out.draws.push_back(std::move(draw));
    return true;
}

VN_GRAPHICS_NS_END
