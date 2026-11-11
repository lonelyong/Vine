#include "DrawListBake.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

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

/// @brief Per-run cache of the world-space bounds the graph walk culls with (per-view runs only).
class BoundsCache {
  public:
    const Aabbd& worldBound(const Node* node, const Mat4d& world)
    {
        const auto cached = bounds_.find(node);
        if (cached != bounds_.end()) {
            return cached->second;
        }
        Aabbd box   = Aabbd::empty();
        Aabbd local = Aabbd::empty();
        if (node->subtreeBounds(local)) {
            box = local.transformed(world);
        }
        else if (const auto* group = obj_cast<Group>(node)) {
            for (const auto& child : group->children()) {
                box.expandBy(worldBound(child.get(), world * child->localTransformMatrix()));
            }
        }
        else {
            box = node->boundingBox();
        }
        return bounds_.emplace(node, box).first->second;
    }

  private:
    std::unordered_map<const Node*, Aabbd> bounds_;
};

/// @brief Collects one subtree's commands by WALKING it: the per-view half of a run (see SceneBake::Fallback).
///
/// The only reason this still exists is a node whose placement is a function of the view (Node::viewDependence): the
/// rows cannot place it, so the run it starts is handed back to the walk that can. Everything else in a select reads
/// rows only.
void collectNodeCommands(const Node*                 node,
                         const Mat4d&                world,
                         const Frustum&              frustum,
                         const CollectView&          view,
                         float                       opacity,
                         const Inherited&            inherited,
                         std::uint32_t               layer_mask,
                         BoundsCache&                bounds,
                         bool&                       viewport_used,
                         std::vector<RenderCommand>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if ((node->subtreeLayerUnion(inherited.layers) & layer_mask) == 0) {
        return;
    }
    Mat4d                node_world = world;
    Mat4d                view_world;
    const ViewDependence dependence = node->localToWorldForView(world, view, view_world);
    if (dependence != ViewDependence::None) {
        node_world = view_world;
    }
    if (dependence == ViewDependence::ViewAndViewport) {
        viewport_used = true;
    }
    const std::uint32_t layers = node->layers().value_or(inherited.layers);
    if (frustum.isOutside(bounds.worldBound(node, node_world))) {
        return;
    }
    const float node_opacity = opacity * node->opacity();

    Inherited subtree = inherited;
    subtree.layers     = layers;
    subtree.draw_order = node->drawOrder().value_or(inherited.draw_order);
    if (const auto* state_node = obj_cast<StateNode>(node)) {
        subtree.state.merge(state_node->renderState());
        if (raw_ptr<MaterialBase> material = state_node->material()) {
            subtree.material = material;
        }
        if (raw_ptr<ShaderProgram> program = state_node->program()) {
            subtree.program = program;
        }
    }

    if (const auto* geometry = obj_cast<Geometry>(node)) {
        if ((layers & layer_mask) == 0) {
            return;
        }
        const float effective = std::clamp(node_opacity, 0.0f, 1.0f);
        for (std::size_t index = 0; index < geometry->primitiveCount(); ++index) {
            const Primitive* primitive = geometry->primitive(index);
            if (primitive == nullptr) {
                continue;
            }
            if (primitive->hasInstances() && primitive->visibleInstanceCount() == 0u) {
                continue;
            }
            const raw_ptr<MaterialBase> geometry_material = geometry->material();
            const MaterialBasePtr       material = primitive->material != nullptr
                                                       ? MaterialBasePtr(primitive->material)
                                                       : (geometry_material != nullptr ? MaterialBasePtr(geometry_material) : subtree.material);
            const raw_ptr<ShaderProgram> geometry_program = geometry->program();
            const ShaderProgramPtr       program = primitive->program != nullptr
                                                       ? ShaderProgramPtr(primitive->program)
                                                       : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : subtree.program);
            auto& cmd          = out.emplace_back(intrusive_ptr<Geometry>(const_cast<Geometry*>(geometry)), material, node_world, static_cast<std::uint32_t>(index));
            cmd.opacity        = effective;
            cmd.draw_order     = subtree.draw_order;
            cmd.authored_state = subtree.state;
            cmd.program        = program;
            if (primitive->hasInstances()) {
                cmd.instance_transforms = primitive->instance_transforms;
                cmd.first_instance =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->firstInstance(), std::numeric_limits<std::uint32_t>::max()));
                cmd.instance_count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->visibleInstanceCount(), std::numeric_limits<std::uint32_t>::max()));
            }
        }
        return;
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            collectNodeCommands(child.get(), node_world * child->localTransformMatrix(), frustum, view, node_opacity, subtree, layer_mask, bounds, viewport_used, out);
        }
    }
}

