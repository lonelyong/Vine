#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstdint>

#include <vine/math/Vector3.hpp>

#include "CollisionObject.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief A single contact point between two collision objects.
 */
struct CollisionContact {
    /// The first collision object.
    CollisionObject* object1{ nullptr };
    /// The second collision object.
    CollisionObject* object2{ nullptr };
    /// Primitive index on object1 (-1 when not applicable).
    std::intptr_t primitive1{ -1 };
    /// Primitive index on object2 (-1 when not applicable).
    std::intptr_t primitive2{ -1 };
    /// Contact position in world space.
    math::Vec3d position;
};

VN_ROBOTICS_PROXIMITY_NS_END
