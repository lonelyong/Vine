#include <vine/graphics/RenderPass.hpp>

#include <memory>
#include <utility>

#include <vine/Color.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

namespace
{
/// @brief The colour attachment 0 clears to when the declaration states none.
///
/// The value the main WINDOW pass was written against (an opaque dark grey), STATED IN THE SPACE IT IS SEEN IN and
/// decoded once: a host or a designer picks #333333, and the engine needs the linear light that produces it. Writing
/// the linear number directly (0.0331) would hide where it came from; keeping the bytes and dividing by 255 at the
/// backend would be a different colour on every sRGB target (the defect this states away).
const Colorf kDefaultClearColor = Color(51, 51, 51, 255).toLinear();

/// @brief The colour an unstated attachment past the first clears to: "nothing was drawn here".
constexpr Colorf kUnstatedClearColor{ 0.0F, 0.0F, 0.0F, 0.0F };
}  // namespace

VN_OBJECT_META_IMPL(RenderPass, vn::Object);

RenderPass::RenderPass()
    : RenderPass(PassKind::Scene)
{
}

RenderPass::RenderPass(PassKind kind)
{
    desc_.kind = kind;
}

RenderPass::~RenderPass() = default;

void RenderPass::setDesc(PassDesc desc)
{
    // The whole declaration, not a merge: whatever this pass stated before is gone, which is what makes "adopt this
    // descriptor" and "build the pass by hand" the same object (see RenderEngine::addPass).
    desc_ = std::move(desc);
    // A STATED view is applied to the declared target; an absent (or invalid) one is not a statement about the
    // target - the same rule setRenderTarget follows - so re-stating a declaration never un-says another pass' edge
    // (which is what the engine relies on when it refuses a conflicting view and adopts the rest).
    const DepthView view = depthView();
    if (desc_.writes != nullptr && view.isValid() && view.layer() == 0) {
        desc_.writes->shareDepth(view);
    }
}

void RenderPass::setRenderTarget(intrusive_ptr<RenderTarget> target)
{
    desc_.writes = std::move(target);
    // The edge follows the target: a pass that stated a depth view keeps drawing against it when it is moved. A pass
    // that stated NONE does not un-say what another pass set (attaching a pass to a target is not a statement about
    // the target's depth) - hence the isValid() gate.
    const DepthView view = depthView();
    if (desc_.writes != nullptr && view.isValid() && view.layer() == 0) {
        desc_.writes->shareDepth(view);
    }
}

void RenderPass::setDepthView(DepthView view)
{
    desc_.depth_view = std::move(view);
    // A layered view has no mechanism until layered targets land (see DepthView): it stays a declaration and is
    // applied to nothing, so a frame never silently draws against the whole image instead of the layer.
    // An INVALID view is a statement of its own ("no borrowed depth") and clears what an earlier declaration set,
    // which is why there is no isValid() gate here.
    const DepthView stated = depthView();
    if (desc_.writes != nullptr && stated.layer() == 0) {
        desc_.writes->shareDepth(stated);
    }
}

Colorf RenderPass::clearColor(int attachment) const
{
    if (attachment <= 0) {
        return desc_.clear_color.value_or(kDefaultClearColor);
    }
    const auto index = static_cast<std::size_t>(attachment - 1);
    return index < desc_.clear_extra_colors.size() ? desc_.clear_extra_colors[index] : kUnstatedClearColor;
}

void RenderPass::setClearColor(const Colorf& color, int attachment)
{
    if (attachment < 0) {
        return;
    }
    if (attachment == 0) {
        desc_.clear_color = color;
        return;
    }
    const auto index = static_cast<std::size_t>(attachment - 1);
    if (desc_.clear_extra_colors.size() <= index) {
        // Grow with transparent black, so the index the caller names is the index the backend clears (an unstated
        // attachment keeps meaning "nothing here").
        desc_.clear_extra_colors.resize(index + 1, kUnstatedClearColor);
    }
    desc_.clear_extra_colors[index] = color;
}

bool RenderPass::shouldClearDepth() const noexcept
{
    // The kind's own policy is the fallback: a Scene pass clears, a Screen pass composites over what is there.
    return desc_.clear_depth.value_or(desc_.kind != PassKind::Screen);
}

bool RenderPass::isClearEnabled() const noexcept
{
    return desc_.clear_enabled.value_or(desc_.kind != PassKind::Screen);
}

PassRenderState RenderPass::stateOverrides() const noexcept
{
    PassRenderState state;
    state.depth       = desc_.depth_mode;
    state.polygonMode = desc_.polygon_mode;
    return state;
}

void RenderPass::addInput(intrusive_ptr<ImageRef> image)
{
    if (image != nullptr) {
        desc_.inputs.emplace_back(std::move(image));
    }
}

