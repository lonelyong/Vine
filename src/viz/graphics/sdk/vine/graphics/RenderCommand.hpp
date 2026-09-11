#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "Geometry.hpp"
#include "Material.hpp"
#include "ShaderProgram.hpp"
#include "StateNode.hpp"

V_GRAPHICS_NS_BEGIN

using vine::math::Mat4d;

/**
 * @brief A single geometry rendering instruction.
 *
 * Encapsulates everything needed to render one object: the leaf geometry
 * itself, its material, and its world-space model matrix baked from the
 * enclosing MatrixTransform chain.
 */
struct V_GRAPHICS_API RenderCommand {
    /** Leaf geometry to render. */
    GeometryPtr geometry;

    /** Material to use. */
    MaterialPtr material;

    /** Effective shader program (leaf/StateNode resolution), null = default.
     *
     * A backend that supports user programs compiles/uses this instead of the
     * built-in program; backends without user-program support ignore it.
     */
    ShaderProgramPtr program;

    /** World-space model matrix. */
    Mat4d modelMatrix;

    /** Whether the object is transparent (requires sorted rendering). */
    bool isTransparent = false;

    /** Effective opacity in [0, 1]: scene x nodes x leaf geometry. */
    float opacity = 1.0f;

    /** Effective resolved render state for this command.
     *
     * Computed at collection time by folding every StateNode from the scene
     * root down to the geometry and applying defaults. A backend uses it to
     * select the matching pipeline variant; backends that do not consume
     * per-object state ignore it.
     */
    ResolvedRenderState renderState;

    /** Whether @ref renderState's depth item came from a StateNode.
     *
     * False when no StateNode on the path set depth, so @ref renderState only
     * carries the defaults. A backend uses this to honour the pass-level depth
     * policy (RenderPass::depthMode) for content that does not ask for a
     * specific depth handling of its own, while an explicit StateNode depth
     * still wins (finer-grained intent over the pass default).
     */
    bool depthExplicit = false;

    /** @brief Default constructor. */
    RenderCommand() = default;

    /** @brief Constructs a render command.
     *
     * @param g     Leaf geometry to render.
     * @param m     Material to use.
     * @param model World-space model matrix.
     */
    RenderCommand(intrusive_ptr<Geometry> g, intrusive_ptr<Material> m, const Mat4d& model);};

V_GRAPHICS_NS_END
