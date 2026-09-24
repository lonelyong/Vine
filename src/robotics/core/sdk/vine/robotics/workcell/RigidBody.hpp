#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vector>

#include "Collision.hpp"
#include "Visual.hpp"

VN_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief A rigid body: visuals and collision shapes.
 *
 * A pure-data value type that owns no coordinate system of its own: all its
 * geometry is expressed relative to the base coordinate frame of the object
 * that embeds it (a Link's base frame, or a SceneObject's base frame). It
 * knows nothing about motion, scene management or ownership; it is embedded
 * by value wherever geometry lives (Link, RigidObject, ...).
 */
class RigidBody
{
  public:
    /**
     * @brief Returns the visual geometries.
     *
     * @return The visuals.
     */
    std::vector<Visual>& visuals()
    {
        return visuals_;
    }

    /**
     * @brief Returns the visual geometries.
     *
     * @return The visuals.
     */
    const std::vector<Visual>& visuals() const
    {
        return visuals_;
    }

    /**
     * @brief Sets the visual geometries.
     *
     * @param visuals New visuals.
     */
    void setVisuals(const std::vector<Visual>& visuals)
    {
        visuals_ = visuals;
    }

    /**
     * @brief Returns the collision shapes.
     *
     * @return The collisions.
     */
    std::vector<Collision>& collisions()
    {
        return collisions_;
    }

    /**
     * @brief Returns the collision shapes.
     *
     * @return The collisions.
     */
    const std::vector<Collision>& collisions() const
    {
        return collisions_;
    }

    /**
     * @brief Sets the collision shapes.
     *
     * @param collisions New collisions.
     */
    void setCollisions(const std::vector<Collision>& collisions)
    {
        collisions_ = collisions;
    }

  private:
    std::vector<Visual>    visuals_;
    std::vector<Collision> collisions_;
};

VN_ROBOTICS_WORKCELL_NS_END
