#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include <vine/geom/spatial/Bvh.hpp>
#include <vine/geom/spatial/BvhTopology.hpp>
#include <vine/geom/spatial/detail/BvhBuildJob.hpp>
#include <vine/geom/spatial/detail/BvhBuilders.hpp>
#include <vine/geom/spatial/detail/BvhRefit.hpp>
#include <vine/geom/spatial/detail/BvhTopologyCheck.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief What `BvhBuilder::buildWith()` accepts: an algorithm that fills one build job and reports its node count.
 *
 * @details
 * - The CONTRACT a build algorithm owes (a permutation of the primitive order, depth-first nodes with the left child at
 *   `node + 1`, parents enclosing their children, a height within `max_depth`) cannot be checked by the type system, but
 *   its SHAPE can be: one call, one job, one node count. Stating the shape as a concept turns "this callable takes the
 *   wrong parameter" into one constraint diagnostic instead of a page of template errors.
 * - The algorithms in this library (`detail::SahBuild`, `detail::MedianBuild`, `RestoreTopology`) all model it, and so
 *   does any caller-supplied one - that is the extension point, and this is its signature.
 *
 * @tparam Algorithm Callable type; considered as an lvalue, so a `const` call operator is fine.
 * @tparam T         Floating-point scalar type.
 * @tparam Volume    Bounding-volume template.
 */
template <typename Algorithm, typename T, template <typename> class Volume>
concept BvhBuildAlgorithm = requires(Algorithm& algorithm, detail::BuildJob<T, Volume>& job) {
    { algorithm(job) } -> std::convertible_to<std::uint32_t>;
};

/**
 * @brief Which construction algorithm `BvhBuilder` uses.
 *
 * @details
 * - These are names for the built-in algorithms, not the limit of what the builder can run: `buildWith()` takes any
 *   algorithm that fills a build job.
 */
enum class BvhBuildQuality
{
    kFast, ///< Median split along the longest axis: one scan per level, for rebuilds inside a frame budget.
    kSah,  ///< Binned surface-area heuristic: the default, better tree for a slightly higher build cost.
};

/**
 * @brief Constructs a `Bvh`: the parameters and the algorithms, and nothing else.
 *
 * @details
 * - **This type holds no structure.** It writes the topology of a tree it is handed and keeps no reference to it, so a
 *   builder is a value: copy it, discard it, use it on another tree.
 * - It never touches primitive volumes except through the build job, which hands an algorithm three spans and two
 *   limits; everything else in the tree stays out of reach.
 * - The default parameters are the ones the literature settled on: four primitives per leaf, depth capped at the
 *   structure's own limit, binned SAH.
 *
 * @warning
 * - Building needs exclusive access to the tree. Do not run `build()` concurrently with a query on the same object, and
 *   do not use one builder from two threads at once: it hands its scratch state to the algorithm.
 *
 * @note
 * - After a custom algorithm has built a tree, call `Bvh::validate()`: nothing else checks that the algorithm honoured
 *   the layout contract, and a violated contract shows up as a query that silently misses.
 */
class BvhBuilder
{
  public:
    /**
     * @brief Sets how many primitives a leaf may hold.
     *
     * @param leaf_size Maximum leaf size; zero is rejected.
     * @return This builder, for chaining.
     * @throws std::invalid_argument when `leaf_size` is zero.
     */
    BvhBuilder& leafSize(unsigned leaf_size)
    {
        if (leaf_size == 0) {
            throw std::invalid_argument("vn::geom::spatial::BvhBuilder: the leaf size must be at least one");
        }
        leaf_size_ = leaf_size;
        return *this;
    }

    /**
     * @brief Sets the height limit the build must stay within.
     *
     * @param max_depth Maximum depth; one to `kBvhMaxDepth`.
     * @return This builder, for chaining.
     * @throws std::invalid_argument when `max_depth` is zero or deeper than `kBvhMaxDepth`, because the traversal stacks
     *                               are sized for that limit.
     */
    BvhBuilder& maxDepth(unsigned max_depth)
    {
        if (max_depth == 0 || max_depth > kBvhMaxDepth) {
            throw std::invalid_argument("vn::geom::spatial::BvhBuilder: the depth limit must be between 1 and kBvhMaxDepth");
        }
        max_depth_ = max_depth;
        return *this;
    }

