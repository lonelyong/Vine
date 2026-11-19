#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <vine/SmallVector.hpp>

#include <vine/graphics/View.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include <vine/graphics/graphics_global.hpp>

#include "Frustum.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::geom::spatial::Aabbd;
using vn::math::Mat4d;

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
 * - Equality is EXACT and defaulted: a field added here joins the comparison automatically, which is what the
 *   change test needs (see RowRefresh).
 */
struct ResolvedDraw {
    std::uint32_t      primitive_index{ 0 };                 ///< Which primitive of the geometry this draw is.
    MaterialValuesPtr    material{};                           ///< Resolved: primitive, else geometry, else inherited.
    /// The same material's values as a value (see MaterialValues::description), taken where the values were read: what
    /// a command hands a backend, so recording never reads the live material.
    MaterialDescriptionPtr material_description{};
    ShaderProgramPtr   program{};                            ///< Resolved the same way.
    /// The same program's text as a value (see ShaderProgramDescription), taken where the values were read: what a
    /// command hands a backend, so recording never reads the live program.
    ShaderProgramDescriptionPtr program_description{};
    BufferSlice<float> instance_transforms{};                ///< Null for a non-instanced draw.
    std::uint32_t      first_instance{ 0 };
    std::uint32_t      instance_count{ 0 };                  ///< 0 for a plain draw (see DrawItem).
    bool               drawn{ true };

    [[nodiscard]] bool operator==(const ResolvedDraw& other) const = default;
};

/**
 * @brief The facts the FOLD resolves for one row: what the row draws with, and what its children inherit from it.
 *
 * @details
 * - IT IS ONE VALUE BECAUSE IT MOVES AS ONE. A node's edit re-derives its whole subtree in this same step, so a
 *   publication has to ask "did any resolved fact move", not "did these nine fields move": storing this value is the
 *   only way the resolved side is written (`storeResolved`), and it moves `Row::resolved_serial` when it changes -
 *   which is what makes "this chunk is still the same picture" one comparison (see `SceneSnapshot`).
 * - THE STATE / MATERIAL / PROGRAM ARE HERE FOR EVERY ROW, not only for the rows that draw: they are what a re-fold
 *   below this row starts from (see `resolveSubtree`), so a container that stored nothing would reset its whole
 *   subtree to the defaults - the edit path's second, hidden rule (design `graphics-frame-pipeline.md` §3.3 F2).
 * - `local_box` covers both halves of one fact: a leaf's own bound (as its node states it) IS its folded box, so a
 *   leaf and a container do not need two spells of "this subtree's extent".
 * - EQUALITY IS DEFAULTED, FIELD FOR FIELD: the comparison is the definition of "the fold produced the same row
 *   again", so it must cover everything. A hand-written list is a list somebody forgets to extend; a defaulted one
 *   cannot fall out of step with the fields.
 */
struct ResolvedRow {
    /// This row's placement accumulated from the root; a per-view row is re-placed by the select.
    Mat4d world{};
    /// The placement accumulated ABOVE this row: a per-view placement composes with this, never with `world`.
    Mat4d parent_world{};
    float opacity{ 1.0F };
    std::uint32_t layers{ kDefaultLayer };
    std::int32_t draw_order{ 0 };
    RenderState state{};
    MaterialValuesPtr material{};
    /// The resolved material's values (see `material`): from the row that owned it, by pointer copy - a material EDIT
    /// is a new value at the next publish, never a live re-read in the middle of a frame.
    MaterialDescriptionPtr material_description{};
    ShaderProgramPtr program{};
    /// The resolved program's text as a value (see `program`), copied for the same reason the material's values are.
    ShaderProgramDescriptionPtr program_description{};
    /// This subtree's box in this row's OWN frame (the frame `local` consumes): a leaf's own bounds, or the union of
    /// its children's boxes placed by their `local`. For a per-view row it does NOT cover the orientations the
    /// placement may choose - that bound is in the PARENT frame, see world_box.
    Aabbd local_box{};
    /// This subtree's box in world space, CONSERVATIVE for every view: what the frustum test reads.
    ///
    /// A per-view row folds `placement->conservativeParentBox(local_box)` through `parent_world`, so the box covers
    /// every orientation the placement can take - the cull above it therefore does not depend on the camera.
    Aabbd world_box{};
    /// OR of the resolved layers of this subtree.
    std::uint32_t layers_union{ 0U };
    /// This row or an ancestor resolves its placement per view: the select re-places the subtree from the view rather
    /// than reading `world`.
    bool view_dependent{ false };
    /// The subtree has NO view-independent bound, so a caller must not cull it (nor any ancestor, whose box was built
    /// from this one). Two causes: a per-view placement that can grow without a finite limit (screen scaling with no
    /// finite maximum), and a leaf that states no box at all.
    bool unbounded{ false };

