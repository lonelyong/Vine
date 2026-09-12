#include <vine/graphics/StateNode.hpp>

#include <vector>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderProgram.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(StateNode, Group);

StateNode::StateNode() = default;

StateNode::~StateNode() = default;

void StateNode::setDepth(const DepthState& state)
{
    state_.depth = state;
}

void StateNode::clearDepth()
{
    state_.depth.reset();
}

std::optional<DepthState> StateNode::depth() const
{
    return state_.depth;
}

void StateNode::setCullMode(CullMode mode)
{
    state_.cullMode = mode;
}

void StateNode::clearCullMode()
{
    state_.cullMode.reset();
}

std::optional<CullMode> StateNode::cullMode() const
{
    return state_.cullMode;
}

void StateNode::setBlend(const BlendState& state)
{
    state_.blend = state;
}

void StateNode::clearBlend()
{
    state_.blend.reset();
}

std::optional<BlendState> StateNode::blend() const
{
    return state_.blend;
}

void StateNode::setPolygonMode(PolygonMode mode)
{
    state_.polygonMode = mode;
}

void StateNode::clearPolygonMode()
{
    state_.polygonMode.reset();
}

std::optional<PolygonMode> StateNode::polygonMode() const
{
    return state_.polygonMode;
}

void StateNode::setTopology(Topology topology)
{
    state_.topology = topology;
}

void StateNode::clearTopology()
{
    state_.topology.reset();
}

std::optional<Topology> StateNode::topology() const
{
    return state_.topology;
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
    state_ = RenderState();
}

void StateNode::setProgram(intrusive_ptr<ShaderProgram> program)
{
    program_ = std::move(program);
}

void StateNode::clearProgram()
{
    program_.reset();
}

raw_ptr<ShaderProgram> StateNode::program() const
{
    return program_.get();
}

void StateNode::setMaterial(intrusive_ptr<Material> material)
{
    material_ = std::move(material);
}

void StateNode::clearMaterial()
{
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
    if (other.topology) {
        topology = other.topology;
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
    if (state.topology) {
        resolved.topology = *state.topology;
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
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
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
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
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

V_GRAPHICS_NS_END
