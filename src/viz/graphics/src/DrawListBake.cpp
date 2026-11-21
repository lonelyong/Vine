#include "DrawListBake.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

namespace
{

/// @brief ONE leaf's own bound as the node states it, and the frame it stated it in.
///
/// @details
/// - The engine's leaf answers in its own frame (`Geometry::subtreeBounds`), and so does any node that can; that box is
///   what every ancestor is folded from, and it survives a placement edit untouched.
/// - A leaf the engine cannot see inside (a Geometry subclass that places itself in ways only it knows) answers in
///   WORLD space only. That answer is kept as a value and the FOLD converts it (see foldRow) - it cannot be converted
///   here, because the row's own placement is what the fold computes.
/// - A leaf that cannot state a bound at all is UNCULLABLE rather than contributing an empty box, which would quietly
///   drop everything above it.
struct LeafBox {
    std::optional<Aabbd> local{};            ///< The answer in the leaf's own frame, when it stated one there.
    std::optional<Aabbd> world{};            ///< The answer in world space, when that is the only frame it can use.
    bool                 unbounded{ false }; ///< No box can be trusted for this leaf.
};

/// @brief Reads @p node's own bound.
///
/// @param node The leaf to read.
/// @return The box and the frame it was stated in (see LeafBox).
LeafBox readLeafBox(const Node& node)
{
    Aabbd local = Aabbd::empty();
    if (node.subtreeBounds(local)) {
        return LeafBox{ local, std::nullopt, false }; // the engine's own leaf: already in its own frame
    }
    const Aabbd world = node.boundingBox();
    if (!world.isValid()) {
        return LeafBox{ std::nullopt, std::nullopt, true }; // nothing to cull by
    }
    return LeafBox{ std::nullopt, world, false };
}

/// @brief Writes @p box into @p body and says whether that changed anything the fold depends on.
///
/// @details
/// - It writes only the body side. The resolved box is the FOLD's derivation (see foldRow), which is what keeps
///   `Row::resolved` a value with one writer.
///
/// @param body The leaf's own body facts.
/// @param box  The bound read from the node.
/// @return true when the own box or its cullability moved, so the rows above it have to be folded again.
bool recordLeafBox(RowOwn::Body& body, const LeafBox& box)
{
    if (body.local_box == box.local && body.world_box == box.world && body.unbounded == box.unbounded) {
        return false; // nothing moved: write nothing, so the fold's previous answer stays exactly as it was
    }
    body.local_box = box.local;
    body.world_box = box.world;
    body.unbounded = box.unbounded;
    return true;
}

/// @brief Stores the fold's answer for one row, moving the row's serial only when the answer actually moved.
///
/// @details
/// - THE ONLY WAY THE RESOLVED SIDE IS WRITTEN, which is what makes `Row::resolved_serial` truthful: a row whose
///   re-fold produced the same facts keeps the number it was published with, so its chunk can be shared - and a row
///   whose facts moved gets a new one, so its chunk cannot (see `SceneSnapshot::makeSceneSnapshot`).
/// - Comparing BEFORE storing is the point, not an optimisation: the incremental path re-derives a whole subtree for
///   one edit, and most of that subtree usually did not move. Without the comparison a single edit would cost the
///   sharing of every chunk it touched.
///
/// @param bake The table the answer belongs to (its counter numbers the rows).
/// @param row  The row to store into.
/// @param next The facts the fold just derived; consumed.
void storeResolved(SceneBake& bake, Row& row, ResolvedRow&& next) noexcept
{
    if (row.resolved == next) {
        return;
    }
    row.resolved        = std::move(next);
    row.resolved_serial = bake.next_serial++;
}

/// @brief Replays the fold for one row given what it inherits, and recurses into its children.
///
/// @param bake   The table being folded.
/// @param index  The row to fold (the root of the subtree being re-derived).
/// @param parent What the row inherits: the PARENT's resolved row, verbatim (see ResolvedRow).
void foldDown(SceneBake& bake, std::uint32_t index, const ResolvedRow& parent)
{
    Row& row = bake.rows[index];

    // THE ROW INHERITS EVERYTHING ITS PARENT HOLDS, then applies what it states itself - so a container's answer is
    // the whole seed a re-fold below it needs. The BOX half is the row's OWN (a leaf's own bound, or the union the
    // last fold derived): it is re-derived bottom-up by foldRow, so it is taken from the row, never from the parent.
    ResolvedRow next  = parent;
    next.local_box    = row.resolved.local_box;
    next.world_box    = row.resolved.world_box;
    next.layers_union = row.resolved.layers_union;
    next.unbounded    = row.resolved.unbounded;

    next.world          = parent.world * row.own.fold.local;
    next.parent_world   = parent.world;
    next.opacity        = parent.opacity * row.own.fold.opacity;
    next.layers         = row.own.fold.layers.value_or(parent.layers);
    next.draw_order     = row.own.fold.draw_order.value_or(parent.draw_order);
    next.view_dependent = parent.view_dependent || row.own.fold.placement != nullptr;
    if (row.own.body.owns_state) {
        next.state.merge(row.own.body.state);
        if (row.own.body.material != nullptr) {
            next.material             = row.own.body.material;
            next.material_description = row.own.body.material_description;
        }
        if (row.own.body.program != nullptr) {
            next.program             = row.own.body.program;
            next.program_description = row.own.body.program_description;
        }
    }
    // KEPT WHETHER OR NOT THE ROW DRAWS. A container's row states no command of its own, but it is what a re-fold
    // BELOW it reads back as the seed (see resolveSubtree): storing only for rows with draws is what used to drop
    // every inherited state, material and program of an edited subtree.
    storeResolved(bake, row, std::move(next));

    const ResolvedRow& inherited = row.resolved;
    for (std::uint32_t cursor = index + 1; cursor < row.links.end;) {
        const std::uint32_t child_end = bake.rows[cursor].links.end;
        foldDown(bake, cursor, inherited);
        cursor = child_end;
    }
}

/// @brief Recomputes ONE row's boxes and union from its children's rows, which must already be folded.
///
/// @details
/// - A box climbs: a row's own box is its leaf answer or the union of its children's, so the fold is bottom-up and
///   stops nowhere in particular. Keeping the one-row step separate is what lets an edit re-derive the EDITED subtree
///   in full and then walk the ancestors, each with this step alone (see resolveSubtree) - instead of re-walking the
///   ancestors' whole subtrees, which is what a per-frame edit would pay on a robot leg.
///
/// @param bake  The table containing @p index.
/// @param index The row to re-derive (its children are read, never written).
void foldRow(SceneBake& bake, std::uint32_t index)
{
    const std::uint32_t end = bake.rows[index].links.end;
    Row&                row = bake.rows[index];

    // The own-frame box is DERIVED here - the resolved side has one writer. A leaf states its bound in its own frame
    // (read at build time, or re-read by refreshBoxes when the bytes under it announce) or, failing that, in world
    // space; a container is the union of its children. A per-view child answers the union through its placement
    // instead of through `local`, because the orientation it may choose is not the axis-aligned one its own box was
    // measured in (see ViewPlacement::conservativeParentBox).
    ResolvedRow next      = row.resolved;
    Aabbd       local     = Aabbd::empty();
    bool        unbounded = row.own.body.unbounded;
    if (row.links.child_count == 0 && row.own.body.local_box.has_value()) {
        local = *row.own.body.local_box;
    }
    else if (row.links.child_count == 0 && row.own.body.world_box.has_value()) {
        // A leaf that could only answer in world space: undo this row's own placement so the row holds the same kind
        // of box every other row does. The AABB of the transformed corners is a SUPERSET for a rotated placement,
        // which errs on the side of drawing. This is the only place the conversion can happen - it needs `world`,
        // which is the fold's own answer.
        Mat4d inverse = next.world;
        if (inverse.invert()) {
            local = row.own.body.world_box->transformed(inverse);
        }
        else {
            unbounded = true; // a degenerate placement cannot be undone, so no box can be trusted
        }
    }

    std::uint32_t union_of_layers = next.layers;
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].links.end;
        const Row&          child     = bake.rows[cursor];
        const Aabbd         placed    = child.own.fold.placement != nullptr
                                            ? child.own.fold.placement->conservativeParentBox(child.resolved.local_box)
                                            : child.resolved.local_box.transformed(child.own.fold.local);
        if (placed.isValid()) {
            local.expandBy(placed);
        }
        union_of_layers |= child.resolved.layers_union;
        unbounded |= child.resolved.unbounded;
        cursor = child_end;
    }
    next.local_box = local;
    // The world box covers EVERY view, so a cull above this row never depends on the camera: a per-view row folds
    // the parent-frame bound through the parent's placement; an ordinary row folds its own box through its own.
    next.world_box = row.own.fold.placement != nullptr
                         ? row.own.fold.placement->conservativeParentBox(local).transformed(next.parent_world)
                         : local.transformed(next.world);

    // A subtree whose bound cannot be stated without a view must not be culled anywhere on the way down: the flag
    // climbs with the boxes it invalidates. It is DERIVED, never carried: a leaf that gains a box later can become
    // cullable again.
    next.unbounded    = unbounded || (row.own.fold.placement != nullptr && !row.own.fold.placement->isBounded());
    next.layers_union = union_of_layers;
    storeResolved(bake, row, std::move(next));
}

