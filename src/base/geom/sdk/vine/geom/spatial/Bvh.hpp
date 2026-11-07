#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/Ray.hpp>
#include <vine/geom/spatial/detail/BvhBuildJob.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

class BvhBuilder;

/// Height limit the traversal stacks are sized for; a builder may go shallower, never deeper.
inline constexpr unsigned kBvhMaxDepth = 64;

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
 * - `float` and `Aabb` give a 32-byte node, half a cache line; the tree is one contiguous array walked front to back.
 *
 * @warning
 * - A build is required before queries answer anything: `add()` after a build clears the nodes, so a query on a tree
 *   that has pending additions visits nothing rather than answering from a stale tree.
 * - `update()` changes a primitive's volume; the tree is only valid again after `refit()`, which keeps the topology.
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
    using VolumeT = Volume<T>;
    using PrimId  = std::uint32_t;
    using Node    = detail::BvhNode<T, Volume>;

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
     */
    PrimId add(const VolumeT& bounds)
    {
        if (!detail::VolumeTraits<VolumeT>::isFinite(bounds)) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: the primitive volume must be finite and not inverted");
        }
        if (bounds_.size() >= static_cast<std::size_t>(std::numeric_limits<PrimId>::max())) {
            throw std::length_error("vn::geom::spatial::Bvh: more primitives than a PrimId can index");
        }
        bounds_.push_back(bounds);
        dropTree();
        return static_cast<PrimId>(bounds_.size() - 1);
    }

    /**
     * @brief Registers `count` primitives whose volumes this call computes on demand.
     *
     * @details
     * - Takes a callable instead of a span so a caller holding vertices and indices does not have to materialise a
     *   temporary array of volumes first; the volumes are computed once and stored.
     *
     * @tparam BoundsOf Callable `VolumeT(std::size_t)` returning the volume of primitive `index`.
     * @param count     Number of primitives.
     * @param bounds_of Computes the volume of one primitive.
     * @throws std::invalid_argument when any volume is not finite, or `bounds_of` throws.
     * @throws std::length_error   when the ids would not fit in `PrimId`.
     */
    template <typename BoundsOf>
    void addRange(std::size_t count, BoundsOf&& bounds_of)
    {
        bounds_.reserve(bounds_.size() + count);
        for (std::size_t index = 0; index < count; ++index) {
            add(static_cast<const VolumeT&>(bounds_of(index)));
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
     */
    void update(PrimId id, const VolumeT& bounds)
    {
        if (id >= bounds_.size()) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: update() needs an id returned by add()");
        }
        if (!detail::VolumeTraits<VolumeT>::isFinite(bounds)) {
            throw std::invalid_argument("vn::geom::spatial::Bvh: the primitive volume must be finite and not inverted");
        }
        bounds_[id] = bounds;
    }

    /**
     * @brief Reserves storage for `prim_count` primitives, so later registrations do not allocate.
     *
     * @param prim_count Expected primitive count.
     */
    void reserve(std::size_t prim_count) { bounds_.reserve(prim_count); }

    /**
     * @brief Forgets every primitive and the tree, keeping the allocated storage.
     *
     * @details
     * - The next registration starts numbering at zero again; the capacity of all three arrays is kept, which is what
     *   makes a rebuild cycle allocation-free.
     */
    void clear()
    {
        bounds_.clear();
        prim_order_.clear();
        nodes_.clear();
        depth_ = 0;
        built_ = false;
    }

    /**
     * @brief Returns how many primitives are registered.
     *
     * @return The primitive count, whether or not a tree has been built.
     */
    [[nodiscard]] std::size_t primCount() const { return bounds_.size(); }

    /**
     * @brief Reports whether no primitive is registered.
     *
     * @return True when the tree holds nothing.
     */
    [[nodiscard]] bool empty() const { return bounds_.empty(); }

    /**
     * @brief Reports whether a build has been performed since the last registration.
     *
     * @return True when the nodes describe the current primitives.
     */
    [[nodiscard]] bool isBuilt() const { return built_; }

    /**
     * @brief Visits every primitive whose leaf volume overlaps `query`.
     *
     * @details
     * - Allocation-free and `const`: the traversal stack lives on the call's own stack.
     * - The visit receives the leaf's volume, which is conservative: a primitive reported here may still miss the
     *   query under an exact test.
     *
     * @tparam Visit Callable `bool(PrimId, const VolumeT&)`, returning false to stop the traversal.
     * @param query  Volume to test against.
     * @param visit  Called for each candidate.
     */
    template <typename Visit>
    void forEachOverlap(const VolumeT& query, Visit&& visit) const
    {
        if (nodes_.empty()) {
            return;
        }
        std::array<PrimId, kMaxDepth + 2> stack{};
        unsigned                          top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const PrimId index = stack[--top];
            const Node&  node  = nodes_[index];
            if (!detail::VolumeTraits<VolumeT>::overlaps(node.bounds, query)) {
                continue;
            }
            if (node.count != 0) {
                if (!visitLeaves(node, visit)) {
                    return;
                }
                continue;
            }
            stack[top++] = node.offset;  // right child
            stack[top++] = index + 1;    // left child is always the next slot after the parent
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
     * @tparam Visit Callable `bool(PrimId, const VolumeT&, T& max_t)`, returning false to stop the traversal.
     * @param ray    The ray; an aimless ray visits nothing.
     * @param visit  Called for each candidate.
     * @param max_t  Initial distance limit; updated by the visit to tighten the search.
     */
    template <typename Visit>
    void forEachRay(const Ray<T>& ray, Visit&& visit, T max_t = std::numeric_limits<T>::infinity()) const
    {
        if (nodes_.empty()) {
            return;
        }
        using Traits = detail::VolumeTraits<VolumeT>;

        const detail::PreparedRay<T> prepared = detail::PreparedRay<T>::from(ray);
        T                            limit    = max_t;

        std::array<PrimId, kMaxDepth + 2> stack{};
        unsigned                          top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const PrimId index = stack[--top];
            const Node&  node  = nodes_[index];
            T           enter = T(0);
            if (!Traits::hitByRay(node.bounds, prepared, enter) || enter > limit) {
                continue;
            }
            if (node.count != 0) {
                for (PrimId slot = node.offset; slot < node.offset + node.count; ++slot) {
                    const PrimId id = prim_order_[slot];
                    if (!visit(id, bounds_[id], limit)) {
                        return;
                    }
                }
                continue;
            }

            // Depth-first but nearest first: test both children, descend into the closer one.
            const Node& left  = nodes_[index + 1];
            const Node& right = nodes_[node.offset];
            T           left_enter  = T(0);
            T           right_enter = T(0);
            const bool  hit_left    = Traits::hitByRay(left.bounds, prepared, left_enter) && left_enter <= limit;
            const bool  hit_right   = Traits::hitByRay(right.bounds, prepared, right_enter) && right_enter <= limit;

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
     */
    void refit()
    {
        if (nodes_.empty()) {
            return;
        }
        for (std::size_t index = nodes_.size(); index-- > 0;) {
            Node& node = nodes_[index];
            if (node.count != 0) {
                node.bounds = leafBounds(node.offset, node.count);
                continue;
            }
            node.bounds = detail::VolumeTraits<VolumeT>::merged(nodes_[index + 1].bounds, nodes_[node.offset].bounds);
        }
    }

    /**
     * @brief Checks the invariants a build algorithm is required to produce.
     *
     * @details
     * - The contract: `primOrder()` is a permutation, nodes are written depth-first with the left child at `node + 1`,
     *   every parent encloses both children, and the height stays within the limit.
     * - Worth calling after a custom build algorithm. The built-in ones are covered by tests; a third-party algorithm
     *   has only this function standing between it and a query that silently misses.
     *
     * @return True when the tree satisfies every invariant.
     */
    [[nodiscard]] bool validate() const
    {
        if (!built_) {
            return bounds_.empty();
        }
        if (nodes_.empty() || prim_order_.size() != bounds_.size()) {
            return false;
        }

        std::vector<bool> seen(bounds_.size(), false);
        for (const PrimId id : prim_order_) {
            if (id >= bounds_.size() || seen[id]) {
                return false;
            }
            seen[id] = true;
        }

        // Every node must sit inside the array, and every parent must enclose its children.
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            const Node& node = nodes_[index];
            if (node.count != 0) {
                if (node.count > bounds_.size() || node.offset > bounds_.size() - node.count) {
                    return false;
                }
                continue;
            }
            if (index + 1 >= nodes_.size() || node.offset >= nodes_.size()) {
                return false;
            }
            if (!encloses(node.bounds, nodes_[index + 1].bounds) || !encloses(node.bounds, nodes_[node.offset].bounds)) {
                return false;
            }
        }

        return depth_ <= kMaxDepth && nodes_.size() <= 2 * bounds_.size();
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
    [[nodiscard]] std::span<const PrimId> primOrder() const { return prim_order_; }

    /**
     * @brief Reports the tree's current cost.
     *
     * @return Leaf area sum, leaf count and depth; all zero before a build.
     */
    [[nodiscard]] Quality qualityReport() const
    {
        Quality quality;
        quality.depth = depth_;
        for (const Node& node : nodes_) {
            if (node.count != 0) {
                quality.leaf_half_area_sum +=
                    static_cast<double>(detail::VolumeTraits<VolumeT>::halfArea(node.bounds));
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
     */
    bool prepareForBuild(unsigned leaf_size, unsigned max_depth)
    {
        (void)leaf_size;
        (void)max_depth;

        if (bounds_.empty()) {
            prim_order_.clear();
            nodes_.clear();
            depth_ = 0;
            built_ = false;
            return false;
        }

        const auto count = static_cast<PrimId>(bounds_.size());

        prim_order_.resize(count);
        for (PrimId index = 0; index < count; ++index) {
            prim_order_[index] = index;
        }
        // `clear()` keeps the capacity, so a rebuild allocates nothing once the tree has reached its size once.
        nodes_.clear();
        depth_ = 0;
        built_ = false;
        return true;
    }

    /**
     * @brief Records the result of a successful build.
     *
     * @param job The job the builder filled.
     */
    void finishBuild(const detail::BuildJob<T, Volume>& job)
    {
        depth_ = job.depth;
        built_ = job.node_count != 0;
    }

    /**
     * @brief Drops the tree while keeping the registered primitives.
     */
    void dropTree()
    {
        if (!nodes_.empty()) {
            nodes_.clear();
        }
        prim_order_.clear();
        depth_ = 0;
        built_ = false;
    }

    /**
     * @brief Returns the volume enclosing a run of the primitive order.
     *
     * @param first First index into the order.
     * @param count Number of primitives; must not be zero.
     * @return The merged volume.
     */
    [[nodiscard]] VolumeT leafBounds(PrimId first, PrimId count) const
    {
        VolumeT out = bounds_[prim_order_[first]];
        for (PrimId index = first + 1; index < first + count; ++index) {
            out = detail::VolumeTraits<VolumeT>::merged(out, bounds_[prim_order_[index]]);
        }
        return out;
    }

    /**
     * @brief Reports whether one volume contains another, by testing the inner volume's corners.
     *
     * @param outer The candidate container.
     * @param inner The candidate content.
     * @return True when every corner of `inner` lies inside `outer`.
     */
    [[nodiscard]] static bool encloses(const VolumeT& outer, const VolumeT& inner)
    {
        for (const auto& corner : detail::VolumeTraits<VolumeT>::corners(inner)) {
            if (!detail::VolumeTraits<VolumeT>::contains(outer, corner)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Runs a visitor over one leaf's primitives.
     *
     * @tparam Visit Callable `bool(PrimId, const VolumeT&)`.
     * @param node  The leaf.
     * @param visit The visitor.
     * @return False when the visitor asked to stop.
     */
    template <typename Visit>
    bool visitLeaves(const Node& node, Visit& visit) const
    {
        for (PrimId index = node.offset; index < node.offset + node.count; ++index) {
            const PrimId id = prim_order_[index];
            if (!visit(id, bounds_[id])) {
                return false;
            }
        }
        return true;
    }

    std::vector<VolumeT> bounds_;
    std::vector<PrimId>  prim_order_;
    std::vector<Node>    nodes_;
    unsigned             depth_{ 0 };
    bool                 built_{ false };
};

VN_GEOM_SPATIAL_NS_END
