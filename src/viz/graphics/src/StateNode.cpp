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

void StateNode::setPolygonOffset(const PolygonOffset& offset)
{
    announceContentChange();
    state_.polygonOffset = offset;
}

void StateNode::clearPolygonOffset()
{
    announceContentChange();
    state_.polygonOffset.reset();
}

std::optional<PolygonOffset> StateNode::polygonOffset() const
{
    return state_.polygonOffset;
}

void StateNode::setPointSize(float size)
{
    announceContentChange();
    state_.pointSize = size;
}

void StateNode::clearPointSize()
{
    announceContentChange();
    state_.pointSize.reset();
}

std::optional<float> StateNode::pointSize() const
{
    return state_.pointSize;
}

void StateNode::setLineWidth(float width)
{
    announceContentChange();
    state_.lineWidth = width;
}

void StateNode::clearLineWidth()
{
    announceContentChange();
    state_.lineWidth.reset();
}

std::optional<float> StateNode::lineWidth() const
{
    return state_.lineWidth;
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

void StateNode::setMaterial(MaterialBasePtr material)
{
    announceContentChange();
    material_ = std::move(material);
}

void StateNode::clearMaterial()
{
    announceContentChange();
    material_.reset();
}

raw_ptr<MaterialBase> StateNode::material() const
{
    return material_.get();
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
    for (const Node* current = node->parent(); current != nullptr; current = current->parent()) {
        if (const auto* state_node = dynamic_cast<const StateNode*>(current)) {
            if (raw_ptr<ShaderProgram> ancestor = state_node->program()) {
                return ShaderProgramPtr(ancestor);
            }
        }
    }
    return ShaderProgramPtr();
}

MaterialBasePtr effectiveMaterial(raw_ptr<const Node> node)
{
    // A leaf Geometry's own material wins; otherwise the nearest ancestor StateNode material applies; otherwise
    // the engine default (null). Deliberately the SAME shape as effectiveProgram() above: both are
    // single-valued overrides that replace the shading, as opposed to the render-state items, which fold
    // item by item.
    if (node == nullptr) {
        return MaterialBasePtr();
    }
    // Same rule as effectiveProgram: the first primitive's own material, then the geometry's default.
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        if (const Primitive* draw = geometry->primitive(0); draw != nullptr && draw->material != nullptr) {
            return MaterialBasePtr(draw->material);
        }
        if (raw_ptr<MaterialBase> own = geometry->material()) {
            return MaterialBasePtr(own);
        }
    }
    for (const Node* current = node->parent(); current != nullptr; current = current->parent()) {
        if (const auto* state_node = dynamic_cast<const StateNode*>(current)) {
            if (raw_ptr<MaterialBase> ancestor = state_node->material()) {
                return MaterialBasePtr(ancestor);
            }
        }
    }
    return MaterialBasePtr();
}

VN_GRAPHICS_NS_END
