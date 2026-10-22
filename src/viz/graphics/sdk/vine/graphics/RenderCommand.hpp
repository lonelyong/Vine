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
 * - ONE COMMAND IS ONE DRAW. It names the leaf geometry AND which of its primitives to draw (see
 *   Primitive.hpp): a geometry with three parts collects three commands, because a primitive's topology, index
 *   span and shading are exactly what a draw is. The material and program here are the RESOLVED ones (the
 *   primitive's, else the nearest StateNode's), so a backend never has to look at the scene graph to shade.
 */
struct VN_GRAPHICS_API RenderCommand {
    /** Leaf geometry to render. */
    GeometryPtr geometry;

    /** Which of the geometry's primitives this command draws (its index, see Geometry::primitive). */
    std::uint32_t primitive_index{0};

    /** Material to use. */
    MaterialPtr material;

    /** Effective shader program (leaf/StateNode resolution), null = default.
     *
     * @note A backend that supports user programs compiles/uses this instead of the built-in program; backends
     *       without user-program support ignore it.
     */
    ShaderProgramPtr program;

    /** World-space model matrix. */
    Mat4d model_matrix;

    /** @brief Returns whether the object is transparent and therefore needs sorted rendering.
     *
     * @details
     * - DERIVED from @ref opacity, never stored: two fields for one fact let a caller set `opacity = 0.5` without the
     *   flag (the command then sorts into the opaque batch and draws with the wrong blend expectation) or set the
     *   flag alone (it draws as opaque).
     * - Scene::collectRenderCommands is the only producer and derives it the same way, so the two spellings cannot
     *   disagree — and the epsilon lives here, once, instead of at every producer.
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
     * - The OUTER sort key of the list a pass draws: a higher value is recorded later, so it lands on top of the
     *   earlier draws.
     * - INSIDE one value the order is the picture the collection has always produced - opaque front-to-back, then
     *   transparent back-to-front - which is what keeps alpha content correctly ordered among the commands that share
     *   a draw order.
     * - 0 is the default and means "nobody declared one".
     */
    int draw_order = 0;

    /** The render state the CONTENT authored for this command (every item optional).
     *
     * @details
     * - Computed at collection time by folding every StateNode from the scene root down to the geometry. An unset
     *   item means "nobody on the path said anything about it" - NOT "set to the default": the final state of the
     *   command depends on the pass drawing it as well, and the ONE place that answers that is
     *   `resolveRenderState(authored_state, pass.stateOverrides())` (see StateNode.hpp, which also says which item a
     *   pass overrides and which one it only supplies a default for).
     * - A backend consumes the resolved state to select a pipeline variant and to diff its dynamic state; backends
     *   that consume no per-object state ignore this.
     */
    RenderState authored_state;

    /** The instance transforms this command is drawn with (empty = a plain, single-instance draw).
     *
     * @details
     * - A BufferSlice of SCALARS: 16 per instance, column-major. It is the instance stream of the Primitive
     *   that produced this command; a command without a stream leaves it empty.
     * - The buffer is held (not copied): the frame borrows its address and revision, the same way it borrows the
     *   geometry and material identities.
     */
    BufferSlice<float> instance_transforms;

    /** The first instance drawn from @ref instance_transforms (the primitive slice's start). */
    std::uint32_t first_instance{0};

    /** How many instances this command draws (0 = a plain draw; an instanced primitive uses 1..N). */
    std::uint32_t instance_count{0};

    /** @brief Default constructor. */
    RenderCommand() = default;

    /** @brief Constructs a render command.
     *
     * @param g             Leaf geometry to render.
     * @param m             Material to use.
     * @param model         World-space model matrix.
     * @param primitive_index Which of @p g's primitives this command draws.
     */
    RenderCommand(intrusive_ptr<Geometry> g, intrusive_ptr<Material> m, const Mat4d& model,
                  std::uint32_t primitive_index = 0);

    /** @brief Gets the primitive this command draws (null when the geometry no longer has it).
     *
     * @details
     * - Read through the geometry rather than copied: a primitive's index span is a slice that follows a
     *   growing buffer, and the collected list is rebuilt when the content announces a change, so a command
     *   never outlives the shape it names.
     *
     * @return The primitive, or null when the geometry was edited under this command.
     */
    [[nodiscard]] const Primitive* primitive() const noexcept
    {
        return geometry != nullptr ? geometry->primitive(primitive_index) : nullptr;
    }
};

VN_GRAPHICS_NS_END