    /**
     * @brief Selects one of the built-in algorithms.
     *
     * @param quality Algorithm to use for the next build.
     * @return This builder, for chaining.
     */
    BvhBuilder& quality(BvhBuildQuality quality)
    {
        quality_ = quality;
        return *this;
    }

    /**
     * @brief Returns how many primitives a leaf may hold.
     *
     * @return The leaf size.
     */
    [[nodiscard]] unsigned leafSize() const { return leaf_size_; }

    /**
     * @brief Returns the height limit the build stays within.
     *
     * @return The depth limit.
     */
    [[nodiscard]] unsigned maxDepth() const { return max_depth_; }

    /**
     * @brief Returns which built-in algorithm `build()` would run.
     *
     * @return The selected quality.
     */
    [[nodiscard]] BvhBuildQuality quality() const { return quality_; }

    /**
     * @brief Builds the tree with the selected built-in algorithm.
     *
     * @details
     * - Repeated builds reuse the tree's storage: with enough capacity already reserved, no allocation happens here.
     * - The primitive order is reset before every build, so the result depends only on the registered volumes.
     *
     * @tparam T      Floating-point scalar type.
     * @tparam Volume Bounding-volume template.
     * @param bvh     The tree to fill; its registered primitives are what gets partitioned.
     * @throws std::logic_error when @p bvh is sealed: a build partitions the per-primitive volumes it no longer holds
     *                          (see `Bvh::seal()`).
     */
    template <typename T, template <typename> class Volume>
    void build(Bvh<T, Volume>& bvh) const
    {
        switch (quality_) {
            case BvhBuildQuality::kFast:
                buildWith(bvh, detail::MedianBuild<T, Volume>{});
                return;
            case BvhBuildQuality::kSah:
            default:
                buildWith(bvh, detail::SahBuild<T, Volume>{});
                return;
        }
    }

    /**
     * @brief Builds the tree with an algorithm the caller supplies.
     *
     * @details
     * - This is the extension point: a spatial-split builder, a GPU-friendly linear builder or a test stub is written
     *   outside this class and plugged in here, with no change to the tree.
     * - The algorithm receives a `detail::BuildJob` and returns the number of node slots it wrote. It must leave the
     *   primitive order a permutation, write nodes depth-first with the left child at `node + 1`, keep every parent
     *   enclosing its children, and respect `max_depth`.
     *
     * @tparam T         Floating-point scalar type.
     * @tparam Volume    Bounding-volume template.
     * @tparam Algorithm Build algorithm; anything modelling `BvhBuildAlgorithm`.
     * @param bvh        The tree to fill.
     * @param algorithm  The construction algorithm.
     * @throws std::logic_error when @p bvh is sealed (see `Bvh::seal()`).
     */
    template <typename T, template <typename> class Volume, BvhBuildAlgorithm<T, Volume> Algorithm>
    void buildWith(Bvh<T, Volume>& bvh, Algorithm&& algorithm) const
    {
        if (!bvh.prepareForBuild(leaf_size_, max_depth_)) {
            return;
        }

        detail::BuildJob<T, Volume> job{ std::span<const typename Bvh<T, Volume>::VolumeType>(bvh.prim_bounds_),
                                         std::span<typename Bvh<T, Volume>::PrimId>(bvh.shape_.order),
                                         bvh.shape_.nodes,
                                         bvh.node_bounds_,
                                         leaf_size_,
                                         max_depth_ };

        job.node_count = algorithm(job);
        bvh.finishBuild(job);
    }

  private:
    unsigned        leaf_size_{ 4 };
    unsigned        max_depth_{ kBvhMaxDepth };
    BvhBuildQuality quality_{ BvhBuildQuality::kSah };
};

