#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::geom::spatial::Aabbd;
using vn::math::Mat4d;
using vn::math::Vec3d;
using vn::math::Vec4d;

namespace detail
{

/**
 * @brief ONE DRAW a leaf row names, resolved: what the select needs to emit a command, and none of the arithmetic.
 *
 * @details
 * - A row with N primitives carries N of these; a container carries none. Resolving them on the PRODUCING side is
 *   what lets the select run without asking a geometry anything: the fold that used to happen per primitive, per
 *   pass, now happens once per description.
 * - `drawn` is false for a primitive whose visible instance prefix is zero: not a draw this frame, but the row keeps
 *   it (an edit may make it one again without changing the description).
 */
struct DrawFacts {
    std::uint32_t      primitive_index{ 0 };                 ///< Which primitive of the geometry this draw is.
    MaterialBasePtr    material{};                           ///< Resolved: primitive, else geometry, else inherited.
    ShaderProgramPtr   program{};                            ///< Resolved the same way.
    BufferSlice<float> instance_transforms{};                ///< Null for a non-instanced draw.
    std::uint32_t      first_instance{ 0 };
    std::uint32_t      instance_count{ 0 };                  ///< 0 for a plain draw (see RenderCommand).
    bool               drawn{ true };
};

/**
 * @brief ONE ROW of a draw list: a node's OWN facts and the values the fold resolved from them.
 *
 * @details
 * - OWN facts are what the node itself says: its local placement, visibility, opacity, its own layer / draw-order
 *   declaration, its own state, material and program, and its primitives. They are the fold's INPUT, which is what
 *   lets a producer that was handed VALUES (the render world) re-run the fold without a node.
 * - RESOLVED facts are what a select reads: the accumulated placement and folds, the subtree's layer union and world
 *   box, and the end of the subtree in walk order. Nothing there needs the node again.
 * - `node` is kept for exactly two named reasons: a per-view run is resolved by the walk that knows how to place it
 *   (see `SceneBake::Fallback`), and this table keeps the content it names alive (see ADR-2026-11-09). Neither is a
 *   read of the node's mutable state during a select.
 * - `identity` is the address the table keys by - the same address `node` carries, kept separately so a producer
 *   that has no node at all (the render world) can still name what it mirrors.
 */
struct Row {
    /// The address this row mirrors (the content object; see ADR-2026-11-09).
    const void* identity{ nullptr };

    // ---- structure: walk order, so a row is followed by its own subtree
    const Node*   node{ nullptr };              ///< Kept for a per-view run and to hold the content alive.
    std::uint32_t parent{ 0xFFFFFFFFU };        ///< 0-based row index; the table's first row has none.
    std::uint32_t end{ 0 };                     ///< One past this row's subtree.
    std::uint32_t child_count{ 0 };

    // ---- OWN facts (the fold's input)
    Mat4d                        local{};        ///< Placement in the parent's frame.
    bool                         own_visible{ true };
    float                        own_opacity{ 1.0F };
    std::optional<std::uint32_t> own_layers{};
    std::optional<int>           own_draw_order{};
    bool                         owns_state{ false };  ///< A StateNode's contribution below is meaningful.
    RenderState                  own_state{};
    MaterialBasePtr              own_material{};
    ShaderProgramPtr             own_program{};
    ViewDependence               dependence{ ViewDependence::None };
    GeometryPtr                  geometry{};           ///< The command's payload (a share, never dereferenced here).
    /// The same geometry's shape as an immutable value (see Geometry::description): what a command hands a backend, so
    /// describing streams never reads the live object. Shared by every draw of this row and stable per revision.
    GeometryDescriptionPtr       description{};
    std::vector<DrawFacts>       draws{};

    /// A row whose own box cannot be answered in its own frame (a custom leaf): its world box is a seed the producer
    /// resolved, and a placement edit cannot re-place it (the render world refuses such a row).
    bool  box_from_node{ false };
    Aabbd seed_world_box{};
    std::uint64_t box_revision{ 0 };             ///< The geometry revision the own box was read at.

