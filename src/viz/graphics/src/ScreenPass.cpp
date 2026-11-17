#include <vine/graphics/ScreenPass.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(ScreenPass, vn::graphics::RenderPass);

ScreenPass::ScreenPass()
  : RenderPass(PassKind::Screen)
{
    // Nothing to state: "draws through a program over its resolved inputs, clears nothing" is what the KIND means
    // (see RenderPass::isClearEnabled / shouldClearDepth), so a screen pass cannot be built that halves it.
}

ScreenPass::~ScreenPass() = default;

raw_ptr<RenderTarget> ScreenPass::sourceTarget() const
{
    return source_;
}

raw_ptr<ShaderProgram> ScreenPass::program() const
{
    return desc_.program.get();
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

bool ScreenPass::describe(raw_ptr<Scene> scene, PassDraw& out)
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
    if (desc_.program == nullptr || camera() == nullptr) {
        // Nothing to draw with, or no view to build: the ENGINE reports both while it compiles the frame's
        // declarations (RenderEngine::collectPlanProblems), so this path stays silent and draws nothing.
        return true;
    }
    DrawDescription draw;
    draw.kind        = DrawDescription::Kind::Screen;
    draw.view.camera = CameraSnapshot::of(*camera());
    draw.source      = source_;
    draw.program     = desc_.program;
    // Its text as a value, taken with the pointer (see DrawDescription::program_description): the recorder reads the
    // value, so a program edited while a frame is in flight keeps the text it was described with.
    draw.program_description = desc_.program != nullptr ? desc_.program->description() : ShaderProgramDescriptionPtr{};
    // Forward the content scene's lights so the backend can push them to the
    // fullscreen fragment program (mirrors how scene passes feed their lights).
    // A pass with no scene announces NONE rather than an empty set: a screen pass is content-agnostic, and
    // "there is no scene" is not a statement about the lighting.
    if (scene != nullptr) {
        // THE SCENE'S ONE SHARED SET (see LightSet): a screen pass and a scene pass of the same frame hold the same
        // value, so a light edited while the frame records cannot change what it lights and nothing is copied per pass.
        draw.light_values = scene->lightSet();
    }
    out.draws.push_back(std::move(draw));
    return true;
}

VN_GRAPHICS_NS_END
