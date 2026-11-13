#include "DrawListBake.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

namespace
{

/// @brief The values a row inherits from its parent (the fold's input as it descends).
struct Inherited {
    Mat4d            world{};      // the parent's accumulated placement (identity for the table's first row)
    float            opacity{ 1.0F };
    std::uint32_t    layers{ 1U };
    std::int32_t     draw_order{ 0 };
    RenderState      state{};
    MaterialBasePtr  material{};
    MaterialDescriptionPtr material_description{};
    ShaderProgramPtr program{};
    ShaderProgramDescriptionPtr program_description{};
    bool             view_dependent{ false };
};

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
    Aabbd                local{ Aabbd::empty() }; ///< In the leaf's own frame; empty when the answer was `world`.
    std::optional<Aabbd> world{};                 ///< The node's answer in world space, when it stated one there.
    bool                 unbounded{ false };      ///< No box can be trusted for this leaf.
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
        return LeafBox{ Aabbd::empty(), std::nullopt, true }; // nothing to cull by
    }
    return LeafBox{ Aabbd::empty(), world, false };
}

/// @brief Writes @p box into @p row and says whether that changed anything the fold depends on.
///
/// @param row The leaf's row.
/// @param box The bound read from the node.
/// @return true when the row's own box or its cullability moved, so the rows above it have to be folded again.
bool recordLeafBox(Row& row, const LeafBox& box)
{
    // Compared against what the NODE said: its own-frame box, or - for a leaf that can only answer in world space -
    // that world answer (the row's own-frame box is then the FOLD's derivation of it, not a statement).
    const bool from_world = box.world.has_value();
    const bool changed    = (from_world ? row.world_box_answer != box.world : row.local_box != box.local) || row.unbounded != box.unbounded;
    if (!changed) {
        return false; // nothing moved: write nothing, so the derived box the fold produced stays exactly as it was
    }
    row.world_box_answer = box.world;
    row.local_box        = from_world ? Aabbd::empty() : box.local; // from_world: the fold derives it from `world`
    row.unbounded        = box.unbounded;
    return true;
}

