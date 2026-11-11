#include "DrawListBake.hpp"

#include <algorithm>
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
    ShaderProgramPtr program{};
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
            inherited.material = row.own_material;
        }
        if (row.own_program != nullptr) {
            inherited.program = row.own_program;
        }
    }

    row.world              = inherited.world;
    row.parent_world       = parent.world;
    row.opacity            = inherited.opacity;
    row.layers             = inherited.layers;
    row.draw_order         = inherited.draw_order;
    if (!row.draws.empty()) {
        row.state    = inherited.state;
        row.material = inherited.material;
        row.program  = inherited.program;
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

std::uint32_t bakeSubtree(const Node& node, SceneBake& bake, std::uint32_t parent = SceneBake::kNoParent)
{
    if (!node.isVisible()) {
        return static_cast<std::uint32_t>(bake.rows.size());
    }

    const std::uint32_t index = static_cast<std::uint32_t>(bake.rows.size());
    bake.rows.emplace_back();
    bake.nodes.push_back(&node);

    {
        Row& row           = bake.rows[index];
        row.identity       = &node;
        row.parent         = parent;
        row.local          = node.localTransformMatrix();
        row.own_visible    = node.isVisible();
        row.own_opacity    = node.opacity();
        row.own_layers     = node.layers();
        row.own_draw_order = node.drawOrder();
        // The per-view half of the placement, IF the node has one: a value the select resolves from the view it
        // draws (see ViewPlacement). Taking it here is what keeps the select free of node reads.
        row.placement = node.viewPlacement();
        if (const auto* state_node = obj_cast<StateNode>(&node)) {
            row.owns_state = true;
            row.own_state  = state_node->renderState();
            if (raw_ptr<MaterialBase> material = state_node->material()) {
                row.own_material = material;
            }
            if (raw_ptr<ShaderProgram> program = state_node->program()) {
                row.own_program = program;
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
                draw.program         = primitive->program != nullptr
                                           ? ShaderProgramPtr(primitive->program)
                                           : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : ShaderProgramPtr());
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
        parent.program   = above.program;
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

void selectFromBake(const SceneBake&            bake,
                    const Frustum&              frustum,
                    const CollectView&          view,
                    std::uint32_t               layer_mask,
                    bool&                       viewport_used,
                    std::vector<RenderCommand>& out)
{
    // The rows are in walk order, so the rows currently being resolved form the ancestor chain, deepest last. A row
    // whose placement was resolved for THIS view hands its world down; every other row is already placed by the fold.
    struct OpenRow {
        std::uint32_t end{ 0 };          ///< One past the row's subtree: it stops being an ancestor there.
        bool          resolved{ false }; ///< Its world was resolved from the view (children must compose with it).
        Mat4d         world{};           ///< The world the row resolved to.
    };
    std::vector<OpenRow> open;

    for (std::uint32_t index = 0; index < bake.rows.size();) {
        while (!open.empty() && open.back().end <= index) {
            open.pop_back();
        }
        const Row&  row             = bake.rows[index];
        const bool  parent_resolved = !open.empty() && open.back().resolved;
        const bool  in_run          = row.view_dependent || parent_resolved;
        const Mat4d parent_world    = parent_resolved ? open.back().world : row.parent_world;

        Mat4d world    = row.world;
        bool  resolved = false;
        if (row.placement != nullptr) {
            const ViewDependence dependence = localToWorldForView(*row.placement, parent_world, view, world);
            if (dependence == ViewDependence::ViewAndViewport) {
                viewport_used = true;
            }
            if (dependence == ViewDependence::None) {
                world = parent_world * row.local; // this view keeps the node's own placement
            }
            resolved = true;
        }
        else if (in_run) {
            world    = parent_world * row.local; // an ancestor was re-placed, so this row's folded world is stale
            resolved = true;
        }

        if ((row.layers_union & layer_mask) == 0U) {
            index = row.end;
            continue;
        }
        // A row with no view-independent bound must not be culled: no box can promise it is out of the frustum.
        if (!row.unbounded && frustum.isOutside(resolved ? row.local_box.transformed(world) : row.world_box)) {
            index = row.end;
            continue;
        }
        if (resolved) {
            open.push_back(OpenRow{ row.end, true, world });
        }
        if (!row.draws.empty()) {
            const float effective = std::clamp(row.opacity, 0.0f, 1.0f);
            for (const DrawFacts& draw : row.draws) {
                if (!draw.drawn) {
                    continue;
                }
                const MaterialBasePtr material = draw.material != nullptr ? draw.material : row.material;
                const ShaderProgramPtr program = draw.program != nullptr ? draw.program : row.program;
                auto&                  cmd     = out.emplace_back(row.geometry, material, world, draw.primitive_index);
                cmd.row                        = row.identity;
                cmd.description                = row.description;
                cmd.opacity                    = effective;
                cmd.draw_order                 = row.draw_order;
                cmd.authored_state             = row.state;
                cmd.program                    = program;
                cmd.instance_transforms        = draw.instance_transforms;
                cmd.first_instance             = draw.first_instance;
                cmd.instance_count             = draw.instance_count;
            }
        }
        ++index;
    }
}

void sortCollected(std::vector<RenderCommand>& commands, const Vec3d& eye)
{
    std::vector<std::pair<double, RenderCommand*>> keyed;
    keyed.reserve(commands.size());
    for (RenderCommand& cmd : commands) {
        const auto origin = cmd.model_matrix * vn::math::Point3d(0.0, 0.0, 0.0);
        keyed.emplace_back((origin.asVector() - eye).length2(), &cmd);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const std::pair<double, RenderCommand*>& lhs, const std::pair<double, RenderCommand*>& rhs) {
        // The draw order DECLARED on the content is the outer key (see Node::setDrawOrder): a higher order is drawn
        // later, which is how content that must land on top of the rest is expressed without its own pass. Equal
        // orders fall through to the ordering the collection has always produced.
        if (lhs.second->draw_order != rhs.second->draw_order) {
            return lhs.second->draw_order < rhs.second->draw_order;
        }
        if (lhs.second->isTransparent() != rhs.second->isTransparent()) {
            return !lhs.second->isTransparent();
        }
        return lhs.second->isTransparent() ? lhs.first > rhs.first : lhs.first < rhs.first;
    });
    std::vector<RenderCommand> ordered;
    ordered.reserve(keyed.size());
    for (auto& [distance, cmd] : keyed) {
        (void)distance;
        ordered.push_back(std::move(*cmd));
    }
    commands = std::move(ordered);
}

} // namespace detail

VN_GRAPHICS_NS_END
