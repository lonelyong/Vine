#include <vine/graphics/Node.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(Node, vine::Object);

Node::Node() = default;

Node::~Node() = default;

String Node::name() const
{
    return name_;
}

void Node::setName(const String& name)
{
    name_ = name;
}

bool Node::isVisible() const
{
    return visible_;
}

void Node::setVisible(bool visible)
{
    visible_ = visible;
}

float Node::opacity() const
{
    return opacity_;
}

void Node::setOpacity(float opacity)
{
    opacity_ = opacity;
}

raw_ptr<Node> Node::parent() const
{
    return parent_;
}

Aabbd Node::boundingBox() const
{
    // Base Node is neither a container nor a leaf renderable, so it has no
    // extent of its own; concrete kinds (Group/Geometry) override this.
    return Aabbd::empty();
}

Mat4d Node::worldMatrix() const
{
    // Fold the local matrices from the top of the chain down to this node.
    // Non-transform nodes contribute the identity, so only enclosing
    // MatrixTransforms (and this node itself when it is one) affect the
    // result.
    //
    // Walked bottom-up with a left-multiply per level: the previous recursive
    // form recomputed the whole ancestor chain at every level, i.e. O(depth^2)
    // matrix products for a chain of depth d, on a call every leaf makes once
    // per pass. This is O(depth) and produces the identical matrix.
    Mat4d       world = localTransformMatrix();
    const Node* p     = parent_;
    while (p != nullptr) {
        world = p->localTransformMatrix() * world;
        p     = p->parent_;
    }
    return world;
}

Mat4d Node::localTransformMatrix() const
{
    return Mat4d();
}

V_GRAPHICS_NS_END