    // ---- RESOLVED facts (the fold's output; what a select reads)
    Mat4d            world{};
    std::uint32_t    layers{ kDefaultLayer };
    std::int32_t     draw_order{ 0 };
    float            opacity{ 1.0F };
    RenderState      state{};
    MaterialBasePtr  material{};
    ShaderProgramPtr program{};
    Aabbd            local_box{};                ///< This subtree's box in this row's own frame.
    Aabbd            world_box{};                ///< ... placed by `world` (what the frustum test reads).
    std::uint32_t    layers_union{ 0U };         ///< OR of the resolved layers of this subtree.
    bool             view_dependent{ false };    ///< This row or an ancestor resolves its placement per view.
};

/**
 * @brief THE DRAW LIST'S ROWS: every node's facts in walk order, shared by every producer of a draw list.
 *
 * @details
 * - WHY IT IS SHARED. `Scene` builds the rows by walking the live graph; the render world builds them from values it
 *   was told (see `.ai/design/graphics-threading.md`). Both must run the SAME fold, so the row type, the build and
 *   the passes over it live in one place (`DrawListBake.cpp`) instead of in two implementations that would drift.
 * - WHY AN ARRAY AND NOT A MAP. The build and the select are driven by the same order (children in order, invisible
 *   subtrees pruned), so the cursor IS the identity: no node-keyed map, no hashing. `end` keeps that alignment when
 *   a view prunes a subtree - the select jumps straight past it.
 * - WHAT IS NOT HERE. Anything a camera decides: the frustum test, the sort key, the final list.
 * - LIFETIME. A row holds SHARES of the content it names (geometry, material, program), so a row can never name a
 *   freed object. The table is dropped when the graph announces a change (the epoch) or the scene invalidates.
 */
struct SceneBake {
    /// The row index meaning "this row has no parent" (the table's first row).
    inline static constexpr std::uint32_t kNoParent = 0xFFFFFFFFU;

    /// What the fold inherits ABOVE the table's first row (the scene's own opacity, and no placement).
    struct Seed {
        float parent_opacity{ 1.0F };
    };

    /// The context a per-view run is handed back to the walk with (see selectFromBake): one per run, the TOPMOST
    /// such node, so a run is resolved once - by the code that already knows how to place it.
    struct Fallback {
        std::uint32_t    index{ 0 };
        const Node*      node{ nullptr };
        Mat4d            world{};              ///< The run's TOP node's own accumulated placement (the walk's convention).
        float            parent_opacity{ 1.0F }; ///< Opacity accumulated ABOVE the run.
        std::uint32_t    layers{ kDefaultLayer }; ///< Layers the run inherits from above.
        std::int32_t     draw_order{ 0 };
        RenderState      state{};
        MaterialBasePtr  material{};
        ShaderProgramPtr program{};
    };

    std::vector<Row>      rows;
    std::vector<Fallback> fallbacks; // ascending by `index` (the build walks in order)
    Seed                  seed{};
};

/**
 * @brief View frustum defined by six planes, used for culling.
 *
 * Planes are stored in the order: left, right, bottom, top, near, far. Each plane is a 4-vector (a, b, c, d)
 * satisfying a*x + b*y + c*z + d = 0 in world space.
 */
class Frustum {
  public:
    /** @brief Extracts the frustum from a combined view-projection matrix.
     * @param vp Combined projection * view matrix.
     * @return Frustum with normalized plane equations.
     */
    static Frustum fromViewProjection(const Mat4d& vp)
    {
        Frustum     f;
        const auto  row = [&vp](int r) { return Vec4d(vp.element(r, 0), vp.element(r, 1), vp.element(r, 2), vp.element(r, 3)); };
        const Vec4d r0  = row(0);
        const Vec4d r1  = row(1);
        const Vec4d r2  = row(2);
        const Vec4d r3  = row(3);
        f.planes_[0]    = r3 + r0; // left
        f.planes_[1]    = r3 - r0; // right
        f.planes_[2]    = r3 + r1; // bottom
        f.planes_[3]    = r3 - r1; // top
        f.planes_[4]    = r3 + r2; // near
        f.planes_[5]    = r3 - r2; // far
        for (auto& p : f.planes_) {
            const Vec3d  n(p.x, p.y, p.z);
            const double len = n.length();
            if (len > 1e-12) {
                p.x /= len;
                p.y /= len;
                p.z /= len;
                p.w /= len;
            }
        }
        return f;
    }

