#pragma once
#include "graphics_global.hpp"

#include <optional>

#include "Group.hpp"
// Included rather than forward-declared: this class HOLDS an intrusive_ptr<MaterialBase>, so every translation
// unit that destroys a StateNode instantiates the reference-count hooks and needs the complete type.
#include "Material.hpp"

#include "RenderState.hpp"
#include "StateResolution.hpp"

VN_GRAPHICS_NS_BEGIN

class ShaderProgram;
using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief Scene-graph node applying render state to its subtree.
 *
 * @details
 * - Contributes optional state items (depth, culling, blending, polygon mode) to every drawable in
 *   its subtree; an unset item is inherited from enclosing StateNodes, and a deeper node overrides a
 *   shallower one for the same item.
 * - A leaf's effective state is the fold of every StateNode from the scene root to the leaf
 *   (collectRenderState) with defaults applied (resolveRenderState), and it participates in the backend's
 *   pipeline-variant key.
 * - State deliberately lives on nodes rather than per drawable, so a batch of geometry sharing one StateNode
 *   also shares one pipeline variant; the model mirrors vsg::StateGroup / the GL state stack and stays
 *   backend-agnostic.
 * - The TYPES this node uses live in two narrower headers, so a consumer that only needs the vocabulary or
 *   the priority table (a pass, a backend's pipeline key) does not include the scene graph: RenderState.hpp
 *   holds what content may say, StateResolution.hpp holds what a resolved state is and how it is computed.
 */
class VN_GRAPHICS_API StateNode : public Group {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a state node that overrides nothing. */
    StateNode();
    ~StateNode();

  public:
    /** @brief Sets the depth state (test/write/compare) for this subtree.
     *
     * @param state Depth test / write / compare for this subtree.
     */
    void setDepth(const DepthState& state);

    /** @brief Clears the depth override so enclosing state is inherited. */
    void clearDepth();

    /** @brief Gets the depth override.
     *
     * @return The override, or nullopt when unset (the enclosing state applies).
     */
    std::optional<DepthState> depth() const;

    /** @brief Sets the face culling mode for this subtree.
     *
     * @param mode Which faces to cull (see CullMode).
     */
    void setCullMode(CullMode mode);

    /** @brief Clears the culling override so enclosing state is inherited. */
    void clearCullMode();

    /** @brief Gets the culling override.
     *
     * @return The override, or nullopt when unset (the enclosing state applies).
     */
    std::optional<CullMode> cullMode() const;

    /** @brief Sets the blend state for this subtree.
     *
     * @param state Blending on/off and its factors (see BlendState).
     */
    void setBlend(const BlendState& state);

    /** @brief Clears the blend override so enclosing state is inherited. */
    void clearBlend();

    /** @brief Gets the blend override.
     *
     * @return The override, or nullopt when unset (the enclosing state applies).
     */
    std::optional<BlendState> blend() const;

    /** @brief Sets the polygon rasterisation mode for this subtree.
     *
     * @param mode How triangles rasterise: fill, line or point (see PolygonMode).
     */
    void setPolygonMode(PolygonMode mode);

    /** @brief Clears the polygon-mode override so enclosing state is inherited. */
    void clearPolygonMode();

    /** @brief Gets the polygon-mode override.
     *
     * @return The override, or nullopt when unset (the enclosing state applies).
     */
    std::optional<PolygonMode> polygonMode() const;

    /** @brief Sets the depth bias for this subtree (coplanar surfaces, decals, wireframe over a face).
     *
     * @details
     * - The offset is applied to the RASTERISED depth, so two faces at the same physical depth stop fighting
     *   without moving anything: the vertex, the bounding box and the picking answer are unchanged.
     * - {0, 0} is the same as clearing it (see PolygonOffset::isZero).
     *
     * @param offset Slope-scaled and constant depth offset in depth-buffer units.
     */
    void setPolygonOffset(const PolygonOffset& offset);

    /** @brief Clears the depth-bias override so enclosing state is inherited. */
    void clearPolygonOffset();

    /** @brief Gets the depth-bias override.
     *
     * @return The override, or nullopt when unset (the enclosing state applies).
     */
    std::optional<PolygonOffset> polygonOffset() const;

    /** @brief Sets the pixel size of point primitives for this subtree.
     *
     * @details
     * - The size reaches the shading as the draw block's `params.y` (a vertex-shader output, `gl_PointSize`),
     *   so it is per-drawable data rather than pipeline state: changing it never recompiles a pipeline.
     * - Only POINT rasterisation uses it (`Topology::Points`, or `PolygonMode::Point` on triangle data);
     *   triangles and lines ignore it. Values below 1.0 are raised to 1.0, and the device clamps the result
     *   to its own point-size range.
     *
     * @param size Point size in framebuffer pixels.
     */
    void setPointSize(float size);

    /** @brief Clears the point-size override so enclosing state is inherited. */
    void clearPointSize();

    /** @brief Gets the point-size override.
     *
     * @return The size in framebuffer pixels, or nullopt when unset (the enclosing state applies, then 1).
     */
    std::optional<float> pointSize() const;

    /** @brief Sets the width of LINE primitives for this subtree, in framebuffer pixels.
     *
     * @details
     * - It applies to `Topology::Lines` (axes, skeletons, trajectories, leader lines) and to nothing else:
     *   `PolygonMode::Line` edges keep their own width, so a thick WIREFRAME is a different technique
     *   (expanded quads or a shader), not this value.
     * - Delivered as dynamic state (`vkCmdSetLineWidth`), so one pipeline serves every width and changing it
     *   recompiles nothing. It replaces the LAYOUT value the pipeline was created with.
     * - The device's `lineWidthRange` is the ceiling (see Capabilities::max_line_width) and values below 1.0
     *   are raised to 1.0.
     *
     * @param width Line width in framebuffer pixels.
     */
    void setLineWidth(float width);

    /** @brief Clears the line-width override so enclosing state is inherited. */
    void clearLineWidth();

    /** @brief Gets the line-width override.
     *
     * @return The width in framebuffer pixels, or nullopt when unset (the enclosing state applies, then 1).
     */
    std::optional<float> lineWidth() const;

    /** @brief Sets a custom shader program for this subtree.
     *
     * Program resolution (graphics-rendering.md): a leaf Geometry's own program
     * wins; otherwise the nearest ancestor StateNode program applies. This is
     * separate from the render-state items above: a program replaces the
     * shading, not a pipeline-state slot.
     *
     * @param program Program, or nullptr to clear.
     */
    void setProgram(intrusive_ptr<ShaderProgram> program);

    /** @brief Clears this node's program override so it is inherited. */
    void clearProgram();

    /** @brief Gets this node's program override.
     *
     * @return The program, or null when unset (the nearest ancestor's applies, then the engine default).
     */
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
    void setMaterial(MaterialBasePtr material);

    /** @brief Clears this node's material override so it is inherited. */
    void clearMaterial();

    /** @brief Gets this node's material override.
     *
     * @return The material, or null when unset (the nearest ancestor's applies).
     */
    raw_ptr<MaterialBase> material() const;

    /** @brief Gets this node's whole optional state block.
     *
     * @return The block (unset items inherit from enclosing StateNodes).
     */
    const RenderState& renderState() const;

    /** @brief Returns whether any state item is set on this node.
     *
     * @return true when at least one item is set.
     */
    bool hasState() const;

    /** @brief Clears every state item set on this node. */
    void clearState();

  private:
    RenderState                  state_;
    intrusive_ptr<ShaderProgram> program_;
    MaterialBasePtr              material_;
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
 * The leaf's first primitive's own program wins; otherwise the Geometry's default program applies; otherwise
 * the nearest ancestor StateNode program; otherwise the engine default (null). The helper answers "what would
 * this node's single draw use"; a geometry whose draws state different programs is read per command instead.
 *
 * @param node Leaf (Geometry) or intermediate node to resolve for.
 * @return Effective program, or null for the engine default.
 */
VN_GRAPHICS_API ShaderProgramPtr effectiveProgram(raw_ptr<const Node> node);

/**
 * @brief Resolves the material a node's Geometry renders with.
 *
 * The same rule as effectiveProgram(): the first primitive's own material wins, then the Geometry's default,
 * then the nearest ancestor StateNode material, then null (the engine default). Nearest, not literal — the
 * first override found walking up the parents ends the search, because a material replaces the shading
 * wholesale rather than overriding one item of it.
 *
 * @param node Node to resolve for (typically a Geometry).
 * @return The material to render with, or null when neither the leaf nor any ancestor sets one.
 */
VN_GRAPHICS_API MaterialBasePtr effectiveMaterial(raw_ptr<const Node> node);

VN_GRAPHICS_NS_END
