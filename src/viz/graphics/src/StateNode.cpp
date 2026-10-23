#include <vine/graphics/StateNode.hpp>

#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(StateNode, Group);

StateNode::StateNode() = default;

StateNode::~StateNode() = default;

void StateNode::setDepth(const DepthState& state)
{
    announceContentChange();
    state_.depth = state;
}

void StateNode::clearDepth()
{
    announceContentChange();
    state_.depth.reset();
}

std::optional<DepthState> StateNode::depth() const
{
    return state_.depth;
}

void StateNode::setCullMode(CullMode mode)
{
    announceContentChange();
    state_.cullMode = mode;
}

void StateNode::clearCullMode()
{
    announceContentChange();
    state_.cullMode.reset();
}

std::optional<CullMode> StateNode::cullMode() const
{
    return state_.cullMode;
}

void StateNode::setBlend(const BlendState& state)
{
    announceContentChange();
    state_.blend = state;
}

void StateNode::clearBlend()
{
    announceContentChange();
    state_.blend.reset();
}

std::optional<BlendState> StateNode::blend() const
{
    return state_.blend;
}

void StateNode::setPolygonMode(PolygonMode mode)
{
    announceContentChange();
    state_.polygonMode = mode;
}

void StateNode::clearPolygonMode()
{
    announceContentChange();
    state_.polygonMode.reset();
}

std::optional<PolygonMode> StateNode::polygonMode() const
{
    return state_.polygonMode;
}

const RenderState& StateNode::renderState() const
{
    return state_;
}

bool StateNode::hasState() const
{
    return !state_.empty();
}

void StateNode::clearState()
{
    announceContentChange();
    state_ = RenderState();
}

void StateNode::setProgram(intrusive_ptr<ShaderProgram> program)
{
    announceContentChange();
    program_ = std::move(program);
}

void StateNode::clearProgram()
{
    announceContentChange();
    program_.reset();
}

raw_ptr<ShaderProgram> StateNode::program() const
{
    return program_.get();
}

void StateNode::setMaterial(intrusive_ptr<Material> material)
{
    announceContentChange();
    material_ = std::move(material);
}

void StateNode::clearMaterial()
{
    announceContentChange();
    material_.reset();
}

raw_ptr<Material> StateNode::material() const
{
    return material_.get();
}

void RenderState::merge(const RenderState& other)
{
    if (other.depth) {
        depth = other.depth;
    }
    if (other.cullMode) {
        cullMode = other.cullMode;
    }
    if (other.blend) {
        blend = other.blend;
    }
    if (other.polygonMode) {
        polygonMode = other.polygonMode;
    }
}

RenderState collectRenderState(raw_ptr<const Node> node)
{
    RenderState result;

    // Walk from the leaf up to the root, then apply root-to-leaf so that a
    // deeper node overrides a shallower one for the same item.
    std::vector<const Node*> chain;
    for (const Node* current = node; current != nullptr; current = current->parent()) {
        chain.push_back(current);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const auto* state_node = dynamic_cast<const StateNode*>(*it)) {
            result.merge(state_node->renderState());
        }
    }
    return result;
}

ResolvedRenderState resolveRenderState(const RenderState& state)
{
    ResolvedRenderState resolved;
    if (state.depth) {
        resolved.depth = *state.depth;
    }
    if (state.cullMode) {
        resolved.cullMode = *state.cullMode;
    }
    if (state.blend) {
        resolved.blend = *state.blend;
    }
    if (state.polygonMode) {
        resolved.polygonMode = *state.polygonMode;
    }
    return resolved;
}

