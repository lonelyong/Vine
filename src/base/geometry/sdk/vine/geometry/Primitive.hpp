#pragma once

#include "geometry_global.hpp"

#include <vine/math/Math.hpp>
#include <vine/math/Types.hpp>

#include "Aabb.hpp"
#include "Shape.hpp"
#include "Solid.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A modeling shape: identity plus the analytic solid it is made of.
 *
 * @details
 * - The modeling side is `Shape`: a name, a revision and the tag the family dispatches on. The geometric side is one of
 *   the five analytic solids, stored **by value** as `SolidShapef` (see `Solid.hpp`). This class holds the two together
 *   and adds nothing of its own: no algorithm, no second copy of a radius.
 * - Two views reach the geometry, and they answer different questions:
 *   - `solid()` gives the **family interface** `const Solidf&` — ask it a question (`bounds()`, `closestPoint()`,
 *     `intersectWith()`) when the kind does not matter.
 *   - `shape()` gives the stored **variant** — use it where a per-kind answer is required (a mesh, a B-rep, an element
 *     in a description file), because a `std::visit` over it is exhaustive at compile time.
 * - `shapeType()` is derived from the stored solid, so the tag cannot drift from the geometry it describes.
 * - The canonical local space is the solid's: a box, sphere and ellipsoid are centred on the origin, and a cylinder /
 *   cone spans `[-height / 2, +height / 2]` on +Z with the cone's base at the lower end. Placement belongs to the node
 *   transform that holds the shape, never here.
 * - The solid is stored as float: that is the precision a robot description is read in and the precision a triangle mesh
 *   is drawn at. A consumer that needs to compute in `double` builds the `double` kind it needs (`Boxd`, `Sphered`, …);
 *   nothing is converted behind its back.
 *
 * @warning
 * - A dimension at or below 0 makes the shape unusable: `isValid()` reports it, and every query of `solid()` answers
 *   nothing. `bounds()` stays a well formed box either way.
 *
 * @note
 * - This is the only place a model and a geometry meet. Everything else either asks `solid()` a question, visits
 *   `shape()`, or treats the object as a `Shape`.
 * - The default constructor builds a unit box, matching the default of the analytic box it holds.
 */
class VN_GEOMETRY_API Primitive : public Shape {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Constructs a primitive holding a unit box.
     */
    Primitive();

    /**
     * @brief Constructs a primitive from an analytic solid.
     *
     * @param solid The geometry this shape is made of; a kind converts implicitly (`Primitive(Spheref{ 1.0f })`).
     */
    explicit Primitive(const SolidShapef& solid);

  public:
    /**
     * @brief Returns the family view of the solid this shape is made of.
     *
     * @details
     * - This is how a consumer asks a question without knowing the kind: the two shape predicates and the three queries
     *   are virtual on `Solid<float>`, so no `switch` and no `obj_cast` is involved.
     * - The reference follows the stored variant; `setSolid()` replaces its contents, so a reference taken before the
     *   call reports the new geometry.
     *
     * @return The family interface of the stored solid.
     */
    [[nodiscard]] const Solidf& solid() const noexcept;

    /**
     * @brief Returns the stored solid, for the consumers that must answer per kind.
     *
     * @details
     * - Triangulation, the B-rep bridge and the description writer each need a different answer per kind. A `std::visit`
     *   over this variant is exhaustive at compile time, which is why those consumers take this view rather than the
     *   interface above.
     *
     * @return The variant, by const reference.
     */
    [[nodiscard]] const SolidShapef& shape() const noexcept;

    /**
     * @brief Replaces the geometry this shape is made of.
     *
     * @details
     * - This is the **only** mutator: replacing the whole solid keeps `shapeType()` and every derived value consistent
     *   by construction, which per-dimension setters could not — a radius set to 0 leaves a sphere that is no longer one.
     * - The revision is not advanced here: the holder reports the change once it is complete (`Shape::bumpRevision`).
     *
     * @param solid The new solid, of any kind; a kind converts implicitly.
     */
    void setSolid(const SolidShapef& solid) noexcept;

    /**
     * @brief Returns whether the solid's dimensions can be queried.
     * @return `true` when every dimension of the held kind is positive.
     */
    [[nodiscard]] bool isValid() const override;

    /**
     * @brief Returns whether the held solid encloses a volume larger than @p eps in every characteristic size.
     *
     * @param eps Numerical tolerance a dimension must exceed.
     * @return `true` when the shape encloses a volume by that measure.
     */
    [[nodiscard]] bool hasVolume(double eps = static_cast<double>(vn::math::EPS<float>())) const override;

    /**
     * @brief Returns the axis-aligned bounds of the held solid, in its canonical local space.
     *
     * @details
     * - The box is the solid's own answer, so this class does not recompute it; a degenerate shape still answers a well
     *   formed box.
     *
     * @return The bounds; multiply by the node transform for a world box.
     */
    [[nodiscard]] Aabbf bounds() const noexcept;

  private:
    SolidShapef solid_;
};

VN_GEOMETRY_NS_END
