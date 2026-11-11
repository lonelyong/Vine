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
 *   declaration, its own state, material and program, its primitives - and, when its placement depends on the camera,
 *   a ViewPlacement VALUE. They are the fold's INPUT, which is what lets a producer that was handed VALUES (the
 *   render world) re-run the fold without a node.
 * - RESOLVED facts are what a select reads: the accumulated placement and folds, the subtree's layer union and world
 *   box, and the end of the subtree in walk order. Nothing there needs the node again - a per-view row is resolved
 *   from the view by the select, which reads its placement value (see ViewPlacement).
 * - `geometry` is the content object a content-side producer HAD (a share); a mirror built from values carries none,
 *   and nothing in the fold or the select reads it (see `.ai/design/graphics-threading.md`).
 * - `identity` is the address the table keys by, kept separately so a producer that has no object at all (the render
 *   world) can still name what it mirrors.
 */
struct Row {
    /// The address this row mirrors (the content object; see ADR-2026-11-09).
    const void* identity{ nullptr };

    // ---- structure: walk order, so a row is followed by its own subtree
    std::uint32_t parent{ 0xFFFFFFFFU };        ///< 0-based row index; the table's first row has none.
    std::uint32_t end{ 0 };                     ///< One past this row's subtree.
    std::uint32_t child_count{ 0 };

    // ---- OWN facts (the fold's input)
    Mat4d                        local{};        ///< Placement in the parent's frame.
    float                        own_opacity{ 1.0F };
    std::optional<std::uint32_t> own_layers{};
    std::optional<int>           own_draw_order{};
    bool                         owns_state{ false };  ///< A StateNode's contribution below is meaningful.
    RenderState                  own_state{};
    MaterialBasePtr              own_material{};
    ShaderProgramPtr             own_program{};
    /// The leaf's own bound as the NODE states it, in WORLD space, for the leaves that cannot state one in their own
    /// frame (a Geometry subclass may place itself in ways only it knows - see Node::subtreeBounds).
    ///
    /// The fold turns that answer into `local_box` with the row's own placement, so every box downstream lives in one
    /// frame and the cull above the row still works. `refreshBoxes` re-reads it and compares, which is why such a row
    /// is a content-side one: a mirror keeps the value it was told (see SceneBake::nodes).
    std::optional<Aabbd>         world_box_answer{};
    /// The per-view half of this row's placement, or null when the row is placed by `local` alone.
    ///
    /// A VALUE (see ViewPlacement): the select resolves it from the view it is drawing, which is what lets a mirror - or
    /// any side that owns no node - draw a billboard without handing its subtree back to the graph walk.
    ViewPlacementPtr             placement{};
    GeometryPtr                  geometry{};           ///< The command's payload (a share, never dereferenced here).
    /// The same geometry's shape as an immutable value (see Geometry::description): what a command hands a backend, so
    /// describing streams never reads the live object. Shared by every draw of this row and stable per revision.
    GeometryDescriptionPtr       description{};
    std::vector<DrawFacts>       draws{};

    // ---- RESOLVED facts (the fold's output; what a select reads)
    Mat4d            world{};                    ///< Placed by this row's own `local`, from the fold (no camera read).
    /// The placement accumulated ABOVE this row.
    ///
    /// A per-view placement produces the row's WHOLE local matrix (it replaces `local`), so it has to be composed
    /// with what the parent already resolved - not with `world`, which already contains this row's own `local`.
    Mat4d            parent_world{};
    std::uint32_t    layers{ kDefaultLayer };
    std::int32_t     draw_order{ 0 };
    float            opacity{ 1.0F };
    RenderState      state{};
    MaterialBasePtr  material{};
    ShaderProgramPtr program{};
    /// This subtree's box in this row's OWN frame (the frame `local` consumes): a leaf's own bounds, or the union of
    /// its children's boxes placed by their `local`. For a per-view row it does NOT cover the orientations the
    /// placement may choose - that bound is in the PARENT frame, see world_box.
    Aabbd            local_box{};
    /// This subtree's box in world space, CONSERVATIVE for every view: what the frustum test reads.
    ///
    /// A per-view row folds `placement->conservativeParentBox(local_box)` through `parent_world`, so the box covers
    /// every orientation the placement can take - the cull above it therefore does not depend on the camera.
    Aabbd            world_box{};
    /// This row or an ancestor resolves its placement per view: the select re-places the subtree from the view instead
    /// of reading `world`.
    bool             view_dependent{ false };
    std::uint32_t    layers_union{ 0U };         ///< OR of the resolved layers of this subtree.
    /// The subtree has NO view-independent bound, so a caller must not cull it (nor any ancestor, whose box was built
    /// from this one). Two causes: a per-view placement that can grow without a finite limit (screen scaling with no
    /// finite maximum), and a leaf that states no box at all.
    bool             unbounded{ false };
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
 * - WHAT IS CONTENT-SIDE ONLY. `nodes` is the build's record of which node each row came from, used by
 *   `refreshBoxes` (a byte edit announces itself on a buffer, so the box that depends on those bytes has to be
 *   re-read). It is NOT part of the draw list: a mirror adopts the rows and drops it (`RenderWorld::adopt`), which
 *   is why nothing in the fold or the select reads a node.
 */
struct SceneBake {
    /// The row index meaning "this row has no parent" (the table's first row).
    inline static constexpr std::uint32_t kNoParent = 0xFFFFFFFFU;

