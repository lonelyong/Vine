#pragma once

#include <vine/geom/geom_global.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include <vine/math/Point3.hpp>
#include <vine/math/Types.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/detail/BvhBuildJob.hpp>
#include <vine/geom/spatial/detail/VolumeTraits.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace detail
{

/**
 * @brief World-axis extents of a volume, longest axis first, for choosing a splitting axis.
 *
 * @details
 * - Measured from the eight corners, so it works for both a box aligned with the world and an oriented one without the
 *   volume type having to expose its own extent accessors.
 *
 * @param volume The volume.
 * @return Extent along X, Y and Z.
 */
template <typename T, template <typename> class Volume>
[[nodiscard]] vn::math::Vector3<T> worldExtents(const Volume<T>& volume)
{
    const auto corners = VolumeTraits<Volume<T>>::corners(volume);
    T          low[3]  = { corners[0].x, corners[0].y, corners[0].z };
    T          high[3] = { corners[0].x, corners[0].y, corners[0].z };
    for (const auto& corner : corners) {
        const T coordinate[3] = { corner.x, corner.y, corner.z };
        for (int axis = 0; axis < 3; ++axis) {
            low[axis]  = coordinate[axis] < low[axis] ? coordinate[axis] : low[axis];
            high[axis] = coordinate[axis] > high[axis] ? coordinate[axis] : high[axis];
        }
    }
    return vn::math::Vector3<T>(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
}

/**
 * @brief Returns the index of the longest world axis of a volume.
 *
 * @param volume The volume.
 * @return 0 for X, 1 for Y, 2 for Z.
 */
template <typename T, template <typename> class Volume>
[[nodiscard]] int longestAxis(const Volume<T>& volume)
{
    const auto extents = worldExtents(volume);
    if (extents.x >= extents.y && extents.x >= extents.z) {
        return 0;
    }
    return extents.y >= extents.z ? 1 : 2;
}

/**
 * @brief Returns one coordinate of a volume's centre.
 *
 * @param volume The volume.
 * @param axis   Axis index (0 = X, 1 = Y, 2 = Z).
 * @return The centre coordinate along that axis.
 */
template <typename T, template <typename> class Volume>
[[nodiscard]] T centerCoordinate(const Volume<T>& volume, int axis)
{
    const auto centre = VolumeTraits<Volume<T>>::center(volume);
    return axis == 0 ? centre.x : (axis == 1 ? centre.y : centre.z);
}

/**
 * @brief Splits a run of primitives at the median of their centres along one axis.
 *
 * @details
 * - One `nth_element` pass, no scratch memory: the comparator computes the two centres on demand, which is cheaper than
 *   materialising an array of centres for a run this size.
 * - A median split always produces two non-empty halves, so it needs no fallback and is the safety net the SAH splitter
 *   falls back to when its bins are degenerate.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct MedianSplit
{
    /**
     * @brief Splits `[first, first + count)` in place.
     *
     * @param job   The build job.
     * @param first First index into the primitive order.
     * @param count Number of primitives; must be at least two.
     * @param axis  Axis to sort along.
     * @return Index of the first primitive of the right half, or 0 when no split was possible.
     */
    vn::geom::spatial::detail::BuildJob<T, Volume>& job;
    std::uint32_t                                   operator()(std::uint32_t first, std::uint32_t count, int axis) const
    {
        const std::uint32_t mid = count / 2;
        const auto          begin = job.order.begin() + static_cast<std::ptrdiff_t>(first);
        std::nth_element(begin, begin + mid, begin + count, [this, axis](std::uint32_t lhs, std::uint32_t rhs) {
            return centerCoordinate<T, Volume>(job.bounds[lhs], axis) <
                   centerCoordinate<T, Volume>(job.bounds[rhs], axis);
        });
        return count > 1 ? first + mid : 0;
    }
};

/**
 * @brief Splits a run of primitives at the surface-area-heuristic minimum over 12 bins.
 *
 * @details
 * - Bins the centres along one axis, sweeps left and right accumulating the merged bounds, and keeps the plane with the
 *   lowest `count_left · halfArea_left + count_right · halfArea_right` — the standard binned SAH of Wald 2007.
 * - Returns 0 when the bins are degenerate (every centre equal within the bin width), which tells the caller to fall
 *   back to a median split; that is what keeps pathological inputs from producing empty halves.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct SahSplit
{
    /// Number of bins per split; 12 is the value the literature settled on and the one pbrt ships.
    static constexpr std::uint32_t kBins = 12;

    vn::geom::spatial::detail::BuildJob<T, Volume>& job;

    /**
     * @brief Splits `[first, first + count)` in place.
     *
     * @param first First index into the primitive order.
     * @param count Number of primitives; must be at least two.
     * @param axis  Axis to split along.
     * @return Index of the first primitive of the right half, or 0 when no useful split was found.
     */
    std::uint32_t operator()(std::uint32_t first, std::uint32_t count, int axis) const
    {
        using Traits = VolumeTraits<Volume<T>>;

        T lowest  = std::numeric_limits<T>::max();
        T highest = std::numeric_limits<T>::lowest();
        for (std::uint32_t index = first; index < first + count; ++index) {
            const T value = centerCoordinate<T, Volume>(job.bounds[job.order[index]], axis);
            lowest        = value < lowest ? value : lowest;
            highest       = value > highest ? value : highest;
        }
        if (!(highest > lowest)) {
            return 0; // every centre on the same plane: no bin can separate them
        }

        const T        scale = T(kBins) / (highest - lowest);
        std::array<T, kBins>            costs{};
        std::array<Volume<T>, kBins>    bin_bounds{};
        std::array<std::uint32_t, kBins> bin_counts{};
        std::array<bool, kBins>         bin_used{};
        (void)costs;

        for (std::uint32_t index = first; index < first + count; ++index) {
            const std::uint32_t id   = job.order[index];
            const T             value = centerCoordinate<T, Volume>(job.bounds[id], axis);
            const auto          bin   = static_cast<std::uint32_t>((value - lowest) * scale);
            const std::uint32_t slot  = bin < kBins ? bin : kBins - 1;
            bin_bounds[slot] = bin_used[slot] ? Traits::merged(bin_bounds[slot], job.bounds[id]) : job.bounds[id];
            bin_used[slot]   = true;
            ++bin_counts[slot];
        }

        // Sweep from the left: bounds and counts of everything at or below bin i.
        std::array<Volume<T>, kBins>    left_bounds{};
        std::array<std::uint32_t, kBins> left_counts{};
        bool                             have_left = false;
        for (std::uint32_t bin = 0; bin < kBins; ++bin) {
            if (bin_used[bin]) {
                left_bounds[bin] = have_left ? Traits::merged(left_bounds[bin - 1], bin_bounds[bin]) : bin_bounds[bin];
                have_left        = true;
            }
            else {
                left_bounds[bin] = have_left ? left_bounds[bin - 1] : bin_bounds[bin];
            }
            left_counts[bin] = (bin == 0 ? 0 : left_counts[bin - 1]) + bin_counts[bin];
        }

        // Sweep from the right, then pick the plane with the lowest estimated cost.
        T                   best_cost = std::numeric_limits<T>::max();
        std::uint32_t       best_bin  = kBins;
        std::uint32_t       right_running = 0;
        bool                have_right    = false;
        Volume<T>           right_bounds{};
        for (std::uint32_t bin = kBins - 1; bin > 0; --bin) {
            if (bin_used[bin]) {
                right_bounds = have_right ? Traits::merged(right_bounds, bin_bounds[bin]) : bin_bounds[bin];
                have_right   = true;
            }
            right_running += bin_counts[bin];

            const std::uint32_t left_count = left_counts[bin - 1];
            if (left_count == 0 || right_running == 0 || !have_right) {
                continue;
            }
            const T cost = T(left_count) * Traits::halfArea(left_bounds[bin - 1]) +
                           T(right_running) * Traits::halfArea(right_bounds);
            if (cost < best_cost) {
                best_cost = cost;
                best_bin  = bin;
            }
        }

        if (best_bin == kBins) {
            return 0;
        }

        // Partition in place: everything below the chosen plane first.
        const auto begin = job.order.begin() + static_cast<std::ptrdiff_t>(first);
        const auto split = std::partition(begin, begin + count, [this, axis, lowest, scale, best_bin](std::uint32_t id) {
            const T value = centerCoordinate<T, Volume>(job.bounds[id], axis);
            const auto bin = static_cast<std::uint32_t>((value - lowest) * scale);
            return (bin < kBins ? bin : kBins - 1) < best_bin;
        });

        const auto position = static_cast<std::uint32_t>(split - job.order.begin());
        // A partition that put everything on one side is not a split.
        return (position == first || position == first + count) ? 0 : position;
    }
};

