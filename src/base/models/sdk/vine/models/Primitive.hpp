#pragma once

#include <vine/models/models_global.hpp>

#include <vine/geometry/Aabb.hpp>

#include <vine/models/Shape.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief A modelling shape: identity plus whatever describes it.
 *
 * @details
 * - The modelling side is `Shape`: a name, a revision and the tag the family dispatches on. A primitive is **not required
 *   to be an analytic solid**, so this base holds no geometry at all: each subclass states its own data and answers
 *   `isValid()`, `hasVolume()` and `bounds()` from it.
 * - `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` are the five subclasses this repository models. They are solids,
 *   so each one also answers `solid()` with its concrete analytic kind (`vn::geometry::BoxSolidf`, …), which is what the geometry
 *   algorithms take. A primitive that is not a solid derives from this base without touching `Solid` at all.
 * - The tag is handed to the base by the subclass constructor, so `shapeType()` cannot name a different kind than the
 *   class that was written.
 * - The canonical local space belongs to the subclass: box, sphere and ellipsoid are centred on the origin, and cylinder
 *   and cone span `[-height / 2, +height / 2]` on +Z with the cone's base at the lower end. Placement belongs to the node
 *   transform that holds the shape, never here.
 *
 * @warning
 * - A dimension at or below 0 makes a shape unusable: `isValid()` reports it and the queries answer nothing.
 *   `bounds()` stays a well formed box either way.
 *
 * @note
 * - `bounds()` is pure virtual here because every primitive can answer it; `solid()` is not, because being a solid is a
 *   property of those five subclasses rather than of the concept.
 */
class VN_MODELS_API Primitive : public Shape {
    VN_OBJECT_META_DECL;

  protected:
    /**
     * @brief Constructs a primitive that reports @p type.
     *
     * @param type The concrete tag; every subclass passes its own and nothing else.
     */
    explicit Primitive(ShapeType type) noexcept;

  public:
    /**
     * @brief Returns the axis-aligned bounds of this primitive in its own local space.
     *
     * @return The bounds; multiply by the node transform for a world box.
     */
    [[nodiscard]] virtual vn::geometry::Aabbf bounds() const = 0;
};

VN_MODELS_NS_END
