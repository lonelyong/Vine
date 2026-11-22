#include <vine/graphics/FullscreenPass.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(FullscreenPass, vn::graphics::RenderPass);

FullscreenPass::FullscreenPass()
  : RenderPass(DrawKind::Fullscreen)
{
    // Nothing to state: "draws through a program over its resolved inputs, clears nothing" is what the KIND means
    // (see RenderPass::isClearEnabled / shouldClearDepth), so a full-screen pass cannot be built that halves it.
}

FullscreenPass::~FullscreenPass() = default;

raw_ptr<RenderTarget> FullscreenPass::sourceTarget() const
{
    return source_;
}

raw_ptr<ShaderProgram> FullscreenPass::program() const
{
    return desc_.program.get();
}

void FullscreenPass::resolveInputTextures(std::span<const ImageSource> inputs)
{
    source_ = nullptr;
    for (const ImageSource& input : inputs) {
        if (input.isProduced() && input.target != nullptr) {
            source_ = input.target;
            break;
        }
    }
}

bool FullscreenPass::describe(raw_ptr<Scene> scene, PassDraw& out)
{
    if (source_ == nullptr) {
        // Nothing was resolved to sample: this pass issues nothing at all, not even a target - which is what
        // keeps a full-screen pass whose input never resolved out of the frame entirely (the engine reports it).
        return false;
    }
    // A full-screen pass is content-agnostic, so a null scene is not its "nothing to issue" - the gate above is. The
    // output target, the sub-viewport and the clear are the inherited ones (one mapping for both kinds of pass).
    (void)scene;
    fillPassFacts(out);
    if (desc_.program == nullptr || camera() == nullptr) {
        // Nothing to draw with, or no view to build: the ENGINE reports both while it compiles the frame's
        // declarations (RenderEngine::collectPlanProblems), so this path stays silent and draws nothing.
        return true;
    }
    DrawPacket draw;
    draw.kind        = DrawKind::Fullscreen;
    draw.view.camera = CameraSnapshot::of(*camera());
    draw.source      = source_;
    draw.program     = desc_.program;
    // Its text as a value, taken with the pointer (see DrawPacket::program_description): the recorder reads the
    // value, so a program edited while a frame is in flight keeps the text it was described with.
    draw.program_description = desc_.program != nullptr ? desc_.program->description() : ShaderProgramDescriptionPtr{};
    // Forward the content scene's lights so the backend can push them to the
    // fullscreen fragment program (mirrors how scene passes feed their lights).
    // A pass with no scene announces NONE rather than an empty set: a full-screen pass is content-agnostic, and
    // "there is no scene" is not a statement about the lighting.
    if (scene != nullptr) {
        // THE SCENE'S ONE SHARED SET (see LightSet): a full-screen pass and a scene pass of the same frame hold the same
        // value, so a light edited while the frame records cannot change what it lights and nothing is copied per pass.
        draw.light_values = scene->lightSet();
    }
    out.packets.push_back(std::move(draw));
    return true;
}

VN_GRAPHICS_NS_END