/// @brief Replays the fold for one row given what it inherits, and recurses into its children.
void foldDown(SceneBake& bake, std::uint32_t index, const Inherited& parent)
{
    Row& row = bake.rows[index];

    row.view_dependent = parent.view_dependent || row.placement != nullptr;

    Inherited inherited = parent;
    inherited.world          = parent.world * row.local;
    inherited.opacity        = parent.opacity * row.own_opacity;
    inherited.layers         = row.own_layers.value_or(parent.layers);
    inherited.draw_order     = row.own_draw_order.value_or(parent.draw_order);
    inherited.view_dependent = row.view_dependent;
    if (row.owns_state) {
        inherited.state.merge(row.own_state);
        if (row.own_material != nullptr) {
            inherited.material             = row.own_material;
            inherited.material_description = row.own_material_description;
        }
        if (row.own_program != nullptr) {
            inherited.program = row.own_program;
            inherited.program_description = row.own_program_description;
        }
    }

    row.world              = inherited.world;
    row.parent_world       = parent.world;
    row.opacity            = inherited.opacity;
    row.layers             = inherited.layers;
    row.draw_order         = inherited.draw_order;
    if (!row.draws.empty()) {
        row.state    = inherited.state;
        row.material             = inherited.material;
        row.material_description = inherited.material_description;
        row.program  = inherited.program;
        row.program_description = inherited.program_description;
    }

    for (std::uint32_t cursor = index + 1; cursor < row.end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
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
    const std::uint32_t end = bake.rows[index].end;
    Row&                row = bake.rows[index];
    // The own-frame box is a VALUE: a leaf carries its own (read at build time, or re-read by refreshBoxes when the
    // bytes under it announce), a container is the union of its children. A per-view child answers the union through
    // its placement instead of through `local`, because the orientation it may choose is not the axis-aligned one
    // its own box was measured in (see ViewPlacement::conservativeParentBox).
    Aabbd local = row.child_count == 0 ? row.local_box : Aabbd::empty();
    if (row.world_box_answer.has_value()) {
        // A leaf that could only answer in world space: undo this row's own placement so the row holds the same kind
        // of box every other row does. The AABB of the transformed corners is a SUPERSET for a rotated placement,
        // which errs on the side of drawing. This is the only place the conversion can happen - it needs `world`,
        // which is the fold's own answer.
        local         = Aabbd::empty();
        Mat4d inverse = row.world;
        if (inverse.invert()) {
            local = row.world_box_answer->transformed(inverse);
        }
        else {
            row.unbounded = true; // a degenerate placement cannot be undone, so no box can be trusted
        }
    }
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
        const Row&          child     = bake.rows[cursor];
        const Aabbd         placed    = child.placement != nullptr ? child.placement->conservativeParentBox(child.local_box)
                                                                  : child.local_box.transformed(child.local);
        if (placed.isValid()) {
            local.expandBy(placed);
        }
        cursor = child_end;
    }
    row.local_box = local;
    // The world box covers EVERY view, so a cull above this row never depends on the camera: a per-view row folds
    // the parent-frame bound through the parent's placement; an ordinary row folds its own box through its own.
    row.world_box = row.placement != nullptr ? row.placement->conservativeParentBox(local).transformed(row.parent_world)
                                             : local.transformed(row.world);

    // A subtree whose bound cannot be stated without a view must not be culled anywhere on the way down: the flag
    // climbs with the boxes it invalidates.
    bool          unbounded       = (row.placement != nullptr && !row.placement->isBounded()) || row.unbounded;
    std::uint32_t union_of_layers = row.layers;
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
        union_of_layers |= bake.rows[cursor].layers_union;
        unbounded |= bake.rows[cursor].unbounded;
        cursor = child_end;
    }
    row.unbounded    = unbounded;
    row.layers_union = union_of_layers;
}

/// @brief Recomputes one subtree's boxes and layer unions bottom-up.
void foldUp(SceneBake& bake, std::uint32_t index)
{
    const std::uint32_t end = bake.rows[index].end;
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
        foldUp(bake, cursor);
        cursor = child_end;
    }
    foldRow(bake, index);
}