/// @brief Replays the fold for one row given what it inherits, and recurses into its children.
void foldDown(SceneBake& bake, std::uint32_t index, const Inherited& parent)
{
    Row& row = bake.rows[index];

    const bool starts_run = !parent.view_dependent && row.dependence != ViewDependence::None;
    row.view_dependent    = parent.view_dependent || row.dependence != ViewDependence::None;

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
    row.opacity            = inherited.opacity;
    row.layers             = inherited.layers;
    row.draw_order         = inherited.draw_order;
    if (!row.draws.empty()) {
        row.state    = inherited.state;
        row.material = inherited.material;
        row.program  = inherited.program;
    }

    if (starts_run) {
        // The select hands this whole subtree back to the graph walk, so what it needs is the context ABOVE the run
        // and the run's own accumulated placement (the walk's convention, see collectNodeCommands).
        bake.fallbacks.push_back(SceneBake::Fallback{ index, row.node, inherited.world, parent.opacity, parent.layers, parent.draw_order, parent.state, parent.material, parent.program });
    }

    for (std::uint32_t cursor = index + 1; cursor < row.end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
        foldDown(bake, cursor, inherited);
        cursor = child_end;
    }
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

    Row& row = bake.rows[index];
    if (row.box_from_node) {
        row.world_box = row.seed_world_box;
    }
    else {
        // The own-frame box comes from the producer that knows it: a graph producer asks the node's cached subtree
        // box (a box in a node's own frame does not depend on the camera, so a camera move recomputes nothing and
        // only an announcement inside the subtree invalidates it); a value producer already carries it. The fold
        // unions the children only when there is no node to ask.
        Aabbd local  = Aabbd::empty();
        bool  answered = row.node != nullptr && row.node->subtreeBounds(local);
        if (!answered && row.geometry != nullptr) {
            local    = row.geometry->localBounds();
            answered = true;
        }
        if (!answered) {
            for (std::uint32_t cursor = index + 1; cursor < end;) {
                const std::uint32_t child_end = bake.rows[cursor].end;
                if (bake.rows[cursor].local_box.isValid()) {
                    local.expandBy(bake.rows[cursor].local_box.transformed(bake.rows[cursor].local));
                }
                cursor = child_end;
            }
        }
        row.local_box = local;
        row.world_box = local.transformed(row.world);
    }
    if (row.geometry != nullptr) {
        row.box_revision = row.geometry->revision();
    }

    std::uint32_t union_of_layers = row.layers;
    for (std::uint32_t cursor = index + 1; cursor < end;) {
        const std::uint32_t child_end = bake.rows[cursor].end;
        union_of_layers |= bake.rows[cursor].layers_union;
        cursor = child_end;
    }
    row.layers_union = union_of_layers;
}

std::uint32_t bakeSubtree(const Node& node, SceneBake& bake, std::uint32_t parent = SceneBake::kNoParent)
{
    if (!node.isVisible()) {
        return static_cast<std::uint32_t>(bake.rows.size());
    }

    const std::uint32_t index = static_cast<std::uint32_t>(bake.rows.size());
    bake.rows.emplace_back();

    {
        Row& row           = bake.rows[index];
        row.identity       = &node;
        row.node           = &node;
        row.parent         = parent;
        row.local          = node.localTransformMatrix();
        row.own_visible    = node.isVisible();
        row.own_opacity    = node.opacity();
        row.own_layers     = node.layers();
        row.own_draw_order = node.drawOrder();
        row.dependence     = node.viewDependence();
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
            row.geometry = GeometryPtr(const_cast<Geometry*>(geometry));
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
            // Its own-frame box: the engine's own leaf answers it (and caches it per data revision); a SUBCLASS may
            // place itself differently and answers only in world space (Node::subtreeBounds says so), in which case
            // the producer records the node's answer as a seed and the row is not re-placeable from values.
            Aabbd local = Aabbd::empty();
            if (!node.subtreeBounds(local)) {
                row.box_from_node  = true;
                row.seed_world_box = node.boundingBox();
            }
            else {
                row.local_box = local;
            }
        }
        else if (obj_cast<Group>(&node) == nullptr) {
            // A leaf the engine does not own: it may answer only in world space, so the value is a SEED and the fold
            // cannot re-place it (see Row::box_from_node).
            Aabbd local = Aabbd::empty();
            if (!node.subtreeBounds(local)) {
                row.box_from_node   = true;
                row.seed_world_box  = node.boundingBox();
            }
            else {
                row.local_box = local;
            }
        }    }

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

