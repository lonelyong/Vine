#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/geometry/Shape.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/RefCounted.hpp>

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief Engine-side collision shape built from a geometry Shape.
 *
 * A collision geometry is the backend representation of a Shape (e.g. an FCL
 * collision geometry). It carries no pose and is immutable, so it can be
 * shared by many CollisionObject instances. The concrete backend decides how
 * a Shape is converted; this base only defines the contract.
 *
 * @note Reference counted through vn::RefCounted; owned by
 *       intrusive_ptr<CollisionGeometry>.
 */
class CollisionGeometry : public vn::RefCounted<CollisionGeometry> {
  protected:
    /**
     * @brief Constructs an empty collision geometry.
     */
    CollisionGeometry() = default;

  public:
    /**
     * @brief Destroys the collision geometry.
     */
    virtual ~CollisionGeometry() = default;

    /**
     * @brief Builds the collision geometry from a shape.
     *
     * @param shape The source geometry shape.
     * @return true when the shape was accepted, false otherwise.
     */
    virtual bool buildFromShape(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) = 0;

    /**
     * @brief Checks whether the geometry holds valid, queryable data.
     *
     * @return true when the geometry is valid.
     */
    virtual bool isValid() const = 0;
};

VN_ROBOTICS_PROXIMITY_NS_END