/**
 * @brief Builds a tree top-down with a caller-supplied splitting rule.
 *
 * @details
 * - Recursion depth is bounded by `max_depth`, which the job carries, so the recursion cannot run away; the alternative
 *   (an explicit stack) buys nothing here.
 * - Node slots are handed out in depth-first order, which is what makes the left child sit at `node + 1` and lets a
 *   node store only its right child.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 * @tparam Split  Splitting rule: `uint32 operator()(first, count, axis)`, returning the right half's first index or 0.
 * @param job     The build job.
 * @param split   The splitting rule.
 * @return The number of node slots used.
 */
template <typename T, template <typename> class Volume, typename Split>
std::uint32_t buildTopDown(vn::geom::spatial::detail::BuildJob<T, Volume>& job, const Split& split)
{
    using Job = vn::geom::spatial::detail::BuildJob<T, Volume>;

    struct Builder
    {
        Job&         job;
        const Split& split;

        std::uint32_t build(std::uint32_t first, std::uint32_t count, unsigned depth)
        {
            const auto node = job.appendNode();
            job.nodes[node].bounds = job.rangeBounds(first, count);

            if (count <= job.leaf_size || depth + 1 >= job.max_depth) {
                job.writeLeaf(node, job.nodes[node].bounds, first, count);
                job.depth = depth > job.depth ? depth : job.depth;
                return node;
            }

            int axis = longestAxis<T, Volume>(job.nodes[node].bounds);
            auto middle = split(first, count, axis);

            if (middle == 0) {
                // The heuristic found no separable plane: fall back to a median split, which always separates.
                const MedianSplit<T, Volume> median{ job };
                middle = median(first, count, axis);
            }
            if (middle == 0) {
                job.writeLeaf(node, job.nodes[node].bounds, first, count);
                job.depth = depth > job.depth ? depth : job.depth;
                return node;
            }

            build(first, middle - first, depth + 1);
            const auto right = build(middle, first + count - middle, depth + 1);
            job.nodes[node].offset = right;
            return node;
        }
    };

    Builder builder{ job, split };
    builder.build(0, static_cast<std::uint32_t>(job.order.size()), 0);
    return job.node_count;
}