/// @brief Recomputes one subtree's boxes and layer unions bottom-up.
void foldUp(SceneBake& bake, std::uint32_t index)
{
    const std::uint32_t end = bake.rows[index].links.end;
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].links.end;
        foldUp(bake, cursor);
        cursor = child_end;
    }
    foldRow(bake, index);
}

/// @brief Reads @p node's own facts: the row's half of the fold, and the only place a node is read for one.
///
/// @details
/// - A FRESH VALUE, never a merge: the caller stores it as it is, so a field added to `RowOwn` can never survive an
///   edit because somebody forgot to extend a reset list.
/// - A drawable's draws are resolved here - its per-primitive material and program, the visible instance range -
///   because this is the one place the geometry is read: a publication then carries the answer and never touches the
///   live object.
/// - The leaf's bound is read here too (see readLeafBox); a container states none, because its box is the union the
///   fold derives from its children.
///
/// @param node The node to read.
/// @return The node's own facts (see RowOwn).
RowOwn readRowOwn(const Node& node)
{
    RowOwn own;
    own.fold.local      = node.localTransformMatrix();
    own.fold.opacity    = node.opacity();
    own.fold.layers     = node.layers();
    own.fold.draw_order = node.drawOrder();
    own.fold.placement  = node.viewPlacement();

    if (const auto* state_node = obj_cast<StateNode>(&node)) {
        own.body.owns_state = true;
        own.body.state      = state_node->renderState();
        if (raw_ptr<MaterialValues> material = state_node->material()) {
            own.body.material = material;
            // The values are taken WITH the identity, here, on the content side: the fold below only copies the value
            // pointer, which is what lets a side that owns no node resolve a material without reading the live object.
            own.body.material_description = material->description();
        }
        if (raw_ptr<ShaderProgram> program = state_node->program()) {
            own.body.program = program;
            // Its text travels with it, for the same reason the material's values do (see the fold's program copy).
            own.body.program_description = program->description();
        }
    }
    if (const auto* geometry = obj_cast<Geometry>(&node)) {
        own.body.geometry    = GeometryPtr(const_cast<Geometry*>(geometry));
        own.body.description = geometry->description();
        const raw_ptr<MaterialValues> geometry_material = geometry->material();
        const raw_ptr<ShaderProgram>  geometry_program  = geometry->program();
        const std::size_t             primitive_count   = geometry->primitiveCount();
        own.body.draws.reserve(primitive_count);
        for (std::size_t ordinal = 0; ordinal < primitive_count; ++ordinal) {
            const Primitive* primitive = geometry->primitive(ordinal);
            if (primitive == nullptr) {
                continue;
            }
            ResolvedDraw draw;
            draw.primitive_index = static_cast<std::uint32_t>(ordinal);
            draw.material        = primitive->material != nullptr
                                       ? MaterialValuesPtr(primitive->material)
                                       : (geometry_material != nullptr ? MaterialValuesPtr(geometry_material) : MaterialValuesPtr());
            draw.material_description =
                draw.material != nullptr ? draw.material->description() : MaterialDescriptionPtr();
            draw.program         = primitive->program != nullptr
                                       ? ShaderProgramPtr(primitive->program)
                                       : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : ShaderProgramPtr());
            draw.program_description = draw.program != nullptr ? draw.program->description() : ShaderProgramDescriptionPtr();
            draw.drawn = !(primitive->hasInstances() && primitive->instanceCount() == 0u);
            if (primitive->hasInstances()) {
                draw.instance_transforms = primitive->instance_transforms;
                draw.first_instance =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->firstInstance(), std::numeric_limits<std::uint32_t>::max()));
                draw.instance_count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->instanceCount(), std::numeric_limits<std::uint32_t>::max()));
            }
            own.body.draws.push_back(std::move(draw));
        }
        (void)recordLeafBox(own.body, readLeafBox(node));
    }
    else if (obj_cast<Group>(&node) == nullptr) {
        // A leaf the engine does not own: the same rule (see readLeafBox).
        (void)recordLeafBox(own.body, readLeafBox(node));
    }

    return own;
}

