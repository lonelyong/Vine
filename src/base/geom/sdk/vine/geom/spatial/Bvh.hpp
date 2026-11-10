#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/detail/BvhBuildJob.hpp>
#include <vine/geom/spatial/detail/BvhRefit.hpp>
#include <vine/geom/spatial/detail/BvhTopologyCheck.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

class BvhBuilder;

/**
 * @brief A bounding-volume hierarchy over caller-owned primitives: geometry, invariants and traversal only.
 *
 * @details
 * - **This type holds no build logic.** It stores the primitives' volumes, the reordered primitive indices and the
 *   flattened nodes; how a tree is *constructed* lives in `BvhBuilder`, so a new construction algorithm never touches
 *   this class.
 * - **It owns no geometry.** A primitive is referred to by a `PrimId` the caller assigned when it called `add()`; the
 *   vertices, triangles or points stay in the caller's arrays and are never copied here.
 * - **The leaf volumes are conservative, not exact.** A leaf's box encloses its primitives; a query reports candidates,
 *   and the caller still runs its own exact test.
 * - **The tree keeps the node COMBINATORICS and the node VOLUMES in two parallel arrays** (`shape_.nodes`,
 *   `node_bounds_`): the combinatorics are `BvhNodeTopology`, which is also what a file holds, so a stored shape is the
 *   tree's own node array rather than a transcription of it (`topologyView()` is free, and `restoreTopology()` adopts an
 *   owned shape by moving it). Boxes stay out of the stored half for the reason they are derived from the primitives.
 * - **A tree has two states, and the difference is what it holds for its primitives**: a MUTABLE tree keeps one volume
 *   per primitive (which is what `update()` and `refit()` work from), a SEALED tree (see `seal()`) keeps the build's
 *   RESULT - the nodes, their boxes and the primitive order - and drops those volumes. Sealing is the answer to "who
 *   pays 48 B per primitive" (`Aabb<double>`): a tree that is built to be queried, not updated, stops paying it.
 *
 * @warning
 * - A build is required before queries answer anything: `add()` after a build clears the nodes, so a query on a tree
 *   that has pending additions visits nothing rather than answering from a stale tree.
 * - `update()` changes a primitive's volume; the tree is only valid again after `refit()`, which keeps the topology.
 * - A SEALED tree is read-only: `add()`, `addRange()`, `update()`, `refit()`, `reserve()` and `restoreTopology()` throw
 *   `std::logic_error`, because the volumes they need are gone. Its query visitors receive the leaf's box instead of a
 *   primitive's own (a superset, which is the conservative answer the tree already promises); `topology()`,
 *   `primOrder()`, `validate()` and `qualityReport()` keep answering.
 * - Queries are `const` and safe to run concurrently. Everything else needs exclusive access to the object.
 *
 * @tparam T      Floating-point scalar type.
 * @tparam Volume Bounding-volume template: `Aabb` today; see `detail::VolumeTraits` for what a new volume type owes.
 */
template <typename T, template <typename> class Volume = Aabb>
class Bvh
{
  public:
    using Scalar  = T;
    using VolumeType = Volume<T>;
    using PrimId  = std::uint32_t;
    using Node    = BvhNodeTopology;

    /// Hard limit on the tree height; a builder may choose a smaller one but never a larger one.
    static constexpr unsigned kMaxDepth = kBvhMaxDepth;

    /**
     * @brief What the tree currently costs: leaf area, leaf count and height.
     *
     * @details
     * - `leaf_half_area_sum` is the surface-area term of the SAH cost. Comparing it with the value from the last build
     *   is how a caller decides that repeated `refit()` calls have loosened the tree enough to rebuild.
     */
    struct Quality
    {
        /// Sum of the half surface areas of every leaf.
        double leaf_half_area_sum{ 0 };

        /// Number of leaves.
        std::size_t leaf_count{ 0 };

        /// Depth of the deepest leaf; zero for a single-leaf tree.
        unsigned depth{ 0 };
    };