/**
 * @brief A construction algorithm that puts a STORED grouping into a tree instead of computing a split.
 *
 * @details
 * - It is a build algorithm like the median and SAH ones - same `BvhBuilder::buildWith()` door, same `finishBuild` tail -
 *   so "how a tree came to have this shape" stays one concept: every way of building one is content written into a
 *   `BuildJob`, and none of them can leave the tree in a state the build path would not produce (see
 *   `.ai/design/geometry-bvh.md` 2.13).
 * - It copies the primitive order, appends the nodes with their stored `offset`/`count`, and then re-derives every box
 *   with `detail::refitNodes` - the same pass `refit()` uses. The primitives' volumes must already be registered on the
 *   tree (one per triangle, in triangle order), which is what `restoreTriangleBvh()` does before calling this.
 * - The COPYING door. A caller that owns the shape it decoded should prefer `Bvh::restoreTopology()`, which adopts the
 *   same arrays by moving them and validates them through the same `detail::installableHeight()`.
 * - Nothing is trusted: a node count outside `[1, 2n]`, a leaf outside the primitive order, an internal node whose right
 *   child is not strictly after its left one, an order that is not a permutation, an unreachable node or a height that
 *   is not the tree's own all make the call return 0, which leaves the tree unbuilt. A caller treats that as "no
 *   cache" rather than as damage.
 *
 * @warning
 * - It BORROWS the shape it is given (a `BvhTopologyView` says so): the arrays have to outlive the `buildWith()` call that
 *   runs it - a temporary is fine, it lives to the end of the full expression that contains the call. Everything the tree
 *   keeps is written into the build job, so nothing refers to the shape afterwards.
 */
class RestoreTopology
{
  public:
    /**
     * @brief Prepares a restore of @p shape.
     *
     * @param shape The shape to install: nodes, primitive order and height, all from one tree.
     */
    explicit RestoreTopology(BvhTopologyView shape)
      : shape_(shape)
    {}

    /**
     * @brief Fills @p job with the stored grouping.
     *
     * @details
     * - Reads as a checklist, and the checklist itself is `detail::installableHeight()` - the one statement of what
     *   makes a shape describe a tree, shared with the move-in path.
     *
     * @tparam T      Floating-point scalar type.
     * @tparam Volume Bounding-volume template.
     * @param job     The build job to fill; `order` and `nodes` are written, `depth` and `node_count` are set.
     * @return The number of node slots used, or 0 when the stored shape does not describe this tree.
     */
    template <typename T, template <typename> class Volume>
    std::uint32_t operator()(detail::BuildJob<T, Volume>& job) const
    {
        const auto                    prim_count = static_cast<std::uint32_t>(job.prim_bounds.size());
        const std::optional<unsigned> height     = detail::installableHeight(shape_, prim_count, job.max_depth);
        if (!height.has_value()) {
            return 0;
        }
        writeInto(job, *height);
        return static_cast<std::uint32_t>(shape_.nodes.size());
    }

  private:
    /**
     * @brief Copies the validated shape into @p job and re-derives every box from the primitives.
     *
     * @tparam T      Floating-point scalar type.
     * @tparam Volume Bounding-volume template.
     * @param job     The build job to fill.
     * @param height  The tree's height, already verified against the node array and the job's limit.
     */
    template <typename T, template <typename> class Volume>
    void writeInto(detail::BuildJob<T, Volume>& job, unsigned height) const
    {
        std::copy(shape_.order.begin(), shape_.order.end(), job.order.begin());
        job.nodes.clear();
        job.node_bounds.clear();
        job.nodes.reserve(shape_.nodes.size());
        job.node_bounds.reserve(shape_.nodes.size());
        for (const BvhNodeTopology& node : shape_.nodes) {
            const auto index  = job.appendNode();
            job.nodes[index] = node;
        }
        detail::refitNodes<T, Volume>(job.nodes, job.node_bounds, job.order, job.prim_bounds);
        job.depth = height;
    }

    BvhTopologyView shape_;
};

VN_GEOM_SPATIAL_NS_END