std::uint32_t bakeSubtree(const Node& node, SceneBake& bake, std::uint32_t parent = RowLinks::kNoParent)
{
    if (!node.isVisible()) {
        return static_cast<std::uint32_t>(bake.rows.size());
    }

    const std::uint32_t index = static_cast<std::uint32_t>(bake.rows.size());
    bake.rows.emplace_back();
    bake.nodes.push_back(&node);
    bake.node_epochs.push_back(node.contentEpoch());

    {
        Row& row         = bake.rows[index];
        row.identity     = node.id();
        row.links.parent = parent;
        row.own          = readRowOwn(node);
    }

    std::uint32_t children = 0U;
    if (const auto* group = obj_cast<Group>(&node)) {
        for (const auto& child : group->children()) {
            (void)bakeSubtree(*child, bake, index);
            ++children;
        }
    }

    Row& row              = bake.rows[index];
    row.links.end         = static_cast<std::uint32_t>(bake.rows.size());
    row.links.child_count = children;
    return row.links.end;
}

} // namespace

RowRefresh refreshRow(SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size() || index >= bake.nodes.size() || bake.nodes[index] == nullptr) {
        return RowRefresh::RebuildTable;
    }

    Row&         row  = bake.rows[index];
    const RowOwn next = readRowOwn(*bake.nodes[index]);

    // THE ONE QUESTION: with the row's BODY unchanged the fold can reach the new value by re-deriving this row's
    // subtree; a body change is what only a rebuilt table can carry (see RowRefresh). Asked of the two named groups,
    // never of a field list, so a new field lands on one side of the line deliberately.
    RowRefresh answer = RowRefresh::NoChange;
    if (row.own != next) {
        answer = row.own.body == next.body ? RowRefresh::RefoldSubtree : RowRefresh::RebuildTable;
    }
    row.own = next;
    return answer;
}