/// @brief Fills one row's OWN facts from @p node, leaving its resolved fields and structure untouched.
///
/// @details
/// - `bakeSubtree` uses it for a fresh row; the incremental edit path uses it to refresh a row in place.
/// - The resolved fields (`world`, `parent_world`, `opacity`, `layers`, `world_box`, `layers_union`, …) are the
///   fold's output and are recomputed by `resolveSubtree`; this function clears only the fields it owns.
void fillOwnFacts(const Node& node, Row& row)
{
    row.identity       = node.id();
    row.local          = node.localTransformMatrix();
    row.own_opacity    = node.opacity();
    row.own_layers     = node.layers();
    row.own_draw_order = node.drawOrder();
    row.placement      = node.viewPlacement();

    row.owns_state = false;
    row.own_state  = RenderState{};
    row.own_material.reset();
    row.own_material_description.reset();
    row.own_program.reset();
    row.own_program_description.reset();
    row.geometry.reset();
    row.description.reset();
    row.draws.clear();
    row.world_box_answer.reset();
    row.unbounded = false;

    if (const auto* state_node = obj_cast<StateNode>(&node)) {
        row.owns_state = true;
        row.own_state  = state_node->renderState();
        if (raw_ptr<MaterialBase> material = state_node->material()) {
            row.own_material = material;
            // The values are taken WITH the identity, here, on the content side: the fold below only copies the value
            // pointer, which is what lets a mirror - threaded or not - resolve a material without reading the live
            // object.
            row.own_material_description = material->description();
        }
        if (raw_ptr<ShaderProgram> program = state_node->program()) {
            row.own_program = program;
            // Its text travels with it, for the same reason the material's values do (see the fold's program copy).
            row.own_program_description = program->description();
        }
    }
    if (const auto* geometry = obj_cast<Geometry>(&node)) {
        row.geometry    = GeometryPtr(const_cast<Geometry*>(geometry));
        row.description = geometry->description();
        const raw_ptr<MaterialBase>  geometry_material = geometry->material();
        const raw_ptr<ShaderProgram> geometry_program  = geometry->program();
        const std::size_t            primitive_count   = geometry->primitiveCount();
        row.draws.reserve(primitive_count);
        for (std::size_t ordinal = 0; ordinal < primitive_count; ++ordinal) {
            const Primitive* primitive = geometry->primitive(ordinal);
            if (primitive == nullptr) {
                continue;
            }
            DrawFacts draw;
            draw.primitive_index = static_cast<std::uint32_t>(ordinal);
            draw.material        = primitive->material != nullptr
                                       ? MaterialBasePtr(primitive->material)
                                       : (geometry_material != nullptr ? MaterialBasePtr(geometry_material) : MaterialBasePtr());
            draw.material_description =
                draw.material != nullptr ? draw.material->description() : MaterialDescriptionPtr();
            draw.program         = primitive->program != nullptr
                                       ? ShaderProgramPtr(primitive->program)
                                       : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : ShaderProgramPtr());
            draw.program_description = draw.program != nullptr ? draw.program->description() : ShaderProgramDescriptionPtr();
            draw.drawn = !(primitive->hasInstances() && primitive->visibleInstanceCount() == 0u);
            if (primitive->hasInstances()) {
                draw.instance_transforms = primitive->instance_transforms;
                draw.first_instance =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->firstInstance(), std::numeric_limits<std::uint32_t>::max()));
                draw.instance_count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->visibleInstanceCount(), std::numeric_limits<std::uint32_t>::max()));
            }
            row.draws.push_back(std::move(draw));
        }
        (void)recordLeafBox(row, readLeafBox(node));
    }
    else if (obj_cast<Group>(&node) == nullptr) {
        // A leaf the engine does not own: the same rule (see readLeafBox).
        (void)recordLeafBox(row, readLeafBox(node));
    }
}

std::uint32_t bakeSubtree(const Node& node, SceneBake& bake, std::uint32_t parent = SceneBake::kNoParent)
{
    if (!node.isVisible()) {
        return static_cast<std::uint32_t>(bake.rows.size());
    }

    const std::uint32_t index = static_cast<std::uint32_t>(bake.rows.size());
    bake.rows.emplace_back();
    bake.nodes.push_back(&node);

    {
        Row& row    = bake.rows[index];
        row.parent  = parent;
        fillOwnFacts(node, row);
    }

    std::uint32_t children = 0U;
    if (const auto* group = obj_cast<Group>(&node)) {
        for (const auto& child : group->children()) {
            (void)bakeSubtree(*child, bake, index);
            ++children;
        }
    }

    Row& row        = bake.rows[index];
    row.end         = static_cast<std::uint32_t>(bake.rows.size());
    row.child_count = children;
    return row.end;
}

} // namespace