    /**
     * @brief Whether @p other is the same answer, field for field.
     *
     * @details
     * - Defaulted on purpose: this comparison IS the definition of "the fold changed nothing", so every field must
     *   take part. A hand-written version needs a note telling the next author to extend it; a defaulted one cannot
     *   fall out of step with the fields.
     *
     * @param other The value to compare against.
     * @return true when every field is equal.
     */
    [[nodiscard]] bool operator==(const ResolvedRow& other) const = default;
};

/**
 * @brief WHERE ONE ROW SITS in the walk-ordered table: its parent, the end of its subtree, and its child count.
 *
 * @details
 * - THE TABLE IS THE STRUCTURE. The rows are in walk order (a row is followed by its own subtree), so these three
 *   numbers are everything a reader needs: the fold walks a child range, a select jumps to `end` to skip a pruned
 *   subtree, and nothing keeps a node map on the side.
 * - Equality is defaulted: a publication compares it to know a row's PLACE did not move (see `makeSceneSnapshot`).
 */
struct RowLinks {
    /// The parent value meaning "this row has no parent" (the table's first row).
    inline static constexpr std::uint32_t kNoParent = 0xFFFFFFFFU;

    /// 0-based row index of the parent, or `kNoParent` for the table's first row.
    std::uint32_t parent{ kNoParent };
    std::uint32_t end{ 0 };         ///< One past this row's subtree: the index a walk continues at after it.
    std::uint32_t child_count{ 0 }; ///< How many child rows this row has (an invisible child states none).

    [[nodiscard]] bool operator==(const RowLinks& other) const = default;
};

/**
 * @brief WHAT A ROW'S NODE STATES: the fold's input, read once and never asked again.
 *
 * @details
 * - IT IS ONE VALUE BECAUSE IT IS READ IN ONE PASS. `readRowOwn` builds it from a node and the row takes it as it is,
 *   so a field added here can never survive an edit because somebody forgot to extend a reset list.
 * - IT IS TWO NAMED GROUPS BECAUSE THE EDIT PATH ASKS TWO DIFFERENT QUESTIONS (see `RowRefresh`): `fold` is what a
 *   re-fold re-derives from values alone, `body` is what changes the row's meaning. A new field joins one of them
 *   deliberately, and each group compares as a whole (defaulted equality), so no hand-written field list exists
 *   anywhere in the classification.
 * - THE CONTENT VALUES ARE TAKEN WITH THE NODE (`material_description`, `program_description`, `description`): the
 *   fold copies value pointers, so nothing downstream has to ask a live object for its values on a thread that does
 *   not own it (see `.ai/design/graphics-threading.md`).
 */