void buildRows(const Node& root, SceneBake& bake, float opacity)
{
    bake.rows.clear();
    bake.nodes.clear();
    bake.node_epochs.clear();
    bake.parent_opacity = opacity;
    (void)bakeSubtree(root, bake);
    (void)resolveSubtree(bake, 0U);
}

std::uint32_t resolveSubtree(SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size()) {
        return 0U;
    }
    const std::uint32_t first_row = index;
    const std::uint32_t last_row  = bake.rows[index].links.end;

    // WHAT THIS SUBTREE INHERITS is the parent row's OWN resolved answer, read back verbatim: that is why the fold
    // stores the inherited state / material / program on EVERY row and not only on the rows that draw (see
    // foldDown). The table's first row inherits nothing but the scene's opacity.
    ResolvedRow parent;
    if (bake.rows[index].links.parent == RowLinks::kNoParent) {
        parent.opacity = bake.parent_opacity;
    }
    else {
        parent = bake.rows[bake.rows[index].links.parent].resolved;
    }
    foldDown(bake, index, parent);
    foldUp(bake, index);
    // THE CHAIN ABOVE. A box is a union, so the row just re-folded is part of every ancestor's box: leaving them
    // alone keeps a box that says the subtree is still where it was, and a cull then prunes content that moved INTO
    // view - the object silently disappears (a subset of the rows is stale, which no assertion in the edit path can
    // see). Each ancestor needs its OWN row recomputed and nothing more: its children are already folded, so one
    // `foldRow` per ancestor is the whole replay.
    for (std::uint32_t above = bake.rows[index].links.parent; above != RowLinks::kNoParent; above = bake.rows[above].links.parent) {
        foldRow(bake, above);
    }

    return last_row - first_row;
}

