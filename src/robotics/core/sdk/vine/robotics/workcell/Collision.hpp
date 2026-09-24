#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Isometry3.hpp>

#include <vine/geometry/Material.hpp>
#include <vine/geometry/Shape.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief Collision geometry: shape and transform only.
 *
 * Used by simulation/collision checking. Kept separate from Visual because
 * the collision shape is usually a simpler approximation (box/sphere) than
 * the rendered mesh; it carries no material.
 */
class Collision
{
  public:
    /**
     * @brief Returns the collision shape.
     *
     * @return The shape, or null when unset.
     */
    const vn::intrusive_ptr<vn::geometry::Shape>& shape() const
    {
        return shape_;
    }

    /**
     * @brief Sets the collision shape.
     *
     * @param shape New shape.
     */
    void setShape(const vn::intrusive_ptr<vn::geometry::Shape>& shape)
    {
        shape_ = shape;
    }

    /**
     * @brief Returns the local transform of this collision shape.
     *
     * @return The transform relative to the body root frame.
     */
    const math::Isometry3d& tf() const
    {
        return tf_;
    }

    /**
     * @brief Sets the local transform of this collision shape.
     *
     * @param tf New transform relative to the body root frame.
     */
    void setTf(const math::Isometry3d& tf)
    {
        tf_ = tf;
    }

  private:
    vn::intrusive_ptr<vn::geometry::Shape> shape_;
    math::Isometry3d                          tf_;
};

VN_ROBOTICS_WORKCELL_NS_END