void buildRows(const Node& root, SceneBake& bake, float opacity)
{
    bake.rows.clear();
    bake.fallbacks.clear();
    bake.seed.parent_opacity = opacity;
    (void)bakeSubtree(root, bake);
    (void)resolveSubtree(bake, 0U);
}

std::uint32_t resolveSubtree(SceneBake& bake, std::uint32_t index)
{
    if (index >= bake.rows.size()) {
        return 0U;
    }
    if (bake.rows[index].dependence != ViewDependence::None || bake.rows[index].box_from_node) {
        return 0U; // the fold cannot answer for a per-view placement, and cannot re-read a node's own box
    }
    // The fallback list is rebuilt for the folded subtree only: those before it are untouched, those inside it are
    // stale now. A run that starts inside the subtree is re-registered by the fold.
    const std::uint32_t first_row = index;
    const std::uint32_t last_row  = bake.rows[index].end;
    auto                erased   = std::remove_if(bake.fallbacks.begin(), bake.fallbacks.end(), [first_row, last_row](const SceneBake::Fallback& run) {
        return run.index >= first_row && run.index < last_row;
    });
    bake.fallbacks.erase(erased, bake.fallbacks.end());

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

    // The fallbacks stay sorted by row index, which is what the select walks them by.
    std::sort(bake.fallbacks.begin(), bake.fallbacks.end(), [](const SceneBake::Fallback& lhs, const SceneBake::Fallback& rhs) {
        return lhs.index < rhs.index;
    });
    return last_row - first_row;
}

std::uint32_t refreshBoxes(SceneBake& bake)
{
    if (bake.rows.empty()) {
        return 0U;
    }
    // The boxes are the one part of the rows that a BYTE edit can stale without a delta, and the freshness key is the
    // geometry's own (its channels' revisions and their slices - see Geometry::localBounds). Asking the same fold
    // again is what re-reads it: every geometry box is a cache hit unless something it reads announced.
    foldUp(bake, 0U);
    std::uint32_t refreshed = 0U;
    for (const Row& row : bake.rows) {
        if (row.geometry != nullptr) {
            ++refreshed;
        }
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
    std::size_t fallback = 0U;
    for (std::uint32_t index = 0; index < bake.rows.size();) {
        const Row& row = bake.rows[index];
        if (row.view_dependent) {
            // The rare half: one run per top-most per-view node, in row order (see bakeSubtree / foldDown).
            if (fallback < bake.fallbacks.size() && bake.fallbacks[fallback].index == index) {
                const SceneBake::Fallback& run = bake.fallbacks[fallback];
                ++fallback;
                BoundsCache     bounds;
                const Inherited inherited{ {}, run.parent_opacity, run.layers, run.draw_order, run.state, run.material, run.program, false };
                collectNodeCommands(run.node, run.world, frustum, view, run.parent_opacity, inherited, layer_mask, bounds, viewport_used, out);
            }
            index = row.end;
            continue;
        }
        if ((row.layers_union & layer_mask) == 0U) {
            index = row.end;
            continue;
        }
        if (frustum.isOutside(row.world_box)) {
            index = row.end;
            continue;
        }
        if (!row.draws.empty()) {
            const float effective = std::clamp(row.opacity, 0.0f, 1.0f);
            for (const DrawFacts& draw : row.draws) {
                if (!draw.drawn) {
                    continue;
                }
                const MaterialBasePtr material = draw.material != nullptr ? draw.material : row.material;
                const ShaderProgramPtr program = draw.program != nullptr ? draw.program : row.program;
                auto&                  cmd     = out.emplace_back(row.geometry, material, row.world, draw.primitive_index);
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
