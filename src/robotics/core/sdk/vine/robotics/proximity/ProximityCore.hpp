#pragma once

#include <vine/robotics/robot_core_global.hpp>

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief Scalar type used throughout the proximity module.
 *
 * Kept as an alias so a backend can later switch the whole module to another
 * floating-point type without touching the API.
 */
using ProximityScalar = double;

VN_ROBOTICS_PROXIMITY_NS_END
