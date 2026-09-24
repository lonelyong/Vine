#pragma once
#include "graphics_global.hpp"

#include <optional>

#include "Group.hpp"
// Included rather than forward-declared: this class HOLDS an intrusive_ptr<Material>, so every translation
// unit that destroys a StateNode instantiates the reference-count hooks and needs the complete type.
#include "Material.hpp"

VN_GRAPHICS_NS_BEGIN

class ShaderProgram;
using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief Depth comparison function used when the depth test is enabled.
 */
enum class CompareOp
{
    Never,       ///< Depth never passes.
    Less,        ///< Pass when the fragment is closer than the stored depth.
    Equal,       ///< Pass when the depths are equal.
    LessEqual,   ///< Pass when the fragment is closer or equal.
    Greater,     ///< Pass when the fragment is farther.
    NotEqual,    ///< Pass when the depths differ.
    GreaterEqual, ///< Pass when the fragment is farther or equal.
    Always,      ///< Depth always passes.
};

/**
 * @brief Face culling mode for a StateNode subtree.
 *
 * WHICH FACE IS FRONT IS THE SDK'S RULE, NOT THE GPU'S: a triangle is front-facing when its vertices
 * appear counter-clockwise to the camera in a right-handed world (the SDK's view looks down -Z, see
 * Camera), so a host can author geometry against one convention and get the same picture on every
 * backend. A backend is free to render with another clip convention — the vsg backend flips Y for
 * Vulkan, where the front face is defined in framebuffer coordinates — and is responsible for mapping
 * this rule onto its own front face. A backend that got the mapping wrong culls the faces it was asked
 * to keep, which reads as an object disappearing with no diagnostic (the risk is recorded as UB-14 in
 * the vsg backend's notes).
 */
enum class CullMode
{
    None,  ///< No faces are culled (two-sided rendering).
    Front, ///< Counter-clockwise (front-facing) triangles are culled.
    Back,  ///< Clockwise (back-facing) triangles are culled.
};

/**
 * @brief Polygon rasterisation mode for a StateNode subtree.
 */
enum class PolygonMode
{
    Fill,  ///< Filled polygons (the default).
    Line,  ///< Wireframe outlines.
    Point, ///< Points at the polygon vertices.
};

/**
 * @brief Primitive topology of a drawable's vertex stream.
 *
 * Topology decides how vertices are assembled into primitives. It is a render
 * state item, distinct from PolygonMode (which controls how triangles are
 * rasterised): the same vertex data can be drawn filled, as a wireframe
 * (PolygonMode::Line) or as points (PolygonMode::Point) without touching the
 * geometry, and point clouds use Topology::Points.
 */
enum class Topology
{
    Triangles,  ///< Triangle list (the default).
    Points,     ///< Unconnected points (e.g. point clouds).
    Lines,      ///< Line list.
};

/**
 * @brief Blend factors used when blending is enabled.
 */
enum class BlendFactor
{
    Zero,               ///< 0.
    One,                ///< 1.
    SrcAlpha,           ///< Source alpha.
    OneMinusSrcAlpha,   ///< 1 - source alpha.
    DstAlpha,           ///< Destination alpha.
    OneMinusDstAlpha,   ///< 1 - destination alpha.
    SrcColor,           ///< Source color.
    OneMinusSrcColor,   ///< 1 - source color.
    DstColor,           ///< Destination color.
    OneMinusDstColor,   ///< 1 - destination color.
};

/**
 * @brief Concrete depth-test state (defaults applied).
 */
struct VN_GRAPHICS_API DepthState
{
    bool      test    = true;                ///< Depth test enabled.
    bool      write   = true;                ///< Depth writes enabled.
    CompareOp compare = CompareOp::Less;      ///< Depth comparison function.
};

/** @brief Returns whether two depth states are equal. */
inline bool operator==(const DepthState& lhs, const DepthState& rhs)
{
    return lhs.test == rhs.test && lhs.write == rhs.write && lhs.compare == rhs.compare;
}