    /** @brief Tests whether a box lies fully outside the frustum (the p-vertex test).
     * @param box World-space axis-aligned bounding box.
     * @return true when the box is fully outside the frustum.
     */
    bool isOutside(const Aabbd& box) const
    {
        for (const auto& p : planes_) {
            const Vec3d n(p.x, p.y, p.z);
            const Vec3d pos{ (n.x >= 0.0) ? box.max().x : box.min().x, (n.y >= 0.0) ? box.max().y : box.min().y, (n.z >= 0.0) ? box.max().z : box.min().z };
            if (n.dot(pos) + p.w < 0.0) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<Vec4d, 6> planes_{};
};

/**
 * @brief Builds a draw list's rows from a graph: every node's OWN facts, then one fold over them.
 *
 * @details
 * - It is the ONLY code that reads a graph for a draw list, and it runs on the thread that owns that graph. What it
 *   writes is values; a side that owns no graph takes them as they are (see RenderWorld::adopt).
 * - Invisible subtrees produce no rows at all: the walk skipped them before, and a row that can never be drawn is
 *   memory nobody reads.
 * - A node that DECLARES a per-view placement (Node::viewDependence) starts a run recorded in `bake.fallbacks`; the
 *   select hands that run back to the walk that knows how to place it for the camera at hand.
 *
 * @param root    Subtree root to build.
 * @param bake    Receives the rows (its previous contents are dropped).
 * @param opacity Opacity accumulated above @p root (the scene's own).
 */
void buildRows(const Node& root, SceneBake& bake, float opacity = 1.0F);

/**
 * @brief Re-resolves ONE row's subtree: the one fold, run by the build once and by a producer on every edit.
 *
 * @details
 * - Top-down it recomputes the accumulated placement and the folded values from the own facts; bottom-up it
 *   recomputes each row's local and world boxes and its subtree layer union. Only the subtree of @p index is
 *   touched, which is what makes a joint edit cost the joint's subtree instead of the whole graph.
 * - A row that resolves its placement per view, or whose box could only be answered by the node, is skipped: the
 *   fold cannot answer for it from values (the build kept the node's answer instead).
 *
 * @param bake  The table containing @p index.
 * @param index The row to fold (with its subtree).
 * @return How many rows were re-resolved (0 when the row cannot be folded from values).
 */
std::uint32_t resolveSubtree(SceneBake& bake, std::uint32_t index);

/**
 * @brief Re-reads the own boxes of the rows whose geometry revision moved, and folds the ancestors' boxes.
 *
 * @details
 * - Byte content does not travel as a delta (see the design): a writer announces it on the buffer, so a box has to be
 *   re-read when that revision moves. This is the ONE step that still needs the content's thread, and it is a poll
 *   over the rows a collection is about to use - never a read inside the select.
 * - Only boxes are recomputed (plus the placement transform), so a streaming buffer costs its chain, not the graph.
 *
 * @param bake The table to refresh.
 * @return How many rows had their own box re-read.
 */
std::uint32_t refreshBoxes(SceneBake& bake);

/**
 * @brief Collects one view's commands from the rows: the VIEW half of a collection, and it reads rows only.
 *
 * @param bake          The table (see bakeSubtree).
 * @param frustum       The view's frustum.
 * @param view          The view being drawn.
 * @param layer_mask    Layers this picture draws.
 * @param viewport_used Receives true when a per-view row needs the device-pixel viewport.
 * @param out           Receives the commands, in walk order.
 */
void selectFromBake(const SceneBake&            bake,
                    const Frustum&              frustum,
                    const CollectView&          view,
                    std::uint32_t               layer_mask,
                    bool&                       viewport_used,
                    std::vector<RenderCommand>& out);

/**
 * @brief Reorders @p commands into the draw order a frame draws them in, in place.
 *
 * @details
 * - The order is the CONTENT's declared draw order (see Node::setDrawOrder), then opaque before transparent, then
 *   front-to-back for opaque and back-to-front for transparent. Ties keep the collection order.
 * - It exists as its own step because BOTH producers of a draw list need it and neither may own a copy.
 *
 * @param commands Commands to reorder (replaced by their sorted order).
 * @param eye      World-space eye point the distances are measured from.
 */
void sortCollected(std::vector<RenderCommand>& commands, const Vec3d& eye);

} // namespace detail

VN_GRAPHICS_NS_END