struct RowOwn {
    /// The own facts a RE-FOLD re-derives from values alone (see RowRefresh::RefoldSubtree).
    ///
    /// The placement, the opacity, the layer and draw-order declarations and the view placement all feed the top-down
    /// fold, so `resolveSubtree` reaches the same answer from them as a rebuilt table would.
    struct Fold {
        Mat4d                        local{};       ///< Placement in the parent's frame (this node's own transform).
        float                        opacity{ 1.0F }; ///< This node's own multiplier, not the folded value.
        std::optional<std::uint32_t> layers{};      ///< The declaration this node states, or nullopt to inherit.
        std::optional<int>           draw_order{};  ///< The order this node declares, or nullopt to inherit.
        /// The per-view half of the placement, or null when the row is placed by `local` alone.
        ///
        /// A VALUE (see ViewPlacement): the select resolves it from the view it is drawing, which is what lets a side
        /// that owns no node draw a billboard without a graph walk.
        ViewPlacementPtr placement{};

        [[nodiscard]] bool operator==(const Fold& other) const = default;
    };

    /// The own facts that change the ROW SET rather than the fold's answer (see RowRefresh::RebuildTable).
    struct Body {
        /// A StateNode's contribution is meaningful (see `state`).
        bool owns_state{ false };
        /// The render state this node contributes; merged into the inherited one when `owns_state`.
        RenderState state{};
        /// The material this node names, or null when it names none (its draws inherit).
        MaterialValuesPtr material{};
        /// The material's values, taken with it: a material edit is a new value at the next publish, never a live
        /// re-read in the middle of a frame.
        MaterialDescriptionPtr material_description{};
        /// The program this node names, or null when it names none (its draws inherit).
        ShaderProgramPtr program{};
        /// The program's text, taken with it for the same reason the material's values are.
        ShaderProgramDescriptionPtr program_description{};
        /// The leaf's own bound as the NODE states it, in the leaf's OWN frame, when it can answer there.
        std::optional<Aabbd> local_box{};
        /// The leaf's own bound as the NODE states it, in WORLD space, for the leaves that cannot state one in their
        /// own frame (a Geometry subclass may place itself in ways only it knows - see Node::subtreeBounds).
        ///
        /// The fold converts it with the row's own placement, so every box downstream lives in one frame and the cull
        /// above the row still works. `refreshBoxes` re-reads it and compares, which is why such a row is a content-side
        /// one: a side that owns no node keeps the value it was told (see SceneBake::nodes).
        std::optional<Aabbd> world_box{};
        /// The node states no box that can be trusted: the fold marks this subtree (and every ancestor) unbounded.
        bool unbounded{ false };
        GeometryPtr geometry{}; ///< The command's payload (a share, never dereferenced here).
        /// The same geometry's shape as an immutable value (see Geometry::description): what a command hands a backend,
        /// so describing streams never reads the live object. Shared by every draw of this row, stable per revision.
        GeometryDescriptionPtr description{};
        /// ONE entry per primitive, resolved at read time (see ResolvedDraw): what the select emits, with no arithmetic.
        std::vector<ResolvedDraw> draws{};

        [[nodiscard]] bool operator==(const Body& other) const = default;
    };

    Fold fold{};
    Body body{};

    [[nodiscard]] bool operator==(const RowOwn& other) const = default;
};

/**
 * @brief ONE ROW OF A DRAW LIST: its identity, where it sits, what its node states, and what the fold resolved.
 *
 * @details
 * - THE THREE PARTS ARE THE THREE QUESTIONS A ROW ANSWERS: `links` says where it is in the table, `own` is the fold's
 *   INPUT (what the node states, in the form the select reads) and `resolved` is the fold's OUTPUT (what the select
 *   reads, and what a re-fold below this row starts from - see ResolvedRow).
 * - THE FOLD IS THE RESOLVED SIDE'S ONLY WRITER, through `storeResolved`. Nothing else may write `resolved`, which is
 *   what makes the serial below truthful.
 * - `identity` is the node id the table keys by, kept separately so a producer that has no object at all can still
 *   name what it mirrors.
 * - `resolved_serial` changes IF AND ONLY IF `resolved` did, so a publication can ask "is this row still the picture
 *   I published" with one comparison (see `SceneSnapshot`). A node's epoch cannot answer that: announcing an edit
 *   increments the edited node's and its ANCESTORS' counters (`Node::announceContentChange`), never its descendants',
 *   while an edit re-folds the whole subtree below it.
 */
