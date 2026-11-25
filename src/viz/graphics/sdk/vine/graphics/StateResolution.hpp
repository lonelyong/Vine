#pragma once
#include "graphics_global.hpp"

#include <optional>

#include "DepthPolicy.hpp"
#include "PolygonMode.hpp"
#include "Topology.hpp"

#include "RenderState.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Concrete render state after defaults are applied: the pipeline-STATE half of a draw.
 *
 * StateNodes contribute overrides; everything they do not set falls back to the defaults below (which
 * match the backend's current default pipeline state, so a scene without StateNodes resolves to today's
 * behaviour).
 *
 * @note
 * - What is NOT here is a DATA fact: the primitive's topology is not state (see Topology.hpp) and lives on
 *   `ResolvedCommandState`, next to the state it travels with. Keeping it out is what lets
 *   `applyStateDefaults()` answer honestly: it fills what nobody stated and never invents a data fact it
 *   was not given (G1).
 */
struct VN_GRAPHICS_API ResolvedRenderState {
    DepthState    depth;                           // Depth state (defaults: test/write on, Less).
    CullMode      cullMode = CullMode::None;       // Face culling (default: none).
    BlendState    blend;                           // Blending (default: disabled).
    PolygonMode   polygonMode = PolygonMode::Fill; // Polygon mode (default: fill).
    PolygonOffset polygonOffset{};                 // Depth bias (default: none).
    /// Point size in framebuffer pixels (default: 1). It is the ONE resolved item whose destination is the
    /// DRAW BLOCK (`VineDrawBlock::params.y`), because a point's size is a vertex-shader output rather than
    /// API state: it never enters a pipeline key, so two point sizes share one pipeline.
    float pointSize   = 1.0F;
    /// Width of rasterised LINE primitives in framebuffer pixels (default: 1). Its destination is the
    /// backend's DYNAMIC STATE (it never enters a pipeline key either), capped by the device's lineWidthRange
    /// (see Capabilities::limits.max_line_width). It does not widen `PolygonMode::Line`.
    float lineWidth   = 1.0F;
};

/** @brief Returns whether two resolved states are equal. */
inline bool operator==(const ResolvedRenderState& lhs, const ResolvedRenderState& rhs)
{
    return lhs.depth == rhs.depth && lhs.cullMode == rhs.cullMode && lhs.blend == rhs.blend && lhs.polygonMode == rhs.polygonMode &&
           lhs.polygonOffset == rhs.polygonOffset && lhs.pointSize == rhs.pointSize && lhs.lineWidth == rhs.lineWidth;
}

/** @brief Returns whether two resolved states differ. */
inline bool operator!=(const ResolvedRenderState& lhs, const ResolvedRenderState& rhs)
{
    return !(lhs == rhs);
}

/**
 * @brief What a PASS says about the content it draws.
 *
 * @details
 * - The pass side of the state priority table (see resolveRenderState), in one value: a backend resolves a
 *   command's state without asking the pass anything else.
 * - The two items differ ON PURPOSE (the table in `.ai/design/graphics-scene.md` §4.1): `depth` is a POLICY
 *   for content that authored none ("this slot tests and writes", "this slot is translucent over what is
 *   already there", "this slot is an overlay"), and content that authored a depth state keeps it - depth is
 *   something scene content routinely declares; `polygonMode` is an OVERRIDE that beats every command the
 *   pass draws, because it is a viewing switch ("show me the edges") that a content-authored Line must not
 *   be able to veto.
 * - `program` is deliberately NOT here: it is not a render state but a different SHADING, so it travels as its own
 *   field of the pass draw (PassDraw::program_override) and the backend resolves it per command exactly the way it
 *   resolves `state` - the command list itself is never rewritten (RenderPass::setProgramOverride).
 */
struct VN_GRAPHICS_API PassRenderState {
    DepthPolicy                  depth{ DepthPolicy::TestAndWrite }; // Depth policy for content that authored none.
    std::optional<PolygonMode> polygonMode{};                    // Rasterisation override for every command.
    /// Point-size override for every command, in framebuffer pixels. Like the two below it, it is a viewing
    /// switch: the pass that turns a scene into points has to be able to size them, and what the content
    /// authored stays the content's own word for the passes that draw it as geometry.
    std::optional<float> pointSize{};
    /// Line-width override for every command, in framebuffer pixels (the "draw me the structure, thicker"
    /// switch of a CAD view). It shares the point size's viewing-switch rule and, like it, recompiles nothing.
    std::optional<float> lineWidth{};
    /// Primitive-topology override for every command ("show me this scene as points"). It overrides what the
    /// DATA states, because how a view looks at a scene is the pass' decision - the same rule the rasterisation
    /// mode already follows. The data keeps saying what it is; a pipeline is compiled per effective topology.
    std::optional<Topology> topology{};
};

/** @brief One command's FINAL state in one pass, and the assembly it draws with. */
struct VN_GRAPHICS_API ResolvedCommandState {
    ResolvedRenderState state; // The final state (defaults filled in).
    /// The EFFECTIVE primitive assembly: the PRIMITIVE's own value (see Primitive.hpp), or the pass' override
    /// of it for the whole view. It is a DATA fact rather than state, which is why it sits BESIDE @ref state -
    /// and why it has no "nobody said" case: the data states one, or the pass overrides it.
    Topology topology{ Topology::Triangles };
};

/**
 * @brief Resolves the final render state of content drawn by a pass: the ONE implementation of the priority table.
 *
 * @details
 * - The table: `pass override > content (the folded StateNodes) > defaults`.
 * - One place and one test table. Everything consuming state goes through here: the geometry/state a
 *   backend bakes into a pipeline variant, the dynamic state it diffs per command, and any diagnostic that
 *   has to explain a picture.
 * - The assembly travels in the SAME answer but not in the same field: `state` is what a StateNode may
 *   author, `topology` is the data's own word (or the pass' view switch over it). A caller building a
 *   pipeline key reads both, and neither is guessed (`G1`).
 *
 * @param authored The state the CONTENT authored (DrawItem::authored_state): every item left unset is
 *                 "nobody on the path said anything about it", which is not the same as "set to the default".
 * @param pass     What the pass drawing it overrides (RenderPass::stateOverrides).
 * @param data_topology The PRIMITIVE's topology (see Primitive.hpp): the data's own word, which the pass may
 *                 override but a StateNode cannot (topology is not state - it says how this draw consumes its
 *                 vertex/index data).
 * @return The final state and the effective assembly.
 */
VN_GRAPHICS_API ResolvedCommandState resolveRenderState(const RenderState& authored, const PassRenderState& pass, Topology data_topology);

/**
 * @brief Applies default values to a folded optional state block.
 *
 * The content-only spelling of the priority table (resolveRenderState(authored, pass)): a pass that overrides
 * nothing resolves exactly like this, so a caller that has the content's word and no pass to draw it with (a
 * tool, a test, a host reading its own scene) uses it directly.
 *
 * It is NOT a `resolve*` in the vocabulary of `graphics-scene.md` 2.12: that verb takes a value AND a context
 * (a pass, the data's topology) and applies the priority table, while this one takes a value alone and fills
 * what nobody stated. The name is how a reader tells the two apart without opening the header.
 *
 * @param state Folded optional state (see collectRenderState).
 * @return Concrete state with defaults applied - the STATE half of a pipeline-variant key.
 *         A caller building a key also names the data's topology (see resolveRenderState).
 */
VN_GRAPHICS_API ResolvedRenderState applyStateDefaults(const RenderState& state);

VN_GRAPHICS_NS_END