/**
 * @brief The default build: binned SAH, with a median fallback when the bins separate nothing.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct SahBuild
{
    /**
     * @brief Builds the tree in `job`.
     *
     * @param job The build job; `order` and `nodes` are filled, `depth` and `node_count` are written.
     * @return The number of node slots used.
     */
    std::uint32_t operator()(vn::geom::spatial::detail::BuildJob<T, Volume>& job) const
    {
        return buildTopDown<T, Volume>(job, SahSplit<T, Volume>{ job });
    }
};

/**
 * @brief A cheaper build: median split along the longest axis, one scan per level.
 *
 * @details
 * - Intended for callers that rebuild often and care more about the build's cost than the tree's quality; a frame-time
 *   rebuild of a medium scene is the case it was written for.
 *
 * @tparam T Floating-point scalar type.
 * @tparam Volume Bounding-volume template.
 */
template <typename T, template <typename> class Volume>
struct MedianBuild
{
    /**
     * @brief Builds the tree in `job`.
     *
     * @param job The build job; `order` and `nodes` are filled, `depth` and `node_count` are written.
     * @return The number of node slots used.
     */
    std::uint32_t operator()(vn::geom::spatial::detail::BuildJob<T, Volume>& job) const
    {
        return buildTopDown<T, Volume>(job, MedianSplit<T, Volume>{ job });
    }
};

} // namespace detail

VN_GEOM_SPATIAL_NS_END