struct Row {
    NodeId        identity{};
    RowLinks      links{};
    RowOwn        own{};
    ResolvedRow   resolved{};
    std::uint64_t resolved_serial{ 0 };
};

/**
 * @brief THE ROW TABLE of one scene: every node's own facts in walk order, plus the bookkeeping the incremental edit
 *   path needs.
 *
 * @details
 * - WHY IT IS SHARED. `Scene` builds the rows by walking the live graph; a producer that was handed VALUES re-folds
 *   the same rows (see `.ai/design/graphics-threading.md`). Both must run the SAME fold, so the row type, the build
 *   and the passes over it live in one place (`DrawListBake.cpp`) instead of in two implementations that would drift.
 * - WHY AN ARRAY AND NOT A MAP. The build and the select are driven by the same order (children in order, invisible
 *   subtrees pruned), so the cursor IS the identity: no node-keyed map, no hashing. `RowLinks::end` keeps that
 *   alignment when a view prunes a subtree - the select jumps straight past it.
 * - WHAT IS NOT HERE. Anything a camera decides: the frustum test, the sort key, the final list. Nor anything a
 *   PUBLICATION decides: the last snapshot and its version belong to the publisher (see `ScenePublisher`), so a
 *   table cannot disagree with itself about which publication it is current with.
 * - LIFETIME. A row holds SHARES of the content it names (geometry, material, program), so a row can never name a
 *   freed object. The table is dropped when the graph announces a change (the epoch) or the scene invalidates.
 * - WHAT IS CONTENT-SIDE ONLY. `nodes` / `node_epochs` are the build's record of which node each row came from and
 *   when it was read, used by `refreshRow` and `refreshBoxes` (a byte edit announces itself on a buffer, so the box
 *   that depends on those bytes has to be re-read). They are NOT part of the draw list: a published snapshot drops
 *   them, which is why nothing in the fold or the select reads a node.
 */
struct SceneBake {
    std::vector<Row> rows{};

    /// One per row: the node the row was read from. Content side only (a snapshot drops it).
    std::vector<const Node*> nodes{};
    /// One per row: the node's content epoch when its own facts were read (see refreshRow). Content side only.
    std::vector<std::uint64_t> node_epochs{};

    /// Opacity accumulated ABOVE the table's first row (the scene's own).
    float parent_opacity{ 1.0F };

    /// The counter the rows' `resolved_serial`s are drawn from. CARRIED across a full rebuild (see
    /// `ScenePublisher::ensureRows`): a fresh table that started over at 1 would hand a row a number a previous
    /// publication already used for a different value, and the share test would serve that different value.
    std::uint64_t next_serial{ 1 };
};

/**
 * @brief Builds a draw list's rows from a graph: every node's OWN facts, then one fold over them.
 *
 * @details
 * - It is the ONLY code that reads a graph for a draw list, and it runs on the thread that owns that graph. What it
 *   writes is values; a side that owns no graph takes them as they are (see makeSceneSnapshot).
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
 * @brief Re-resolves ONE row's subtree: the one fold, run by the build once and by a producer on every edit.
 *
 * @details
 * - Top-down it recomputes the accumulated placement and the folded values from the own facts; bottom-up it
 *   recomputes each row's local and world boxes and its subtree layer union. Only the subtree of @p index is
 *   touched, which is what makes a joint edit cost the joint's subtree instead of the whole graph.
 * - THE CHAIN ABOVE is re-derived too, one row each: a box is a UNION, so the row just re-folded is part of every
 *   ancestor's box. Skipping that leaves a box that still says the subtree is where it was, and a cull then prunes
 *   content that moved INTO view - the object silently disappears.
 * - The boxes are values (`RowOwn::Body::local_box` for a leaf, the children's union for a container), so a fold never
 *   asks a node: a producer that owns no node re-folds exactly what content does.
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
 *   over the nodes the build recorded (`SceneBake::nodes`) - never a read inside the select.
 * - Only boxes are recomputed (plus the placement transform), and only for the leaves whose answer MOVED: the
 *   refresh compares what each leaf states with what the row holds, so a streaming buffer costs its own chain, and a
 *   collection over unchanged boxes folds nothing at all. The chains of several changed leaves are deduped and folded
 *   deepest-first, because in walk order a child always sits after its parent.
 * - A SNAPSHOT has no nodes, so it does not refresh: its boxes are the publication's until a box
 *   delta exists (a known gap, not a silent read of content).
 *
 * @param bake The table to refresh.
 * @return How many rows' own box actually changed (0 when nothing moved, in which case nothing was folded).
 */