RowRefresh refreshRowFacts(const Node& node, SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size() || bake.nodes.size() != bake.rows.size() || bake.nodes[index] != &node) {
        return RowRefresh::Incompatible;
    }

    const Row before = bake.rows[index];
    Row&      row    = bake.rows[index];
    fillOwnFacts(node, row);

    const auto same_matrix = [](const Mat4d& lhs, const Mat4d& rhs) {
        for (int row_index = 0; row_index < 4; ++row_index) {
            for (int column = 0; column < 4; ++column) {
                if (lhs.element(row_index, column) != rhs.element(row_index, column)) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto same_draw = [](const DrawFacts& lhs, const DrawFacts& rhs) {
        return lhs.primitive_index == rhs.primitive_index && lhs.material.get() == rhs.material.get() &&
               lhs.material_description.get() == rhs.material_description.get() &&
               lhs.program.get() == rhs.program.get() &&
               lhs.program_description.get() == rhs.program_description.get() &&
               lhs.instance_transforms.values.get() == rhs.instance_transforms.values.get() &&
               lhs.instance_transforms.first == rhs.instance_transforms.first &&
               lhs.instance_transforms.count == rhs.instance_transforms.count &&
               lhs.first_instance == rhs.first_instance && lhs.instance_count == rhs.instance_count &&
               lhs.drawn == rhs.drawn;
    };
    const auto same_draws = [&same_draw](const std::vector<DrawFacts>& lhs, const std::vector<DrawFacts>& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t draw_index = 0; draw_index < lhs.size(); ++draw_index) {
            if (!same_draw(lhs[draw_index], rhs[draw_index])) {
                return false;
            }
        }
        return true;
    };
    const auto same_optional_box = [](const std::optional<Aabbd>& lhs, const std::optional<Aabbd>& rhs) {
        if (lhs.has_value() != rhs.has_value()) {
            return false;
        }
        return !lhs.has_value() || *lhs == *rhs;
    };

    const bool non_hot = before.owns_state != row.owns_state || !(before.own_state == row.own_state) ||
                         before.own_material.get() != row.own_material.get() ||
                         before.own_material_description.get() != row.own_material_description.get() ||
                         before.own_program.get() != row.own_program.get() ||
                         before.own_program_description.get() != row.own_program_description.get() ||
                         before.geometry.get() != row.geometry.get() ||
                         before.description.get() != row.description.get() || !same_draws(before.draws, row.draws) ||
                         !same_optional_box(before.world_box_answer, row.world_box_answer) ||
                         !(before.local_box == row.local_box) || before.unbounded != row.unbounded;
    if (non_hot) {
        return RowRefresh::Incompatible;
    }

    const bool hot = !same_matrix(before.local, row.local) || before.own_opacity != row.own_opacity ||
                     before.own_layers != row.own_layers || before.own_draw_order != row.own_draw_order ||
                     before.placement.get() != row.placement.get();
    return hot ? RowRefresh::HotChange : RowRefresh::NoChange;
}

void buildOwnFacts(const Node& root, SceneBake& bake)
{
    bake.rows.clear();
    bake.nodes.clear();
    bake.seed = SceneBake::Seed{};
    (void)bakeSubtree(root, bake);
}

void refoldChain(SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size()) {
        return;
    }
    foldRow(bake, index);
    for (std::uint32_t above = bake.rows[index].parent; above != SceneBake::kNoParent; above = bake.rows[above].parent) {
        foldRow(bake, above);
    }
}

void buildRows(const Node& root, SceneBake& bake, float opacity)
{
    bake.rows.clear();
    bake.nodes.clear();
    bake.seed.parent_opacity = opacity;
    (void)bakeSubtree(root, bake);
    (void)resolveSubtree(bake, 0U);
}