    /**
     * @brief Registers one primitive's bounding volume.
     *
     * @details
     * - Registration is data, not construction: it fills the volume array and nothing else. A build is required
     *   afterwards for the tree to see it.
     * - An addition invalidates an existing tree, because a refit cannot add nodes.
     *
     * @param bounds Volume enclosing the primitive; must be finite and not inverted.
     * @return The primitive id to use with `update()` and query visitors.
     * @throws std::invalid_argument when the volume holds NaN, infinity or an inverted axis.
     * @throws std::length_error   when the id would not fit in `PrimId`.
     * @throws std::logic_error    when the tree is sealed (see `seal()`).
     */
    PrimId add(const VolumeType& bounds)
    {
        rejectWhenSealed("add()");
        if (!detail::VolumeTraits<VolumeType>::isFinite(bounds)) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: the primitive volume must be finite and not inverted");
        }
        if (prim_bounds_.size() >= static_cast<std::size_t>(std::numeric_limits<PrimId>::max())) {
            throw std::length_error("vn::geom::spatial::Bvh: more primitives than a PrimId can index");
        }
        prim_bounds_.push_back(bounds);
        dropTree();
        return static_cast<PrimId>(prim_bounds_.size() - 1);
    }

    /**
     * @brief Registers `count` primitives whose volumes this call computes on demand.
     *
     * @details
     * - Takes a callable instead of a span so a caller holding vertices and indices does not have to materialise a
     *   temporary array of volumes first; the volumes are computed once and stored.
     *
     * @tparam BoundsOf Callable `VolumeType(std::size_t)` returning the volume of primitive `index`.
     * @param count     Number of primitives.
     * @param bounds_of Computes the volume of one primitive.
     * @throws std::invalid_argument when any volume is not finite, or `bounds_of` throws.
     * @throws std::length_error   when the ids would not fit in `PrimId`.
     * @throws std::logic_error    when the tree is sealed (see `seal()`).
     */
    template <typename BoundsOf>
    void addRange(std::size_t count, BoundsOf&& bounds_of)
    {
        rejectWhenSealed("addRange()");
        prim_bounds_.reserve(prim_bounds_.size() + count);
        for (std::size_t index = 0; index < count; ++index) {
            add(static_cast<const VolumeType&>(bounds_of(index)));
        }
    }

    /**
     * @brief Replaces one primitive's bounding volume without touching the topology.
     *
     * @details
     * - For moving geometry: the tree's shape still fits, only its boxes are stale, and `refit()` repairs them in one
     *   linear pass. Adding or removing primitives is not the same thing and needs a build.
     *
     * @param id     Primitive id returned by `add()`.
     * @param bounds The new volume; must be finite and not inverted.
     * @throws std::invalid_argument when `id` is out of range or the volume is not usable.
     * @throws std::logic_error    when the tree is sealed (see `seal()`).
     */
    void update(PrimId id, const VolumeType& bounds)
    {
        rejectWhenSealed("update()");
        if (id >= prim_bounds_.size()) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: update() needs an id returned by add()");
        }
        if (!detail::VolumeTraits<VolumeType>::isFinite(bounds)) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: the primitive volume must be finite and not inverted");
        }
        prim_bounds_[id] = bounds;
    }

    /**
     * @brief Reserves storage for `prim_count` primitives, so later registrations do not allocate.
     *
     * @param prim_count Expected primitive count.
     * @throws std::logic_error when the tree is sealed (see `seal()`).
     */
    void reserve(std::size_t prim_count)
    {
        rejectWhenSealed("reserve()");
        prim_bounds_.reserve(prim_count);
    }

    /**
     * @brief Forgets every primitive and the tree, keeping the allocated storage.
     *
     * @details
     * - The next registration starts numbering at zero again; the capacity of all three arrays is kept, which is what
     *   makes a rebuild cycle allocation-free.
     * - This is also how a SEALED tree becomes mutable again: the sealing goes away with the tree it described.
     */
    void clear()
    {
        prim_bounds_.clear();
        shape_.nodes.clear();
        shape_.order.clear();
        shape_.depth = 0;
        node_bounds_.clear();
    }

    /**
     * @brief Returns how many primitives are registered.
     *
     * @details
     * - A sealed tree answers from its primitive ORDER: that array is the same length as the volumes it dropped, so the
     *   count survives the sealing (`Mesh::setBvh()` and `Geometry::adoptBvh()` check it against the triangle count).
     *
     * @return The primitive count, whether or not a tree has been built.
     */
    [[nodiscard]] std::size_t primCount() const noexcept { return prim_bounds_.empty() ? shape_.order.size() : prim_bounds_.size(); }

    /**
     * @brief Reports whether no primitive is registered.
     *
     * @return True when the tree holds nothing.
     */
    [[nodiscard]] bool empty() const noexcept { return primCount() == 0u; }

    /**
     * @brief Reports whether a build has been performed since the last registration.
     *
     * @return True when the nodes describe the current primitives.
     */
    [[nodiscard]] bool isBuilt() const { return !shape_.nodes.empty(); }

    /**
     * @brief Reports whether the tree is SEALED: built, and no longer holding a volume per primitive.
     *
     * @details
     * - Sealed means "this tree answers queries and nothing else": `seal()` dropped the per-primitive volumes the build
     *   needed, so the object's residency is the RESULT (nodes, their boxes, the primitive order) and no longer grows
     *   with the primitive count twice.
     * - The state is decided by what is held, not by a flag: a built tree whose volume array is gone IS sealed, and an
     *   empty or not-yet-built tree is not (it still holds, or may still take, its primitives).
     *
     * @return True when the tree is built and has no per-primitive volumes left.
     */
    [[nodiscard]] bool isSealed() const { return prim_bounds_.empty() && !shape_.order.empty(); }

    /**
     * @brief Drops the per-primitive volumes, keeping the built tree: the tree becomes read-only, and smaller.
     *
     * @details
     * - The volumes are BUILD SCRATCH for a tree that will never be updated: the queries walk `node_bounds_`, and the
     *   only things that read `prim_bounds_` afterwards are the mutations (`update` / `refit` / `restoreTopology`) and
     *   the visitors' second argument. For `Aabb<double>` this releases 48 bytes per primitive - the dominant term on a
     *   triangle soup, where the primitive count is the triangle count.
     * - The memory is RELEASED, not just emptied: the caller seals precisely to stop paying for it.
     * - A visitor asking for volumes gets the leaf's box afterwards. That box CONTAINS each of its primitives, so every
     *   answer stays a superset of the exact one (the same conservative promise a leaf box already makes) - what a
     *   caller loses is the per-primitive rejection, not the correctness of the primitives reported.
     *
     * @note A tree that is not built has nothing to keep, so sealing it does nothing: the primitives stay mutable, which
     *       is the state a caller that registered but has not built is in.
     * @note Sealing is idempotent.
     * @warning After this call the tree cannot be updated, refit or restored into (see the class warning): a caller that
     *          may still change its primitives must not seal.
     */
    void seal()
    {
        if (shape_.nodes.empty()) {
            return; // nothing built: there is no result to keep, and the primitives have to stay mutable
        }
        std::vector<VolumeType>().swap(prim_bounds_); // release the capacity too: emptying it would keep the bytes
    }

    /**
     * @brief Visits every primitive whose leaf volume overlaps `query`.
     *
     * @details
     * - Allocation-free and `const`: the traversal stack lives on the call's own stack.
     * - The visit receives the primitive's own volume; a SEALED tree answers its leaf's volume instead, which CONTAINS
     *   it (see `seal()`). Either way the primitive reported here may still miss `query` under an exact test.
     *
     * @tparam Visit Callable `bool(PrimId, const VolumeType&)`, returning false to stop the traversal.
     * @param query  Volume to test against.
     * @param visit  Called for each candidate.
     */
    template <typename Visit>
    void forEachOverlap(const VolumeType& query, Visit&& visit) const
    {
        if (shape_.nodes.empty()) {
            return;
        }
        std::array<PrimId, kMaxDepth + 2> stack{};
        unsigned                          top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const PrimId index = stack[--top];
            if (!detail::VolumeTraits<VolumeType>::overlaps(node_bounds_[index], query)) {
                continue;
            }
            const Node& node = shape_.nodes[index];
            if (node.count != 0) {
                if (!visitLeaves(node, node_bounds_[index], visit)) {
                    return;
                }
                continue;
            }
            stack[top++] = node.offset; // right child
            stack[top++] = index + 1;   // left child is always the next slot after the parent
        }
    }

    /**
     * @brief Visits every primitive whose leaf volume the ray enters within `max_t`.
     *
     * @details
     * - The ray is prepared once per query (reciprocals and sign bits), so a node test performs no division.
     * - Children are visited nearest first and the farther one is dropped when its entry distance already exceeds the
     *   current limit, which is what makes a closest-hit search stop early.
     * - The visit may lower `max_t` — that is how a closest-hit search prunes: intersect the primitive, and if it is
     *   closer than `max_t`, write the new distance back.
     *
     * @tparam Visit Callable `bool(PrimId, const VolumeType&, T& max_t)`, returning false to stop the traversal.
     * @param ray    The ray; an aimless ray visits nothing.
     * @param visit  Called for each candidate.
     * @param max_t  Initial distance limit; updated by the visit to tighten the search.
     */
    template <typename Visit>
    void forEachRay(const Ray<T>& ray, Visit&& visit, T max_t = std::numeric_limits<T>::infinity()) const
    {
        if (shape_.nodes.empty()) {
            return;
        }
        using Traits = detail::VolumeTraits<VolumeType>;

        const detail::PreparedRay<T> prepared = detail::PreparedRay<T>::from(ray);
        T                            limit    = max_t;

        std::array<PrimId, kMaxDepth + 2> stack{};
        unsigned                          top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const PrimId index = stack[--top];
            T            enter = T(0);
            if (!Traits::hitByRay(node_bounds_[index], prepared, enter) || enter > limit) {
                continue;
            }
            const Node& node = shape_.nodes[index];
            if (node.count != 0) {
                // A sealed tree has no per-primitive volumes: the leaf's box is what the visitor is handed, and it
                // contains every primitive of that leaf (see seal()).
                for (PrimId slot = node.offset; slot < node.offset + node.count; ++slot) {
                    const PrimId id = shape_.order[slot];
                    if (!visit(id, prim_bounds_.empty() ? node_bounds_[index] : prim_bounds_[id], limit)) {
                        return;
                    }
                }
                continue;
            }

            // Depth-first but nearest first: test both children, descend into the closer one.
            T           left_enter  = T(0);
            T           right_enter = T(0);
            const bool  hit_left    = Traits::hitByRay(node_bounds_[index + 1], prepared, left_enter) && left_enter <= limit;
            const bool  hit_right   = Traits::hitByRay(node_bounds_[node.offset], prepared, right_enter) && right_enter <= limit;

            if (hit_left && hit_right) {
                if (left_enter <= right_enter) {
                    stack[top++] = node.offset;
                    stack[top++] = index + 1;
                }
                else {
                    stack[top++] = index + 1;
                    stack[top++] = node.offset;
                }
            }
            else if (hit_left) {
                stack[top++] = index + 1;
            }
            else if (hit_right) {
                stack[top++] = node.offset;
            }
        }
    }

    /**
     * @brief Recomputes every node's volume from the primitives, without changing the topology.
     *
     * @details
     * - One reverse pass over the flat array: children always sit after their parent, so the sweep is a valid post-order.
     * - Allocation-free and O(n). It does not improve the shape of the tree; if repeated updates have loosened the
     *   leaves, `qualityReport()` tells the caller it is time to build again.
     * - A SEALED tree has nothing to refit from and answers `std::logic_error` (see `seal()`).
     *
     * @throws std::logic_error when the tree is sealed.
     */
    void refit()
    {
        if (shape_.nodes.empty()) {
            return;
        }
        rejectWhenSealed("refit()");
        detail::refitNodes<T, Volume>(shape_.nodes, node_bounds_, shape_.order, prim_bounds_);
    }

    /**
     * @brief Checks the invariants a build algorithm is required to produce.
     *
     * @details
     * - The contract: `primOrder()` is a permutation, nodes are written depth-first with the left child at `node + 1`,
     *   every parent encloses both children, and the height stays within the limit.
     * - Worth calling after a custom build algorithm. The built-in ones are covered by tests; a third-party algorithm
     *   has only this function standing between it and a query that silently misses.
     * - What it checks is the tree's own combinatorics; the per-primitive volumes are only COUNTED, never inspected, so
     *   a sealed tree validates exactly like a mutable one.
     *
     * @return True when the tree satisfies every invariant.
     */
    [[nodiscard]] bool validate() const
    {
        const std::size_t count = primCount();
        if (shape_.nodes.empty()) {
            return count == 0u; // nothing built is valid only when there is nothing to describe
        }
        if (shape_.order.size() != count || shape_.nodes.size() != node_bounds_.size()) {
            return false;
        }

        std::vector<bool> seen(count, false);
        for (const PrimId id : shape_.order) {
            if (id >= count || seen[id]) {
                return false;
            }
            seen[id] = true;
        }

        // Every node must sit inside the array, and every parent must enclose its children.
        for (std::size_t index = 0; index < shape_.nodes.size(); ++index) {
            const Node& node = shape_.nodes[index];
            if (node.count != 0) {
                if (node.count > count || node.offset > count - node.count) {
                    return false;
                }
                continue;
            }
            if (index + 1 >= shape_.nodes.size() || node.offset >= shape_.nodes.size()) {
                return false;
            }
            if (!encloses(node_bounds_[index], node_bounds_[index + 1]) || !encloses(node_bounds_[index], node_bounds_[node.offset])) {
                return false;
            }
        }

        return shape_.depth <= kMaxDepth && shape_.nodes.size() <= 2 * count;
    }

    /**
     * @brief Returns the primitive ids in leaf order.
     *
     * @details
     * - This is the tree's traversal order: iterating it walks primitives in the order the leaves sit in memory, which
     *   is also a sensible order to upload or batch-process them in.
     *
     * @return The permutation, or an empty span before the first build.
     */
    [[nodiscard]] std::span<const PrimId> primOrder() const { return shape_.order; }

    /**
     * @brief Returns the tree's SHAPE as one OWNED value: its nodes, its primitive order and its height.
     *
     * @details
     * - This is the half of a tree that a build cannot recompute cheaply, and therefore the half worth storing: with it
     *   a reader re-derives every box from the primitives (see `RestoreTopology`) instead of re-running a split.
     * - It is ONE value rather than `primOrder()` plus a node array: the node offsets index into the primitive order and
     *   the height states the node array's own shape, so the three only mean anything together. Returning them apart is
     *   how one tree's nodes get paired with another tree's order.
     * - The shape IS the tree's own storage, so this call COPIES nothing that `topologyView()` can borrow: reach for this
     *   one when the value has to outlive the tree or be edited, and for the view when it only has to be read or encoded.
     * - The order is the node array's: depth-first, the left child always at `node + 1` (see `BvhNodeTopology`).
     *
     * @return The shape; empty (no nodes, no order, height 0) before the first build.
     */
    [[nodiscard]] BvhTopology topology() const { return shape_; }

    /**
     * @brief Returns a BORROWED view of the tree's shape, with no copy.
     *
     * @details
     * - The encoding side of the storage story: a writer walks these arrays in place, so saving a tree costs the file
     *   and nothing else. The view dies with the next build, which is what a borrow states.
     * - What a stored shape is and what the tree holds are the same three arrays, which is why restoring can adopt them
     *   instead of transcribing them (see `restoreTopology()`).
     *
     * @warning The view borrows this tree: it must not outlive it, nor a call that rebuilds or clears it.
     *
     * @return A view over the tree's own arrays; empty before the first build.
     */
    [[nodiscard]] BvhTopologyView topologyView() const noexcept
    {
        return BvhTopologyView{ shape_.nodes, shape_.order, shape_.depth };
    }

    /**
     * @brief Installs a stored shape, then re-derives every box from the registered primitives.
     *
     * @details
     * - The move-in half of the storage story: take the shape by value and let the caller `std::move` the one it decoded,
     *   and the tree adopts its arrays instead of copying node by node. The boxes are then computed by the same pass
     *   `refit()` uses, so an adopted tree cannot have a different notion of what a box is.
     * - Nothing is trusted: the shape has to describe THIS tree's primitives (an order that is a permutation, every leaf
     *   inside it, every internal node pointing forward, the node array one tree, and the stored height that tree's own).
     *   A shape that fails leaves the tree UNBUILT - a caller treats that as "no cache", not as damage.
     * - The tree must already hold the primitives the shape was built over; check `primCount()` or register first.
     *
     * @param shape The stored shape, nodes, primitive order and height all from one tree; consumed.
     * @return true when the tree holds a built tree afterwards.
     * @throws std::logic_error when the tree is sealed: there are no volumes to re-derive the boxes from.
     */
    [[nodiscard]] bool restoreTopology(BvhTopology shape)
    {
        rejectWhenSealed("restoreTopology()");
        if (!detail::installableHeight(shape.view(), static_cast<PrimId>(prim_bounds_.size()), kMaxDepth).has_value()) {
            dropTree();
            return false;
        }
        shape_ = std::move(shape);
        node_bounds_.assign(shape_.nodes.size(), VolumeType{});
        detail::refitNodes<T, Volume>(shape_.nodes, node_bounds_, shape_.order, prim_bounds_);
        return true;
    }

    /**
     * @brief Reports the tree's current cost.
     *
     * @return Leaf area sum, leaf count and depth; all zero before a build.
     */
    [[nodiscard]] Quality qualityReport() const
    {
        Quality quality;
        quality.depth = shape_.depth;
        for (std::size_t index = 0; index < shape_.nodes.size(); ++index) {
            if (shape_.nodes[index].count != 0) {
                quality.leaf_half_area_sum += static_cast<double>(detail::VolumeTraits<VolumeType>::halfArea(node_bounds_[index]));
                ++quality.leaf_count;
            }
        }
        return quality;
    }

  private:
    friend class BvhBuilder;

    /**
     * @brief Prepares the arrays a build writes into.
     *
     * @details
     * - Resets the primitive order to the identity, so a build is a pure function of the registered volumes: two builds
     *   of the same input produce the same bytes, whatever happened in between.
     * - Keeps the node array's capacity but does not size it: how many leaves a split produces depends on the split, so
     *   a pre-computed upper bound would be either wrong or wasteful. The builder appends and the array grows on demand.
     *
     * @param leaf_size Maximum primitives per leaf; kept for the record of what the build was asked for.
     * @param max_depth Height limit the builder must respect.
     * @return False when there is nothing to build.
     * @throws std::logic_error when the tree is sealed: a build partitions the per-primitive volumes it no longer holds.
     */
    bool prepareForBuild(unsigned leaf_size, unsigned max_depth)
    {
        (void)leaf_size;
        (void)max_depth;

        rejectWhenSealed("build");
        if (prim_bounds_.empty()) {
            dropTree();
            return false;
        }

        const auto count = static_cast<PrimId>(prim_bounds_.size());

        shape_.order.resize(count);
        for (PrimId index = 0; index < count; ++index) {
            shape_.order[index] = index;
        }
        // `clear()` keeps the capacity, so a rebuild allocates nothing once the tree has reached its size once.
        shape_.nodes.clear();
        node_bounds_.clear();
        shape_.depth = 0;
        return true;
    }

    /**
     * @brief Records the result of a successful build.
     *
     * @param job The job the builder filled.
     */
    void finishBuild(const detail::BuildJob<T, Volume>& job)
    {
        shape_.depth = job.depth;
        if (job.node_count == 0) {
            // An algorithm that wrote nothing (or gave up) leaves no tree at all: a query must not walk its partial slots.
            dropTree();
        }
    }

    /**
     * @brief Drops the tree while keeping the registered primitives.
     */
    void dropTree()
    {
        shape_.nodes.clear();
        shape_.order.clear();
        shape_.depth = 0;
        node_bounds_.clear();
    }

    /**
     * @brief Reports whether one volume contains another, by testing the inner volume's corners.
     *
     * @param outer The candidate container.
     * @param inner The candidate content.
     * @return True when every corner of `inner` lies inside `outer`.
     */
    [[nodiscard]] static bool encloses(const VolumeType& outer, const VolumeType& inner)
    {
        for (const auto& corner : detail::VolumeTraits<VolumeType>::corners(inner)) {
            if (!detail::VolumeTraits<VolumeType>::contains(outer, corner)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Runs a visitor over one leaf's primitives.
     *
     * @details
     * - The visitor's volume is the primitive's own on a mutable tree, and @p leaf_bounds on a sealed one - the leaf's
     *   box contains every primitive it holds, so the answer stays a superset either way (see `seal()`).
     *
     * @tparam Visit Callable `bool(PrimId, const VolumeType&)`.
     * @param node        The leaf.
     * @param leaf_bounds The leaf's volume, handed out when the per-primitive volumes are gone.
     * @param visit       The visitor.
     * @return False when the visitor asked to stop.
     */
    template <typename Visit>
    bool visitLeaves(const Node& node, const VolumeType& leaf_bounds, Visit& visit) const
    {
        const bool sealed = prim_bounds_.empty();
        for (PrimId index = node.offset; index < node.offset + node.count; ++index) {
            const PrimId id = shape_.order[index];
            if (!visit(id, sealed ? leaf_bounds : prim_bounds_[id])) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Refuses an operation that would need the per-primitive volumes a sealed tree dropped.
     *
     * @details
     * - A sealed tree is a RESULT, and every operation refused here rewrites the data it was computed from. Saying so
     *   loudly is the difference between "this tree answers queries" and a silent no-op that leaves a caller believing
     *   its update landed (see `seal()`).
     *
     * @param what The operation, as the message names it.
     * @throws std::logic_error when the tree is sealed.
     */
    void rejectWhenSealed(const char* what) const
    {
        if (isSealed()) {
            throw std::logic_error(std::string("vn::geom::spatial::Bvh: ") + what +
                                   " needs the per-primitive volumes a sealed tree dropped (see seal())");
        }
    }

    std::vector<VolumeType> prim_bounds_; // One volume per registered primitive, indexed by PrimId.
    BvhTopology          shape_;       // The combinatorics: node slots, primitive order and height.
    std::vector<VolumeType> node_bounds_; // One volume per node slot, parallel to shape_.nodes.
};

VN_GEOM_SPATIAL_NS_END