std::uint32_t refreshBoxes(SceneBake& bake);

/**
 * @brief Re-takes every row's value halves (material values, program text), and answers how many rows actually moved.
 *
 * @details
 * - THE TWO CONTENT FACTS THE GRAPH EPOCH CANNOT SEE. A geometry's shape, a node's placement and WHICH program or
 *   material a node names all change through a mutator that announces itself (`Node::announceContentChange`), which is
 *   what the epoch counts. A material's values and a program's stages are edited through LIVE OBJECTS
 *   (`Material::setXxx` / `ShaderProgram::replaceStages`) - no node is touched, so no epoch moves, and a list memoised
 *   on that epoch would keep drawing the old values forever.
 * - SO THIS IS THE PUBLISH BOUNDARY FOR VALUES, the value half of `refreshBoxes`: the content side (the only thread
 *   that may read the objects) walks its own rows, takes `MaterialValues::description()` and
 *   `ShaderProgram::description()` - a pointer compare in a steady frame, one new value when a revision moved - and
 *   reports what moved. The caller turns a non-zero answer into a version bump, and every consumer keyed on that
 *   version (the list memo, a mirror) then re-derives.
 * - ONE FUNCTION FOR BOTH because they share the rule (no epoch, new value at the next publication) and the version a
 *   publication compares; splitting them would be two spellings of one boundary (G1).
 * - A MIRROR has no live objects (see the class note), so it does not refresh: its rows carry the values they were
 *   published with, which is exactly the one-publication lag every content fact has.
 *
 * @param bake The table to refresh; its rows are edited in place.
 * @return How many rows' values changed (0 when nothing moved).
 */
std::uint32_t refreshValues(SceneBake& bake);

/**
 * @brief What re-reading one row's own facts found, and therefore what the table needs.
 *
 * @details
 * - The three answers are the three costs: nothing, one subtree re-folded, or a rebuilt table. They are named for the
 *   ACTION the caller takes rather than for the field that moved, so an answer cannot be mistaken for evidence.
 */
enum class RowRefresh
{
    NoChange,      ///< The row's own facts are the same (an ancestor's epoch moved, nothing below did).
    RefoldSubtree, ///< Only `RowOwn::Fold` moved; the fold can re-derive the row's subtree in place.
    RebuildTable   ///< `RowOwn::Body` moved; the caller must rebuild the table from the graph.
};

/**
 * @brief Re-reads one row's own facts from the node the table recorded, for the incremental edit path.
 *
 * @details
 * - IT READS THE NODE THE TABLE KNOWS (`bake.nodes[index]`), never one handed in beside an index: the row and its
 *   source cannot disagree, so there is no "wrong node" answer to defend against.
 * - It never touches `links` or `resolved`: the structure is the caller's to rebuild when it changes, and the
 *   resolved side is the fold's to write (see `resolveSubtree`).
 * - The classification is asked of the two own GROUPS (`RowOwn::Fold` / `RowOwn::Body`), not of a hand-written field
 *   list, so a field added to `RowOwn` joins the classification by construction.
 *
 * @param bake  The table to refresh.
 * @param index The row to refresh.
 * @return What the caller has to do (see RowRefresh).
 */
RowRefresh refreshRow(SceneBake& bake, std::uint32_t index);

