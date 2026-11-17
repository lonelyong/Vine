#include <vine/graphics/RenderPass.hpp>

#include <utility>

#include <vine/Color.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
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

} // namespace

VN_OBJECT_META_IMPL(RenderPass, vn::Object);

RenderPass::RenderPass()
  : RenderPass(PassKind::Scene)
{}

RenderPass::RenderPass(PassKind kind)
{
    desc_.kind = kind;
}

RenderPass::~RenderPass() = default;

void RenderPass::adoptDesc(PassDesc desc)
{
    // The whole declaration, not a merge: whatever this pass stated before is gone, which is what makes "the plan
    // decided this shape" one assignment (see detail::PassNode::adopt).
    desc_ = std::move(desc);
    // A resolved transient belonged to the declaration that asked for it; a new declaration needs a new resolution.
    resolved_target_.reset();
    // A STATED view is applied to the declared target; an absent one is not a statement about the target, so
    // re-stating a declaration never un-says another pass' edge (which is what the engine relies on when it
    // refuses a conflicting view and adopts the rest). An INVALID view is the stated form of "no borrowed depth"
    // and clears the mechanism it set (the old setDepthView(DepthView{}) contract).
    if (desc_.target.target != nullptr && desc_.depth_view.has_value()) {
        const DepthView view = depthView();
        if (view.layer() == 0) {
            desc_.target.target->shareDepth(view);
        }
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
    state.pointSize   = desc_.point_size;
    state.lineWidth   = desc_.line_width;
    return state;
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
    out.target           = renderTarget();
    out.viewport         = desc_.viewport;
    out.depth_range      = desc_.depth_range;
    out.clear            = clearPolicy();
    out.program_override = desc_.program_override;
    // Its text as a value, taken with the pointer: the recorder applies THIS, so an override edited while a frame is in
    // flight keeps the text the frame described itself with (see PassDraw::program_override_description).
    out.program_override_description =
        desc_.program_override != nullptr ? desc_.program_override->description() : ShaderProgramDescriptionPtr{};
    // This pass' state for the content it draws - its depth policy and its rasterisation override, in one
    // announcement: what a backend needs to resolve each command's FINAL state
    // (StateResolution.hpp's resolveRenderState).
    out.state            = stateOverrides();
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
    draw.kind = DrawDescription::Kind::Scene;
    // THE VIEW, AS A VALUE (see View). Taking it here is what lets the frame's expansion collect on another
    // thread, and what keeps this frame's culling and shading on one camera state.
    draw.view.camera = CameraSnapshot::of(*desc_.camera);
    // WHOSE PUBLICATION ANSWERS THE VIEW is bound at the frame boundary, not here: a description states the VIEW,
    // and the engine points it at the scene's published snapshot (see detail::bindSceneDraws). A live scene never
    // travels in a frame value - one edit on the content thread could otherwise reach a frame that is already being
    // recorded.
    draw.view.layer_mask = desc_.layer_mask;
    // The viewport this pass lands in, in DEVICE pixels. A sub-viewport wins; otherwise an explicit target's
    // size; otherwise the surface size the engine/chain stated for a window pass. Collection needs it for
    // view-dependent features (AutoTransform screen-space scaling), not for the backend's own viewport.
    Viewport viewport = desc_.viewport.value_or(Viewport{});
    if (!viewport.isValid()) {
        if (raw_ptr<RenderTarget> target = renderTarget(); target != nullptr) {
            viewport = Viewport{ 0, 0, target->width(), target->height() };
        }
        else {
            viewport = draw_size_;
        }
    }
    draw.view.viewport = viewport;
    // The pass lights whatever content scene it renders: forward the scene's lights AS ONE SHARED VALUE (see
    // LightValue / LightSet), so editing a light while this frame records cannot change what it lights, and a frame
    // with several passes copies the set once (the scene owns it, not the pass). Announced even when there are none:
    // "there are no lights" restores a backend's default.
    draw.light_values = scene->lightSet();
    // The command list is NOT collected here: `draw.commands` stays null and the frame's expansion asks the source the
    // draw was bound to (see FrameExpand and detail::bindSceneDraws). A program override is a PASS fact, not a
    // per-command rewrite (see PassDraw::program_override): the backend resolves it per command exactly like the
    // pass' state.
    out.draws.push_back(std::move(draw));
    return true;
}

namespace detail
{

void PassNode::adopt(RenderPass& pass, PassDesc desc)
{
    pass.adoptDesc(std::move(desc));
}

void PassNode::resolveTarget(RenderPass& pass, intrusive_ptr<RenderTarget> target) noexcept
{
    pass.resolveTarget(std::move(target));
}

void PassNode::setDrawSize(RenderPass& pass, int width, int height) noexcept
{
    pass.setDrawSize(width, height);
}

} // namespace detail

VN_GRAPHICS_NS_END
