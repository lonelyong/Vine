#pragma once

#include <vine/robotics/robot_core_global.hpp>

VN_ROBOTICS_PROXIMITY_NS_BEGIN

class CollisionMatrix;

/**
 * @brief Options controlling one collision query.
 */
struct CollisionRequest {
    /// Whether to compute contact details (normals, penetration, positions).
    bool compute_contact_details{ true };
    /// Stop after the first contact is found.
    bool stop_at_first_contact{};
    /// Maximum number of contacts computed per object pair.
    unsigned int max_contacts_per_pair{ 1 };
    /// Whether to run the query in parallel.
    bool enable_parallel{};
    /// Optional collision matrix used to filter object pairs; null = check all.
    const CollisionMatrix* collision_matrix{};
};

VN_ROBOTICS_PROXIMITY_NS_END