/**
 * @brief ONE command's place in the order a frame draws it: THE total order `sortCollected` sorts by.
 *
 * @details
 * - A VALUE, compared field by field, in the order the fields are declared - so "why is this drawn before that"
 *   is answered by reading the struct (`G4` / `G11`):
 *   1. @ref draw_order - the content's own declaration (`Node::setDrawOrder`); a higher value is drawn later.
 *   2. @ref layer - the SMALLEST layer index the row belongs to (`Node::setLayers`): a pass that selects several
 *      layers still has one deterministic order among them.
 *   3. @ref transparent - the opaque segment first, the transparent segment after it.
 *   4. @ref depth - CAMERA-SPACE depth of the representative point, quantized: front-to-back for the opaque segment,
 *      back-to-front for the transparent one.
 *   5. @ref tie - the row index in the publication and the primitive index inside it: the last key exists so the
 *      order is TOTAL, not merely stable - any permutation of the same commands sorts to the same list (`R17`, `T9`).
 * - THE REPRESENTATIVE POINT is the world box CENTER when the row has a box, and the node's origin only when it has
 *   none: a mesh whose vertices sit far from its local origin (CAD parts, baked world coordinates) would otherwise
 *   sort by a point that is not on it - the defect `.ai/bugs/graphics-transparent-sort-uses-origin.md` records.
 * - DEPTH IS CAMERA-SPACE, not a Euclidean distance: the sort then agrees with the rasterized depth at the edge of
 *   the field of view, where the two disagree about "which is in front". It is quantized (see @ref kDepthStep) so
 *   that two runs on different machines break ties the same way.
 * - This is still an OBJECT-LEVEL approximation: it does not order self-intersections, non-convex meshes or
 *   concentric shells; content that needs more states it with `setDrawOrder` or splits its geometry
 *   (see `graphics-scene.md` N10).
 */
struct SortKey {
    std::int32_t  draw_order{ 0 };      ///< Declared draw order; a higher value is drawn later.
    std::uint32_t layer{ 0 };           ///< Smallest layer index the row belongs to (0 when it states none).
    bool          transparent{ false }; ///< Opaque segment first, transparent segment after it.
    std::int64_t  depth{ 0 };           ///< Quantized camera-space depth (near = larger, see makeSortKey).
    std::uint64_t tie{ 0 };             ///< (row index << 32) | primitive index: what makes the order total.

    /// @brief Depth quantization: one step is 1/4096 of a world unit.
    static constexpr double kDepthStep = 4096.0;

    /** @brief Gets whether two keys name the same place in the order. */
    friend bool operator==(const SortKey& lhs, const SortKey& rhs) noexcept = default;

    /**
     * @brief Orders two keys (see the struct's note for the field order and for what each field means).
     *
     * @param lhs First key.
     * @param rhs Second key.
     * @return true when @p lhs is drawn before @p rhs.
     */
    friend bool operator<(const SortKey& lhs, const SortKey& rhs) noexcept
    {
        if (lhs.draw_order != rhs.draw_order) {
            return lhs.draw_order < rhs.draw_order;
        }
        if (lhs.layer != rhs.layer) {
            return lhs.layer < rhs.layer;
        }
        if (lhs.transparent != rhs.transparent) {
            return !lhs.transparent;
        }
        if (lhs.depth != rhs.depth) {
            return lhs.transparent ? lhs.depth > rhs.depth : lhs.depth < rhs.depth;
        }
        return lhs.tie < rhs.tie;
    }
};

/**
 * @brief Builds the sort key of one command (see SortKey).
 *
 * @param command  The command the key describes (its draw order and transparency are read from it).
 * @param layers   The layers of the ROW the command came from (its smallest bit is the key's layer).
 * @param box      The row's world box this view resolved (or the folded one); an invalid box falls back to the
 *                 command's own matrix origin.
 * @param view     The world-to-view matrix of the view being drawn (camera-space depth is measured with it).
 * @param tie      The row-and-primitive index that makes the order total.
 * @return The key.
 */