std::uint32_t resolveSubtree(SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size()) {
        return 0U;
    }
    const std::uint32_t first_row = index;
    const std::uint32_t last_row  = bake.rows[index].end;

    Inherited parent;
    if (bake.rows[index].parent == SceneBake::kNoParent) {
        parent.opacity = bake.seed.parent_opacity;
    }
    else {
        const Row& above = bake.rows[bake.rows[index].parent];
        parent.world     = above.world;
        parent.opacity   = above.opacity;
        parent.layers    = above.layers;
        parent.draw_order = above.draw_order;
        parent.state     = above.state;
        parent.material  = above.material;
        parent.material_description = above.material_description;
        parent.program   = above.program;
        parent.program_description = above.program_description;
        parent.view_dependent = above.view_dependent;
    }
    foldDown(bake, index, parent);
    foldUp(bake, index);
    // THE CHAIN ABOVE. A box is a union, so the row just re-folded is part of every ancestor's box: leaving them
    // alone keeps a box that says the subtree is still where it was, and a cull then prunes content that moved INTO
    // view - the object silently disappears (a subset of the rows is stale, which no assertion in the edit path can
    // see). Each ancestor needs its OWN row recomputed and nothing more: its children are already folded (see
    // refoldChain, which is the same rule for a change that moves no values).
    for (std::uint32_t above = bake.rows[index].parent; above != SceneBake::kNoParent; above = bake.rows[above].parent) {
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
        if (node == nullptr || bake.rows[index].child_count != 0) {
            continue;
        }
        if (recordLeafBox(bake.rows[index], readLeafBox(*node))) {
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
        for (std::uint32_t above = bake.rows[changed[position]].parent; above != SceneBake::kNoParent; above = bake.rows[above].parent) {
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

std::uint32_t refreshValues(SceneBake& bake)
{
    // The value halves the fold copies are re-taken here: a material's values and a program's text are edited through
    // live objects, so no node mutator announces them and the graph epoch cannot see the change - while the values a
    // command carries do change. One function for both because they share the rule (no epoch, new value at the next
    // publication) and the same version a publication compares.
    std::uint32_t moved = 0U;
    for (Row& row : bake.rows) {
        bool row_moved = false;
        if (row.material != nullptr) {
            const MaterialDescriptionPtr values = row.material->description();
            if (values != row.material_description) {
                row.material_description = values;
                row_moved                = true;
            }
        }
        if (row.own_material != nullptr) {
            const MaterialDescriptionPtr values = row.own_material->description();
            if (values != row.own_material_description) {
                row.own_material_description = values;
                row_moved                    = true;
            }
        }
        if (row.program != nullptr) {
            const ShaderProgramDescriptionPtr text = row.program->description();
            if (text != row.program_description) {
                row.program_description = text;
                row_moved               = true;
            }
        }
        if (row.own_program != nullptr) {
            const ShaderProgramDescriptionPtr text = row.own_program->description();
            if (text != row.own_program_description) {
                row.own_program_description = text;
                row_moved                   = true;
            }
        }
        for (DrawFacts& draw : row.draws) {
            if (draw.material != nullptr) {
                const MaterialDescriptionPtr values = draw.material->description();
                if (values != draw.material_description) {
                    draw.material_description = values;
                    row_moved                 = true;
                }
            }
            if (draw.program != nullptr) {
                const ShaderProgramDescriptionPtr text = draw.program->description();
                if (text != draw.program_description) {
                    draw.program_description = text;
                    row_moved                = true;
                }
            }
        }
        if (row_moved) {
            ++moved;
        }
    }
    return moved;
}

void selectFromBake(const SceneBake&            bake,
                    const Frustum&              frustum,
                    const CollectView&          view,
                    std::uint32_t               layer_mask,
                    bool&                       viewport_used,
                    std::vector<RenderCommand>& out,
                    std::vector<SortKey>*       keys)
{
    selectRows(static_cast<std::uint32_t>(bake.rows.size()),
               [&bake](std::uint32_t index) -> const Row& { return bake.rows[index]; },
               frustum,
               view,
               layer_mask,
               viewport_used,
               out,
               keys);
}

SortKey makeSortKey(const RenderCommand& command, std::uint32_t layers, const Aabbd& box, const Mat4d& view, std::uint64_t tie)
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

void sortCollected(std::vector<RenderCommand>& commands, std::span<const SortKey> keys)
{
    if (keys.size() != commands.size()) {
        return; // a list of keys that does not describe these commands cannot order them: answer what was handed in
    }
    // NO EQUAL KEYS (the tie field is unique per command), so a plain sort is deterministic and no stability is needed - the
    // order is a property of the commands, not of the walk that collected them (R17 / T9).
    std::vector<std::uint32_t> order(commands.size());
    for (std::uint32_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&keys](std::uint32_t lhs, std::uint32_t rhs) { return keys[lhs] < keys[rhs]; });

    std::vector<RenderCommand> ordered;
    ordered.reserve(commands.size());
    for (const std::uint32_t index : order) {
        ordered.push_back(std::move(commands[index]));
    }
    commands = std::move(ordered);
}

} // namespace detail

VN_GRAPHICS_NS_END
