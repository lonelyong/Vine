#pragma once
#include "graphics_global.hpp"

#include <optional>

#include "PolygonMode.hpp"

VN_GRAPHICS_NS_BEGIN

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
 * @details
 * - WHICH FACE IS FRONT IS THE SDK'S RULE, NOT THE GPU'S: a triangle is front-facing when its vertices
 *   appear counter-clockwise to the camera in a right-handed world (the SDK's view looks down -Z, see
 *   Camera), so a host authors geometry against one convention and gets the same picture on every backend.
 * - A backend rendering under another clip convention (the vsg backend flips Y for Vulkan, where the front
 *   face is defined in framebuffer coordinates) maps this rule onto its own front face.
 *
 * @note
 * - A backend that got the mapping wrong culls the faces it was asked to keep: the object disappears with
 *   no diagnostic (recorded as UB-14 in the vsg backend's notes).
 */
enum class CullMode
{
    None,  ///< No faces are culled (two-sided rendering).
    Front, ///< Counter-clockwise (front-facing) triangles are culled.
    Back,  ///< Clockwise (back-facing) triangles are culled.
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
 *
 * @details
 * - `enabled` is the ENABLE, not a hint: false means the surface writes its fragment unchanged, even when a
 *   program writes an alpha below 1 (the rule three.js / Filament / Unity follow for an opaque material).
 * - TRANSPARENCY HAS ONE OTHER SOURCE, and it is merged where the two meet: a command whose effective
 *   opacity is below 1 (Node::setOpacity / Scene::setOpacity) is drawn with blending on and the pair
 *   below, because opacity is the engine's transparency channel (see RenderCommand::opacity). Content that
 *   wants per-pixel alpha from its own shader states the blend here instead of relying on the object's
 *   opacity.
 * - A backend whose target carries several colour attachments never blends them (they hold data, not a
 *   surface), and a full-screen draw writes its rectangle; both are target-shape rules, not this flag.
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
 * @brief Nudges the depth of a polygon towards the viewer or away from it before the depth test.
 *
 * @details
 * - THE ANSWER to coplanar surfaces: two faces at the same physical depth (a decal on a panel, a
 *   highlight over the face it marks, a shaded face and its own wireframe) fight for the depth buffer,
 *   and no geometric offset fixes that at every zoom level - the offset has to happen in the depth
 *   units the rasteriser uses, which is what this states.
 * - `factor` scales the polygon's slope and `units` is a constant in the depth buffer's own units. The
 *   usual pair is a small positive `units` with a slope-scaled `factor`, since a face seen edge-on has
 *   the largest depth error per pixel.
 * - The offset applies to the RASTERISED depth only: it does not move the vertex, change the bounding
 *   box, or affect picking, which is what makes "push this highlight 0.001 closer" a rendering detail
 *   rather than a change to the scene.
 *
 * @note
 * - {0, 0} is "no offset at all", and that is the default: a pipeline without an offset behaves exactly
 *   as it did before this type existed.
 */
struct VN_GRAPHICS_API PolygonOffset
{
    float factor = 0.0F;  ///< Depth offset per unit of polygon slope.
    float units  = 0.0F;  ///< Constant depth offset, in depth-buffer units.

    /** @brief Returns whether this offset does nothing (the default). */
    [[nodiscard]] bool isZero() const noexcept { return factor == 0.0F && units == 0.0F; }
};

/** @brief Returns whether two polygon offsets are equal. */
inline bool operator==(const PolygonOffset& lhs, const PolygonOffset& rhs)
{
    return lhs.factor == rhs.factor && lhs.units == rhs.units;
}

/** @brief Returns whether two polygon offsets differ. */
inline bool operator!=(const PolygonOffset& lhs, const PolygonOffset& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief Optional render-state contributions declared on the content side (a StateNode).
 *
 * Every item is optional: an unset item means "inherit from the enclosing
 * StateNode". Only the items a node sets take part in the fold towards a
 * leaf's effective state.
 *
 * @note
 * - This is the VOCABULARY only: what a content author may say. Which layer wins, and what an unset item
 *   becomes, is the priority table in StateResolution.hpp.
 */
struct VN_GRAPHICS_API RenderState
{
    std::optional<DepthState>    depth;          ///< Depth test/write/compare override.
    std::optional<CullMode>      cullMode;       ///< Face culling override.
    std::optional<BlendState>    blend;          ///< Blending override.
    std::optional<PolygonMode>   polygonMode;    ///< Polygon rasterisation override.
    std::optional<PolygonOffset> polygonOffset;  ///< Depth-bias override (coplanar surfaces).

    /**
     * @brief Pixel size of a point primitive (framebuffer pixels), the override for a subtree.
     *
     * @details
     * - This is the ONE item whose value ends up in the DRAW BLOCK rather than in the pipeline or its
     *   dynamic state: a point's size is a vertex-shader output (`gl_PointSize`), so changing it re-uploads
     *   one per-draw value and never recompiles a pipeline. It inherits like every other item and the pass
     *   may override it (a "show me this scene as points" view needs points it can see), but its consumer
     *   is `VineDrawBlock::params.y` - see StateResolution.hpp.
     * - It only does anything for POINT rasterisation (`Topology::Points`, or `PolygonMode::Point` on
     *   triangle data). Triangles and lines ignore it, and that is not an error: the value still says what
     *   it says, there is simply nothing to size.
     * - Values below 1.0 are raised to 1.0 (see resolveRenderState): a point is at least one sample, and
     *   "0" must not become a second way to hide geometry.
     */
    std::optional<float> pointSize;

    /**
     * @brief Width of rasterised LINE primitives, in framebuffer pixels (the override for a subtree).
     *
     * @details
     * - It applies to line primitives (`Topology::Lines`): axis lines, skeletons, trajectories, the ruler and
     *   leader lines a CAD drawing is made of. It does NOT widen `PolygonMode::Line` (a wireframe's edges are
     *   rasterised as lines by the polygon stage, and the API's line width does not reach them - the same rule
     *   OpenGL states). A thick WIREFRAME is a different technique (expanded quads or a shader), not this value.
     * - Unlike the point size it IS API state, delivered as dynamic state: one pipeline serves every width, so
     *   changing it recompiles nothing. The device's `lineWidthRange` is the ceiling (see
     *   Capabilities::max_line_width) and values below 1.0 are raised to 1.0.
     */
    std::optional<float> lineWidth;

    /** @brief Returns whether no item is set.
     *
     * @return true when every item is unset (the block contributes nothing).
     */
    bool empty() const
    {
        return !depth && !cullMode && !blend && !polygonMode && !polygonOffset && !pointSize && !lineWidth;
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
           lhs.polygonOffset == rhs.polygonOffset && lhs.pointSize == rhs.pointSize &&
           lhs.lineWidth == rhs.lineWidth;
}

/** @brief Returns whether two optional state blocks differ. */
inline bool operator!=(const RenderState& lhs, const RenderState& rhs)
{
    return !(lhs == rhs);
}

VN_GRAPHICS_NS_END