void RenderPass::addInput(intrusive_ptr<Texture> texture)
{
    if (texture != nullptr) {
        desc_.inputs.emplace_back(std::move(texture));
    }
}

void RenderPass::addInput(intrusive_ptr<Texture> texture, const Sampler& sampler)
{
    if (texture != nullptr) {
        desc_.inputs.emplace_back(std::move(texture), sampler);
    }
}

void RenderPass::addInput(ImageInput input)
{
    if (input) {
        desc_.inputs.push_back(std::move(input));
    }
}

void RenderPass::clearInputs()
{
    // One statement, all three lists: the resolve order is images, then targets, then names, so clearing two of them
    // would leave the pass reading a stale third (see resolveRowInputs).
    desc_.inputs.clear();
    desc_.input_targets.clear();
    desc_.input_names.clear();
}

std::optional<ClearPolicy> RenderPass::clearPolicy() const
{
    // The ONE place "does this pass clear, and how" is answered: a pass that does not clear says nothing, so a backend
    // can tell "clears to this colour" from "draws over what is already there".
    if (!isClearEnabled()) {
        return std::nullopt;
    }
    // One value, one authority: the per-attachment colours travel WITH the single-colour spelling, so a backend reads
    // one policy rather than a list plus a fallback.
    ClearPolicy policy{ clearColor(0), shouldClearDepth() };
    policy.extra_colors = desc_.clear_extra_colors;
    return policy;
}

void RenderPass::fillPassFacts(PassDraw& out)
{
    out.target           = desc_.writes.get();
    out.viewport         = desc_.viewport;
    out.clear            = clearPolicy();
    out.program_override = desc_.program_override;
    // This pass' state for the content it draws - its depth policy and its rasterisation override, in one
    // announcement: what a backend needs to resolve each command's FINAL state (StateNode.hpp's resolveRenderState).
    out.state    = stateOverrides();
    out.draws.clear();
}

bool RenderPass::describe(raw_ptr<Scene> scene, PassDraw& out)
{
    if (scene == nullptr) {
        // A scene pass with no scene issues nothing AT ALL - not even a target: a pass that is not drawing must not
        // clear, stack or claim one.
        return false;
    }
    fillPassFacts(out);
    if (desc_.camera == nullptr) {
        // No view to build: the pass announces what it is and where, and issues no draw.
        return true;
    }
    DrawDescription draw;
    draw.kind   = DrawDescription::Kind::Scene;
    draw.camera = desc_.camera.get();
    // The shared list, not a copy of it: every pass of a frame draws the same commands, and a per-pass copy would cost
    // an intrusive_ptr increment per command plus a command-sized memcpy per pass per frame. What a pass may change is
    // its own VIEW of the list, so the copy happens below - and only for a pass that actually rewrites something.
    const std::shared_ptr<const std::vector<RenderCommand>> commands =
        scene->collectRenderCommands(desc_.camera.get(), desc_.layer_mask);
    // The pass lights whatever content scene it renders: forward the scene's lights so the backend can match this
    // pass's view lighting. Announced even when there are none: "there are no lights" restores a backend's default.
    std::vector<raw_ptr<const Light>> light_ptrs;
    light_ptrs.reserve(scene->lights().size());
    for (const auto& light : scene->lights()) {
        light_ptrs.push_back(light.get());
    }
    draw.lights = std::move(light_ptrs);
    // A program override is the one pass-level override that still rewrites commands: replacing the program is not
    // "another state item" but "draw this content with another program", and the rewrite is what keeps a backend's
    // per-command pipeline key valid. The rasterisation override is NOT rewritten: it travels with the pass' state
    // (stateOverrides()) and is resolved per command, in one place (resolveRenderState).
    // The command list is handed over AS IT WAS COLLECTED - shared, immutable, no copy. A program override is a PASS
    // fact, not a per-command rewrite (see PassDraw::program_override): the backend resolves it per command exactly
    // like the pass' state (PassDraw::state). Rewriting the commands here cost a whole extra list per frame per
    // overriding pass, which is why the rasterisation override gave up that shape first (M1).
    draw.commands = commands;
    out.draws.push_back(std::move(draw));
    return true;
}

void RenderPass::execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend, raw_ptr<PresentTarget> present_to)
{
    if (backend == nullptr) {
        return;
    }
    // Describe, then hand the device's frame half the frame that says so - there is no interleaving and no second
    // wire: what this pass issues is a value, and a pass a host drives itself is a frame with one pass in it (see
    // singlePassFrame), which is the same shape the engine builds for a whole frame. The target is what its default
    // framebuffer row resolves to, and the EXECUTOR presents through it at the frame's tail.
    PassDraw description;
    if (!describe(scene, description)) {
        return;
    }
    backend->executor().executeFrame(singlePassFrame(this, /*order*/ 0, /*inputs*/ {}, std::move(description)),
                                     present_to);
}

VN_GRAPHICS_NS_END