/** @brief Returns whether two depth states differ. */
inline bool operator!=(const DepthState& lhs, const DepthState& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief Concrete blend state (defaults applied).
 */
struct VN_GRAPHICS_API BlendState
{
    bool        enabled = false;                           ///< Blending enabled.
    BlendFactor src     = BlendFactor::SrcAlpha;           ///< Source blend factor.
    BlendFactor dst     = BlendFactor::OneMinusSrcAlpha;   ///< Destination blend factor.
};

/** @brief Returns whether two blend states are equal. */
inline bool operator==(const BlendState& lhs, const BlendState& rhs)
{
    return lhs.enabled == rhs.enabled && lhs.src == rhs.src && lhs.dst == rhs.dst;
}

/** @brief Returns whether two blend states differ. */
inline bool operator!=(const BlendState& lhs, const BlendState& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief Optional render-state contributions declared by a StateNode.
 *
 * Every item is optional: an unset item means "inherit from the enclosing
 * StateNode". Only the items a node sets take part in the fold towards a
 * leaf's effective state.
 */
struct VN_GRAPHICS_API RenderState
{
    std::optional<DepthState>  depth;        ///< Depth test/write/compare override.
    std::optional<CullMode>    cullMode;     ///< Face culling override.
    std::optional<BlendState>  blend;        ///< Blending override.
    std::optional<PolygonMode> polygonMode;  ///< Polygon rasterisation override.
    std::optional<Topology>    topology;     ///< Primitive topology override.

    /** @brief Returns whether no item is set. */
    bool empty() const
    {
        return !depth && !cullMode && !blend && !polygonMode && !topology;
    }

    /** @brief Overlays another state block onto this one.
     *
     * Every item set in @p other replaces the corresponding item of this
     * block; items unset in @p other are left untouched.
     *
     * @param other Source block to overlay.
     */
    void merge(const RenderState& other);
};

/** @brief Returns whether two optional state blocks are equal. */
inline bool operator==(const RenderState& lhs, const RenderState& rhs)
{
    return lhs.depth == rhs.depth && lhs.cullMode == rhs.cullMode &&
           lhs.blend == rhs.blend && lhs.polygonMode == rhs.polygonMode &&
           lhs.topology == rhs.topology;
}

/** @brief Returns whether two optional state blocks differ. */
inline bool operator!=(const RenderState& lhs, const RenderState& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief Concrete render state after defaults are applied.
 *
 * The resolved state is what a backend uses to select pipeline variants:
 * StateNodes contribute overrides; everything they do not set falls back to
 * the defaults below (which match the backend's current default pipeline
 * state, so a scene without StateNodes resolves to today's behaviour).
 */
struct VN_GRAPHICS_API ResolvedRenderState
{
    DepthState   depth;                        ///< Depth state (defaults: test/write on, Less).
    CullMode     cullMode = CullMode::None;    ///< Face culling (default: none).
    BlendState   blend;                        ///< Blending (default: disabled).
    PolygonMode  polygonMode = PolygonMode::Fill;  ///< Polygon mode (default: fill).
    Topology     topology = Topology::Triangles;   ///< Primitive topology (default: triangles).
};

/** @brief Returns whether two resolved states are equal. */
inline bool operator==(const ResolvedRenderState& lhs, const ResolvedRenderState& rhs)
{
    return lhs.depth == rhs.depth && lhs.cullMode == rhs.cullMode &&
           lhs.blend == rhs.blend && lhs.polygonMode == rhs.polygonMode &&
           lhs.topology == rhs.topology;
}

/** @brief Returns whether two resolved states differ. */
inline bool operator!=(const ResolvedRenderState& lhs, const ResolvedRenderState& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief Scene-graph node applying render state to its subtree.
 *
 * StateNode is a Node that contributes optional render-state items (depth,
 * culling, blending, polygon mode, topology) to every drawable in its subtree. Items it
 * does not set are inherited from enclosing StateNodes; a deeper node
 * overrides a shallower one for the same item. The effective state of a leaf
 * is the fold of every StateNode from the scene root to the leaf
 * (collectRenderState) with defaults applied (resolveRenderState), and
 * participates in the backend's pipeline-variant key.
 *
 * State deliberately lives on these nodes rather than per drawable, so a
 * batch of geometry sharing one StateNode also shares one pipeline variant.
 * The model mirrors vsg::StateGroup / the OpenGL state stack and is
 * backend-agnostic.
 */
class VN_GRAPHICS_API StateNode : public Group {
    VN_OBJECT_META_DECL;

  public:
    StateNode();
    ~StateNode();

  public:
    /** @brief Sets the depth state (test/write/compare) for this subtree. */
    void setDepth(const DepthState& state);

    /** @brief Clears the depth override so enclosing state is inherited. */
    void clearDepth();

    /** @brief Gets the depth override, or nullopt when unset. */
    std::optional<DepthState> depth() const;

    /** @brief Sets the face culling mode for this subtree. */
    void setCullMode(CullMode mode);

    /** @brief Clears the culling override so enclosing state is inherited. */
    void clearCullMode();

    /** @brief Gets the culling override, or nullopt when unset. */
    std::optional<CullMode> cullMode() const;

    /** @brief Sets the blend state for this subtree. */
    void setBlend(const BlendState& state);

    /** @brief Clears the blend override so enclosing state is inherited. */
    void clearBlend();

    /** @brief Gets the blend override, or nullopt when unset. */
    std::optional<BlendState> blend() const;

    /** @brief Sets the polygon rasterisation mode for this subtree. */
    void setPolygonMode(PolygonMode mode);

    /** @brief Clears the polygon-mode override so enclosing state is inherited. */
    void clearPolygonMode();

    /** @brief Gets the polygon-mode override, or nullopt when unset. */
    std::optional<PolygonMode> polygonMode() const;

    /** @brief Sets the primitive topology for this subtree. */
    void setTopology(Topology topology);

    /** @brief Clears the topology override so enclosing state is inherited. */
    void clearTopology();

    /** @brief Gets the topology override, or nullopt when unset. */
    std::optional<Topology> topology() const;

    /** @brief Sets a custom shader program for this subtree.
     *
     * Program resolution (graphics-shader.md): a leaf Geometry's own program
     * wins; otherwise the nearest ancestor StateNode program applies. This is
     * separate from the render-state items above: a program replaces the
     * shading, not a pipeline-state slot.
     *
     * @param program Program, or nullptr to clear.
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Clears this node's program override so it is inherited. */
    void clearProgram();

    /** @brief Gets this node's program override, or null when unset. */
    raw_ptr<ShaderProgram> program() const;

    /** @brief Sets a material for this subtree.
     *
     * Material resolution follows the PROGRAM's rule, not the render-state one: a leaf Geometry's own
     * material wins, otherwise the NEAREST ancestor StateNode material applies, otherwise the engine
     * default. A material is single-valued and pointer-like, exactly like a program — which is why it is
     * not one of the optional items of the state block above: those are pipeline-state slots, whereas a
     * material reaches the backend as the command's material and shades through a descriptor.
     *
     * The point of it is that a subtree shading with one material does not have to be set per Geometry.
     *
     * @param material Material, or nullptr to clear.
     */
    void setMaterial(intrusive_ptr<Material> material);

    /** @brief Clears this node's material override so it is inherited. */
    void clearMaterial();

    /** @brief Gets this node's material override, or null when unset (the enclosing material applies). */
    raw_ptr<Material> material() const;

    /** @brief Gets this node's whole optional state block. */
    const RenderState& renderState() const;

    /** @brief Returns whether any state item is set on this node. */
    bool hasState() const;

    /** @brief Clears every state item set on this node. */
    void clearState();

  private:
    RenderState state_;
    intrusive_ptr<ShaderProgram> program_;
    intrusive_ptr<Material> material_;
};

using StateNodePtr = intrusive_ptr<StateNode>;

/**
 * @brief Collects the state contributions of every StateNode from the scene
 * root down to @p node.
 *
 * A deeper node overrides a shallower one for the same item; items nobody
 * sets stay unset. @p node itself contributes when it is a StateNode.
 *
 * @param node Leaf or intermediate node to fold state for.
 * @return The folded optional state block.
 */
VN_GRAPHICS_API RenderState collectRenderState(raw_ptr<const Node> node);

/**
 * @brief Applies default values to a folded optional state block.
 *
 * @param state Folded optional state (see collectRenderState).
 * @return Concrete state with defaults applied, usable as a pipeline-variant
 *         key component.
 */
VN_GRAPHICS_API ResolvedRenderState resolveRenderState(const RenderState& state);

/**
 * @brief Computes the effective render state for a node in one step.
 *
 * Equivalent to resolveRenderState(collectRenderState(node)).
 *
 * @param node Leaf or intermediate node to resolve state for.
 * @return The node's effective (resolved) render state.
 */
VN_GRAPHICS_API ResolvedRenderState effectiveRenderState(raw_ptr<const Node> node);

/**
 * @brief Resolves the effective shader program for a leaf geometry.
 *
 * A Geometry with its own program wins; otherwise the nearest ancestor
 * StateNode program applies; otherwise the engine default (null) is used.
 *
 * @param node Leaf (Geometry) or intermediate node to resolve for.
 * @return Effective program, or null for the engine default.
 */
VN_GRAPHICS_API ShaderProgramPtr effectiveProgram(raw_ptr<const Node> node);

/**
 * @brief Resolves the material a node's Geometry renders with.
 *
 * The same rule as effectiveProgram(): a leaf Geometry's own material wins, otherwise the nearest ancestor
 * StateNode material applies, otherwise null (the engine default). Nearest, not literal — the first
 * override found walking up the parents ends the search, because a material replaces the shading wholesale
 * rather than overriding one item of it.
 *
 * @param node Node to resolve for (typically a Geometry).
 * @return The material to render with, or null when neither the leaf nor any ancestor sets one.
 */
VN_GRAPHICS_API MaterialPtr effectiveMaterial(raw_ptr<const Node> node);

VN_GRAPHICS_NS_END