ResolvedCommandState resolveRenderState(const RenderState& authored, const PassRenderState& pass,
                                        Topology data_topology)
{
    ResolvedCommandState resolved;

    // The content's word first, item by item: what a StateNode authored is kept (and says so), everything it did
    // not author is still the "nobody said" case the defaults below answer.
    if (authored.cullMode) {
        resolved.state.cullMode = *authored.cullMode;
        resolved.origin.cullMode = StateOrigin::Content;
    }
    if (authored.blend) {
        resolved.state.blend = *authored.blend;
        resolved.origin.blend = StateOrigin::Content;
    }
    if (authored.polygonMode) {
        resolved.state.polygonMode = *authored.polygonMode;
        resolved.origin.polygonMode = StateOrigin::Content;
    }
    // Topology: the DATA's own word (a primitive's topology) - no StateNode layer exists for it, because a
    // subtree cannot reinterpret another object's vertex/index data. The pass may override it below, which is
    // the "show me this scene as points" view switch.
    resolved.state.topology  = data_topology;
    resolved.origin.topology = StateOrigin::Data;

    // Depth: the pass states a POLICY, the content may state an intent. An authored depth wins (finer-grained
    // intent over the pass default); where the content authored none, the pass decides whether depth is used at
    // all - and the COMPARE stays whatever the content resolved to (the pass has no opinion about it, see
    // PassRenderState).
    if (authored.depth) {
        resolved.state.depth        = *authored.depth;
        resolved.origin.depth       = StateOrigin::Content;
    }
    else {
        resolved.state.depth.test    = pass.depth != DepthMode::Disabled;
        resolved.state.depth.write   = pass.depth == DepthMode::TestAndWrite;
        resolved.origin.depth        = StateOrigin::Pass;
    }

    // Rasterisation is an OVERRIDE, so it comes last and wins even over a content-authored mode: it is the
    // "show me the edges" switch, and a switch a content-authored Line could veto would never take the picture
    // back to Fill (see PassRenderState).
    if (pass.polygonMode) {
        resolved.state.polygonMode = *pass.polygonMode;
        resolved.origin.polygonMode = StateOrigin::Pass;
    }

    // Same rule for the more drastic view switch: a pass that says "assemble everything as points" wins over
    // what the data states (and the pipeline is then compiled for that topology - see core::PipelineKey).
    if (pass.topology) {
        resolved.state.topology  = *pass.topology;
        resolved.origin.topology = StateOrigin::Pass;
    }

    return resolved;
}

ResolvedRenderState effectiveRenderState(raw_ptr<const Node> node)
{
    return resolveRenderState(collectRenderState(node));
}

ShaderProgramPtr effectiveProgram(raw_ptr<const Node> node)
{
    // A leaf Geometry's own program wins; otherwise the nearest ancestor
    // StateNode program applies (deeper beats shallower); otherwise the
    // engine default (null) is used.
    if (node == nullptr) {
        return ShaderProgramPtr();
    }
    // The LEAF's own word is the first PRIMITIVE's, then the GEOMETRY's default (see Primitive.hpp): this
    // helper answers "what would this node's single draw use", which is the question a host asks about a leaf
    // it built itself. A geometry whose draws state different programs has no single answer - a caller that
    // means a specific draw reads `RenderCommand::program`, which the collection already resolved per primitive.
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        if (const Primitive* draw = geometry->primitive(0); draw != nullptr && draw->program != nullptr) {
            return ShaderProgramPtr(draw->program);
        }
        if (raw_ptr<ShaderProgram> own = geometry->program()) {
            return ShaderProgramPtr(own);
        }
    }
    for (const Node* current = node->parent(); current != nullptr;
         current = current->parent()) {
        if (const auto* state_node = dynamic_cast<const StateNode*>(current)) {
            if (raw_ptr<ShaderProgram> ancestor = state_node->program()) {
                return ShaderProgramPtr(ancestor);
            }
        }
    }
    return ShaderProgramPtr();
}

MaterialPtr effectiveMaterial(raw_ptr<const Node> node)
{
    // A leaf Geometry's own material wins; otherwise the nearest ancestor StateNode material applies; otherwise
    // the engine default (null). Deliberately the SAME shape as effectiveProgram() above: both are
    // single-valued overrides that replace the shading, as opposed to the render-state items, which fold
    // item by item.
    if (node == nullptr) {
        return MaterialPtr();
    }
    // Same rule as effectiveProgram: the first primitive's own material, then the geometry's default.
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        if (const Primitive* draw = geometry->primitive(0); draw != nullptr && draw->material != nullptr) {
            return MaterialPtr(draw->material);
        }
        if (raw_ptr<Material> own = geometry->material()) {
            return MaterialPtr(own);
        }
    }
    for (const Node* current = node->parent(); current != nullptr; current = current->parent()) {
        if (const auto* state_node = dynamic_cast<const StateNode*>(current)) {
            if (raw_ptr<Material> ancestor = state_node->material()) {
                return MaterialPtr(ancestor);
            }
        }
    }
    return MaterialPtr();
}

VN_GRAPHICS_NS_END