SortKey makeSortKey(const DrawItem& command, std::uint32_t layers, const Aabbd& box, const Mat4d& view, std::uint64_t tie);

/**
 * @brief Reorders @p commands into the order a frame draws them in, in place.
 *
 * @details
 * - The order is the TOTAL order @ref SortKey states: declared draw order, then layer, then the opaque segment before
 *   the transparent one, then quantized camera-space depth (front-to-back opaque, back-to-front transparent), and
 *   finally the row/primitive index. Because the last field is unique, no two commands compare equal - so any
 *   permutation of the same commands sorts to the same list and `std::sort` (not `stable_sort`) is correct.
 * - It is the last step of `selectRows`, not a second interface: the keys are built where the row facts are, and no
 *   producer owns a second copy of the list.
 *
 * @param commands Commands to reorder (replaced by their sorted order).
 * @param keys     The key of every command, in the commands' current order. Must have one entry per command.
 */
void sortCollected(std::vector<DrawItem>& commands, std::span<const SortKey> keys);

/**
 * @brief ONE VIEW'S SELECTION: the commands in the order the frame draws them, and what the select had to know.
 *
 * @details
 * - SORTED, NOT WALK ORDER: `selectRows` fills the commands while it walks the rows and then applies the total order
 *   `SortKey` states, so a caller never has to know which of the two orders it is holding.
 * - `viewport_used` records whether a per-view placement resolved against the device-pixel viewport. A caller that
 *   memoises the list has to key it by the viewport too when it is true, and may share it across rectangles when it
 *   is false (see DrawListMemo).
 */
struct Selection {
    std::vector<DrawItem> commands{};             ///< The commands, in draw order (see SortKey).
    bool                  viewport_used{ false }; ///< A per-view row resolved against the device-pixel viewport.
};

/**
 * @brief Selects one view's commands from a walk-ordered row range, through an accessor.
 *
 * @details
 * - THE WHOLE VIEW HALF IN ONE CALL: layer prune, frustum cull, per-view placement, command construction and the
 *   total order. A caller asks a view one question and receives the list a frame draws.
 * - The accessor is what lets the one algorithm run over a flat `SceneBake` and over the chunked `SceneSnapshot`
 *   without materialising either into the other's layout; `row_at(index)` must be stable for the duration of the
 *   call.
 * - A pruned subtree is skipped by jumping to its `RowLinks::end`, so the walk visits only the rows a view can draw.
 *
 * @tparam RowAt Callable `const Row&(std::uint32_t)`.
 *
 * @param row_count Number of rows to walk.
 * @param row_at    Accessor answering one row.
 * @param frustum   The view's frustum.
 * @param view      The view being drawn (its layer mask is the content selection).
 * @return The commands, in draw order, and whether the viewport took part (see Selection).
 */