    /// What the fold inherits ABOVE the table's first row (the scene's own opacity, and no placement).
    struct Seed {
        float parent_opacity{ 1.0F };
    };

    std::vector<Row>        rows;
    std::vector<const Node*> nodes; // One per row, content-side box refresh only (dropped by a mirror).
    Seed                    seed{};
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
 * - A node that DECLARES a per-view placement answers `viewPlacement()`; the value goes on the row, and the select
 *   resolves it from the view it is drawing (see ViewPlacement). No run is handed back to a graph walk, so a mirror
 *   (which owns no node) draws a billboard exactly like content does.
 *
 * @param root    Subtree root to build.
 * @param bake    Receives the rows (its previous contents are dropped).
 * @param opacity Opacity accumulated above @p root (the scene's own).
 */
void buildRows(const Node& root, SceneBake& bake, float opacity = 1.0F);

/**
 * @brief Builds a subtree's OWN facts and nothing else: the block a structure change carries.
 *
 * @details
 * - It is the first half of `buildRows` (the walk), without the fold: the rows come out with `parent` / `end` relative
 *   to a table of their own, and every resolved value untouched. Whoever SPLICES them into a live table folds them
 *   there, where the real parent's placement is known (see RenderWorld::attach).
 * - Splitting it here rather than in the mirror is what keeps "how a subtree's own facts are read" in one place: the
 *   graph is only ever read by this file.
 * - Invisible subtrees still produce no rows (see buildRows): "connect a hidden subtree" is a no-op, and the caller
 *   that shows it later announces it as a new attach.
 *
 * @param root Root of the subtree to read.
 * @param bake Receives the rows (its previous contents are dropped); `nodes` is filled for a content-side caller.
 */
void buildOwnFacts(const Node& root, SceneBake& bake);

/**
 * @brief Re-resolves ONE row's subtree: the one fold, run by the build once and by a producer on every edit.
 *
 * @details
 * - Top-down it recomputes the accumulated placement and the folded values from the own facts; bottom-up it
 *   recomputes each row's local and world boxes and its subtree layer union. Only the subtree of @p index is
 *   touched, which is what makes a joint edit cost the joint's subtree instead of the whole graph.
 * - THE CHAIN ABOVE is re-derived too, one row each: a box is a UNION, so the row just re-folded is part of every
 *   ancestor's box. Skipping that leaves a box that still says the subtree is where it was, and a cull then prunes
 *   content that moved INTO view - the object silently disappears.
 * - The boxes are values (`Row::local_box` for a leaf, the children's union for a container), so a fold never asks a
 *   node: a mirror re-folds exactly what content does.
 *
 * @param bake  The table containing @p index.
 * @param index The row to fold (with its subtree).
 * @return How many rows were re-resolved (0 when the row cannot be folded from values).
 */
std::uint32_t resolveSubtree(SceneBake& bake, std::uint32_t index);

/**
 * @brief Recomputes one row's box and every ancestor's, from children that are already folded.
 *
 * @details
 * - The BOX halves of the fold only: no own fact and no resolved value changes, which is exactly the case a detached
 *   subtree leaves behind (the parent's union lost a child, everything else is as it was).
 * - Deepest first: each row is re-derived after the children it reads.
 *
 * @param bake  The table.
 * @param index The row whose own box moved last (the parent of what was removed), or the row that was inserted.
 */
void refoldChain(SceneBake& bake, std::uint32_t index);

/**
 * @brief Re-reads the own boxes of the rows whose geometry revision moved, and folds the ancestors' boxes.
 *
 * @details
 * - Byte content does not travel as a delta (see the design): a writer announces it on the buffer, so a box has to be
 *   re-read when that revision moves. This is the ONE step that still needs the content's thread, and it is a poll
 *   over the nodes the build recorded (`SceneBake::nodes`) - never a read inside the select.
 * - Only boxes are recomputed (plus the placement transform), and only for the leaves whose answer MOVED: the
 *   refresh compares what each leaf states with what the row holds, so a streaming buffer costs its own chain, and a
 *   collection over unchanged boxes folds nothing at all. The chains of several changed leaves are deduped and folded
 *   deepest-first, because in walk order a child always sits after its parent.
 * - A MIRROR has no nodes (see RenderWorld::adopt), so it does not refresh: its boxes are the handshake's until a box
 *   delta exists (a known gap, not a silent read of content).
 *
 * @param bake The table to refresh.
 * @return How many rows' own box actually changed (0 when nothing moved, in which case nothing was folded).
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