std::uint32_t refreshBoxes(SceneBake& bake)
{
    if (bake.rows.empty()) {
        return 0U;
    }
    // The boxes are the one part of the rows that a BYTE edit can stale without a delta, and the freshness key is the
    // geometry's own (its channels' revisions and their slices - see Geometry::localBounds). Asking a LEAF for its
    // own box is a cache hit unless something it reads announced; a container needs no ask at all (its box is the
    // union its children already answered), and a MIRROR has no nodes and therefore refreshes nothing (its boxes are
    // the handshake's until a box delta exists - see the declaration).
    std::vector<std::uint32_t> changed;
    const std::size_t          count = std::min(bake.rows.size(), bake.nodes.size());
    for (std::size_t index = 0; index < count; ++index) {
        const Node* node = bake.nodes[index];
        if (node == nullptr || bake.rows[index].links.child_count != 0) {
            continue;
        }
        if (recordLeafBox(bake.rows[index].own.body, readLeafBox(*node))) {
            changed.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (changed.empty()) {
        return 0U; // NOTHING moved: a collection over unchanged boxes folds nothing at all
    }
    const std::uint32_t refreshed = static_cast<std::uint32_t>(changed.size());
    // A changed box belongs to every box above it, so those rows have to be re-derived - one row each, from children
    // that are already folded. The chains of several changed leaves overlap; the dedupe is what keeps a streaming
    // buffer's cost at its own chain instead of the whole table.
    const std::size_t leaves = changed.size();
    for (std::size_t position = 0; position < leaves; ++position) {
        for (std::uint32_t above = bake.rows[changed[position]].links.parent; above != RowLinks::kNoParent;
             above = bake.rows[above].links.parent) {
            changed.push_back(above);
        }
    }
    // Deepest first: in walk order a child always sits after its parent, so folding in DESCENDING row index means
    // every row is folded after the children it reads.
    std::sort(changed.begin(), changed.end(), [](std::uint32_t lhs, std::uint32_t rhs) { return lhs > rhs; });
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (const std::uint32_t row : changed) {
        foldRow(bake, row);
    }
    return refreshed;
}

namespace
{

/**
 * @brief Re-takes one live object's immutable description, storing it only when it moved.
 *
 * @details
 * - A material's values and a program's text are edited through live objects, so no node mutator announces them and
 *   the graph epoch cannot see the change - while the values a command carries do change (see refreshValues).
 * - The slot keeps its old value when the object is null or its description is unchanged, which is what keeps a
 *   publication from claiming an edit nobody made.
 *
 * @param object The live object, or null when the slot holds none.
 * @param slot   The description the row carries; receives the new one when it moved.
 * @return true when @p slot changed.
 */
template <typename Pointer, typename Description>
bool retakeDescription(const Pointer& object, Description& slot)
{
    if (object == nullptr) {
        return false;
    }
    Description next = object->description();
    if (next == slot) {
        return false;
    }
    slot = std::move(next);
    return true;
}

} // namespace

std::uint32_t refreshValues(SceneBake& bake)
{
    // The value halves the fold copies are re-taken here: a material's values and a program's text are edited through
    // live objects, so no node mutator announces them and the graph epoch cannot see the change - while the values a
    // command carries do change. One function for both because they share the rule (no epoch, new value at the next
    // publication) and the same version a publication compares.
    std::uint32_t moved = 0U;
    for (Row& row : bake.rows) {
        bool row_moved = false;
        // Each re-taken value is stored through the fold's one writer, so a row whose command value moved is not
        // served from the previous publication (see Row::resolved_serial). The resolved side is compared BEFORE the
        // copy storeResolved needs, so a row nobody edited costs no copy here.
        if (row.resolved.material != nullptr) {
            const MaterialDescriptionPtr values = row.resolved.material->description();
            if (values != row.resolved.material_description) {
                ResolvedRow next          = row.resolved;
                next.material_description = values;
                storeResolved(bake, row, std::move(next));
                row_moved = true;
            }
        }
        if (row.resolved.program != nullptr) {
            const ShaderProgramDescriptionPtr text = row.resolved.program->description();
            if (text != row.resolved.program_description) {
                ResolvedRow next         = row.resolved;
                next.program_description = text;
                storeResolved(bake, row, std::move(next));
                row_moved = true;
            }
        }
        if (retakeDescription(row.own.body.material, row.own.body.material_description)) {
            row_moved = true;
        }
        if (retakeDescription(row.own.body.program, row.own.body.program_description)) {
            row_moved = true;
        }
        for (ResolvedDraw& draw : row.own.body.draws) {
            if (retakeDescription(draw.material, draw.material_description)) {
                row_moved = true;
            }
            if (retakeDescription(draw.program, draw.program_description)) {
                row_moved = true;
            }
        }
        if (row_moved) {
            ++moved;
        }
    }
    return moved;
}

SortKey makeSortKey(const DrawItem& command, std::uint32_t layers, const Aabbd& box, const Mat4d& view, std::uint64_t tie)
{
    SortKey key;
    key.draw_order  = command.draw_order;
    key.transparent = command.isTransparent();
    key.tie         = tie;
    // The SMALLEST layer index the row belongs to; a row that states no layers belongs to the default one (bit 0).
    key.layer = layers == 0U ? 0U : static_cast<std::uint32_t>(std::countr_zero(layers));

    // THE POINT THE PICTURE IS SORTED BY: the center of the world box the row already carries (the fold computed it), and
    // the node's own origin only when the row has NO box - see .ai/bugs/graphics-transparent-sort-uses-origin.md.
    const vn::math::Point3d origin = command.model_matrix * vn::math::Point3d(0.0, 0.0, 0.0);
    const vn::math::Point3d point  = box.isValid() ? box.center() : origin;
    // CAMERA-SPACE depth, not a Euclidean distance: the order then agrees with the depth the rasterizer writes at the edge
    // of the field of view. A camera looks down its own -z, so a smaller view-space z is FARTHER.
    constexpr double kMaxSortDepth = 1.0e9; // far enough for any scene in world units; keeps the ticks inside int64
    const double     depth         = -(view * point).z;
    const double     clamped       = std::isfinite(depth) ? std::clamp(depth, -kMaxSortDepth, kMaxSortDepth) : 0.0;
    key.depth                      = static_cast<std::int64_t>(std::llround(clamped * SortKey::kDepthStep));
    return key;
}

void sortCollected(std::vector<DrawItem>& commands, std::span<const SortKey> keys)
{
    // One key per command, in the commands' current order (see selectRows). A mismatch is a broken caller, not a case
    // to recover from: answering the walk order would hide the defect behind a picture that merely sorts differently.
    assert(keys.size() == commands.size());
    if (keys.size() != commands.size()) {
        return;
    }
    // NO EQUAL KEYS (the tie field is unique per command), so a plain sort is deterministic and no stability is needed - the
    // order is a property of the commands, not of the walk that collected them (R17 / T9).
    std::vector<std::uint32_t> order(commands.size());
    for (std::uint32_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&keys](std::uint32_t lhs, std::uint32_t rhs) { return keys[lhs] < keys[rhs]; });

    std::vector<DrawItem> ordered;
    ordered.reserve(commands.size());
    for (const std::uint32_t index : order) {
        ordered.push_back(std::move(commands[index]));
    }
    commands = std::move(ordered);
}

} // namespace detail

VN_GRAPHICS_NS_END