template <class RowAt>
[[nodiscard]] Selection selectRows(std::uint32_t row_count, RowAt row_at, const Frustum& frustum, const View& view)
{
    // The rows are in walk order, so the rows currently being resolved form the ancestor chain, deepest last. A row
    // whose placement was resolved for THIS view hands its world down; every other row is already placed by the fold.
    struct OpenRow {
        std::uint32_t end{ 0 };          ///< One past the row's subtree: it stops being an ancestor there.
        bool          resolved{ false }; ///< Its world was resolved from the view (children must compose with it).
        Mat4d         world{};           ///< The world the row resolved to.
    };
    // A TREE'S DEPTH, not a scene's size: the chain of rows currently being resolved. The inline capacity covers an
    // ordinary assembly, and the vector in the repo only grows past it for a pathological hierarchy - which is why this
    // is a SmallVector and not a std::vector: a collection happens per view per frame, and it must not allocate.
    vn::SmallVector<OpenRow, 32U> open;

    Selection            selection;
    std::vector<SortKey> keys;
    selection.commands.reserve(row_count);
    keys.reserve(row_count);

    for (std::uint32_t index = 0; index < row_count;) {
        while (!open.empty() && open.back().end <= index) {
            open.pop_back();
        }
        const Row&  row             = row_at(index);
        const bool  parent_resolved = !open.empty() && open.back().resolved;
        const bool  in_run          = row.resolved.view_dependent || parent_resolved;
        const Mat4d parent_world    = parent_resolved ? open.back().world : row.resolved.parent_world;

        Mat4d world    = row.resolved.world;
        bool  resolved = false;
        if (row.own.fold.placement != nullptr) {
            const ViewDependence dependence = localToWorldForView(*row.own.fold.placement, parent_world, view, world);
            if (dependence == ViewDependence::ViewAndViewport) {
                selection.viewport_used = true;
            }
            if (dependence == ViewDependence::None) {
                world = parent_world * row.own.fold.local; // this view keeps the node's own placement
            }
            resolved = true;
        }
        else if (in_run) {
            world    = parent_world * row.own.fold.local; // an ancestor was re-placed, so this row's folded world is stale
            resolved = true;
        }

        if ((row.resolved.layers_union & view.layer_mask) == 0U) {
            index = row.links.end;
            continue;
        }
        // A row with no view-independent bound must not be culled: no box can promise it is out of the frustum.
        if (!row.resolved.unbounded &&
            frustum.isOutside(resolved ? row.resolved.local_box.transformed(world) : row.resolved.world_box)) {
            index = row.links.end;
            continue;
        }
        if (resolved) {
            open.push_back(OpenRow{ row.links.end, true, world });
        }
        if (!row.own.body.draws.empty()) {
            const float effective = std::clamp(row.resolved.opacity, 0.0f, 1.0f);
            // The box the key's representative point is taken from: the one this view resolved for a per-view row,
            // otherwise the box the fold published.
            const Aabbd sort_box = resolved ? row.resolved.local_box.transformed(world) : row.resolved.world_box;
            for (const ResolvedDraw& draw : row.own.body.draws) {
                if (!draw.drawn) {
                    continue;
                }
                const MaterialValuesPtr material =
                    draw.material != nullptr ? draw.material : row.resolved.material;
                const MaterialDescriptionPtr material_description =
                    draw.material_description != nullptr ? draw.material_description : row.resolved.material_description;
                const ShaderProgramPtr program = draw.program != nullptr ? draw.program : row.resolved.program;
                const ShaderProgramDescriptionPtr program_description =
                    draw.program_description != nullptr ? draw.program_description : row.resolved.program_description;
                auto&                  cmd     = selection.commands.emplace_back(row.own.body.geometry, material, world, draw.primitive_index);
                cmd.node                       = row.identity;
                cmd.description                = row.own.body.description;
                cmd.geometry_ref               = contentRefOf(row.identity, row.own.body.description != nullptr ? row.own.body.description->revision() : 0U);
                cmd.material_description       = material_description;
                cmd.material_ref               = material_description != nullptr
                                                     ? ContentRef{ material != nullptr ? material->id().handle() : Handle{},
                                                                   material_description->revision() }
                                                     : ContentRef{};
                cmd.opacity                    = effective;
                cmd.draw_order                 = row.resolved.draw_order;
                cmd.authored_state             = row.resolved.state;
                cmd.program                    = program;
                cmd.program_description        = program_description;
                cmd.instance_transforms        = draw.instance_transforms;
                cmd.first_instance             = draw.first_instance;
                cmd.instance_count             = draw.instance_count;
                keys.push_back(makeSortKey(selection.commands.back(),
                                           row.resolved.layers,
                                           sort_box,
                                           view.camera.view,
                                           (static_cast<std::uint64_t>(index) << 32U) | draw.primitive_index));
            }
        }
        ++index;
    }

    // THE DRAW ORDER: the walk order is not the draw order. One reordering, here, so the result is a property of the
    // commands rather than of the walk that collected them (see SortKey and sortCollected).
    sortCollected(selection.commands, keys);
    return selection;
}

} // namespace detail

VN_GRAPHICS_NS_END
