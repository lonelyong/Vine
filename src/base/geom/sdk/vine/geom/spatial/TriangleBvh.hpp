#pragma once

#include <vine/geom/geom_global.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/math/Vector3.hpp>

#include <vine/geom/spatial/Aabb.hpp>
#include <vine/geom/spatial/Bvh.hpp>
#include <vine/geom/spatial/BvhBuilder.hpp>
#include <vine/geom/spatial/BvhTopology.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

/**
 * @brief What a TRIANGLE SOURCE owes: a vertex set, a triangle count and the corners of one triangle.
 *
 * @details
 * - A triangle tree numbers its primitives by TRIANGLE ORDINAL, so building one needs exactly three facts about the
 *   triangles: which vertices exist, how many triangles there are, and which three vertices triangle `t` uses. That is
 *   the whole contract, and it is spelled as a concept so the two frames that own triangles - a model's `Mesh` and a
 *   draw's `TriangleStream` - are wired to ONE definition of it instead of two copies of the same loop (`G1`, `G5`).
 * - `positions()` is a span of `Vec3f`, not a callable per vertex: every source in this library already holds its
 *   vertices contiguously, and the tree is `double`, so the widening happens once, in the registration below.
 * - The accessors are spelled the Qt way the rest of the library spells them (`positions()`, not `getPositions()`), so
 *   a type that models this concept reads like any other value class here.
 *
 * @tparam Source Candidate type, examined as a `const` object.
 */
template <typename Source>
concept TriangleSource = requires(const Source& source, std::size_t triangle, std::array<std::uint32_t, 3>& corners) {
    { source.positions() } -> std::convertible_to<std::span<const vn::math::Vec3f>>;
    { source.triangleCount() } -> std::convertible_to<std::size_t>;
    { source.triangleCorners(triangle, corners) } -> std::convertible_to<bool>;
};

/**
 * @brief Registers one primitive per triangle of @p source on @p tree, in triangle order.
 *
 * @details
 * - This is THE numbering a triangle tree uses: primitive id == triangle ordinal, so a hit reported by a query is a
 *   triangle without a second lookup, and a build and a restore cannot name different triangles.
 * - A primitive's volume is the box of its three corners widened to double. The widening is exact, so the tree states the
 *   triangles' bounds without rounding them; the tree being `double` while the vertices are `float` is what lets a small
 *   leaf stay accurate far from the origin.
 * - All or nothing at the CALL level: a triangle whose corners do not resolve, or a corner holding a non-finite
 *   coordinate, stops the registration and reports false. Refusing here is the difference between "no tree, walk the
 *   triangles linearly" and an exception thrown out of a pick, so it is a return value and not a `Bvh::add()` throw.
 * - A refusal may leave the triangles before it registered: `tree` is the caller's to drop, and every caller in this
 *   library fills a local tree and publishes it only when the call succeeded (`G10`: no second pass over the triangles
 *   to make a failure tidy).
 * - Registration is DATA, not construction: the boxes are stored and the tree is left unbuilt; `buildTriangleBvh()` is
 *   the call that constructs it.
 *
 * @tparam Source Triangle source type; see @ref TriangleSource.
 * @param tree   Receives the primitives; it must be empty, because the ids it hands out start at 0.
 * @param source The triangles to register.
 * @return true when every triangle was registered.
 * @throws std::length_error when @p source holds more triangles than a `Bvh::PrimId` can index.
 */
template <TriangleSource Source>
[[nodiscard]] bool registerTriangles(Bvh<double>& tree, const Source& source)
{
    const std::size_t                     triangle_count = source.triangleCount();
    const std::span<const vn::math::Vec3f> positions      = source.positions();
    if (triangle_count == 0u || positions.empty() || !tree.empty()) {
        return false; // nothing to index, no vertices to index it with, or a tree already numbered from 0
    }

    tree.reserve(triangle_count);
    std::array<std::uint32_t, 3>             corners{};
    std::array<vn::math::Vector3<double>, 3> points{};
    for (std::size_t triangle = 0; triangle < triangle_count; ++triangle) {
        if (!source.triangleCorners(triangle, corners)) {
            return false; // a corner that names no stored vertex: a tree here would describe something else
        }
        for (std::size_t corner = 0; corner < 3u; ++corner) {
            if (corners[corner] >= positions.size()) {
                return false;
            }
            const vn::math::Vec3f& position = positions[corners[corner]];
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                return false;
            }
            points[corner] = vn::math::Vector3<double>(
                static_cast<double>(position.x), static_cast<double>(position.y), static_cast<double>(position.z));
        }
        tree.add(Aabbd::compute(points));
    }
    return true;
}

/**
 * @brief Builds @p tree over @p source's triangles, or leaves it unbuilt.
 *
 * @details
 * - The whole build in one call: register the triangles (the numbering above), then let @p builder decide the shape. It
 *   is what a model's `buildBvh()` and a draw's `prepareBvh()` both do, so a caller that owns a triangle set has one call
 *   to make rather than a loop to re-derive.
 * - @p builder is a value, so a caller that cares about leaf size or algorithm keeps that choice here.
 *
 * @tparam Source Triangle source type; see @ref TriangleSource.
 * @param tree    Receives the primitives and the tree; it must be empty.
 * @param source  The triangles to index.
 * @param builder Build parameters; the default is the library's own choice.
 * @return true when @p tree is built afterwards.
 */
template <TriangleSource Source>
[[nodiscard]] bool buildTriangleBvh(Bvh<double>& tree, const Source& source, const BvhBuilder& builder = {})
{
    if (!registerTriangles(tree, source)) {
        return false;
    }
    builder.build(tree);
    return tree.isBuilt();
}

/**
 * @brief Rebuilds @p tree's boxes from @p source's current triangles, in a STORED shape.
 *
 * @details
 * - The restore path of the triangle-tree contract, in one call: register the triangles exactly as the build does, then
 *   install @p shape through `RestoreTopology` - which re-derives every box from the primitives it was just given.
 * - A shape that does not describe THIS source (an order that is not a permutation of its triangles, a leaf outside it,
 *   an unreachable node, a height that is not the array's own) leaves @p tree unbuilt. A caller treats that as "no cache"
 *   rather than as damage: the answer for a stale or foreign shape is the triangles, slower, never wrong.
 * - The installed tree is `validate()`d once here, because a shape is data from outside this process: what the build
 *   path guarantees by construction is checked rather than assumed. The check is linear and runs once per restore.
 *
 * @tparam Source Triangle source type; see @ref TriangleSource.
 * @param tree   Receives the primitives and the tree; it must be empty.
 * @param source The triangles the shape claims to describe.
 * @param shape  The stored shape: nodes, primitive order and height, all from one tree.
 * @return true when @p tree holds a built tree that describes @p source.
 */
template <TriangleSource Source>
[[nodiscard]] bool restoreTriangleBvh(Bvh<double>& tree, const Source& source, BvhTopologyView shape)
{
    if (!registerTriangles(tree, source)) {
        return false;
    }
    BvhBuilder{}.buildWith(tree, RestoreTopology{ shape });
    return tree.isBuilt() && tree.validate();
}

VN_GEOM_SPATIAL_NS_END
