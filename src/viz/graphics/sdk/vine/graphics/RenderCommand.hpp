#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "Geometry.hpp"
#include "Material.hpp"
#include "ShaderProgram.hpp"
#include "StateNode.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;

/**
 * @brief A single geometry rendering instruction.
 *
 * @details
 * - Encapsulates everything needed to render one object: the leaf geometry itself, its material, and its world-space model matrix baked from the enclosing MatrixTransform chain.
 */
struct VN_GRAPHICS_API RenderCommand {
    /** Leaf geometry to render. */
    GeometryPtr geometry;

    /** Material to use. */
    MaterialPtr material;

    /** Effective shader program (leaf/StateNode resolution), null = default.
     *
     * @note A backend that supports user programs compiles/uses this instead of the built-in program; backends without user-program support ignore it.
     */
    ShaderProgramPtr program;

    /** World-space model matrix. */
    Mat4d modelMatrix;

    /** @brief Returns whether the object is transparent and therefore needs sorted rendering.
     *
     * @details
     * - DERIVED from @ref opacity, never stored: two fields for one fact let a caller set `opacity = 0.5` without the flag (the command then sorts into the opaque batch and draws with the wrong blend expectation) or set the flag alone (it draws as opaque).
     * - Scene::collectRenderCommands is the only producer and derives it the same way, so the two spellings cannot disagree — and the epsilon lives here, once, instead of at every producer.
     *
     * @return true when the effective opacity is below 1 (within the comparison epsilon).
     */
    [[nodiscard]] bool isTransparent() const noexcept
    {
        return opacity < 1.0f - 1e-6f;
    }

    /** Effective opacity in [0, 1]: scene x nodes x leaf geometry. */
    float opacity = 1.0f;

    /** Where this command's draw sits in the order of the pass drawing it (see Node::setDrawOrder).
     *
     * @details
     * - The OUTER sort key of the list a pass draws: a higher value is recorded later, so it lands on top of the earlier draws.
     * - INSIDE one value the order is the picture the collection has always produced - opaque front-to-back, then transparent back-to-front - which is what keeps alpha content correctly ordered among the commands that share a draw order.
     * - 0 is the default and means "nobody declared one".
     */
    int drawOrder = 0;

    /** The render state the CONTENT authored for this command (every item optional).
     *
     * @details
     * - Computed at collection time by folding every StateNode from the scene root down to the geometry. An unset item means "nobody on the path said anything about it" - NOT "set to the default": the final state of the command depends on the pass drawing it as well, and the ONE place that answers that is `resolveRenderState(authoredState, pass.stateOverrides())` (see StateNode.hpp, which also says which item a pass overrides and which one it only supplies a default for).
     * - A backend consumes the resolved state to select a pipeline variant and to diff its dynamic state; backends that consume no per-object state ignore this.
     */
    RenderState authoredState;

    /** @brief Default constructor. */
    RenderCommand() = default;

    /** @brief Constructs a render command.
     *
     * @param g     Leaf geometry to render.
     * @param m     Material to use.
     * @param model World-space model matrix.
     */
    RenderCommand(intrusive_ptr<Geometry> g, intrusive_ptr<Material> m, const Mat4d& model);};

VN_GRAPHICS_NS_END
