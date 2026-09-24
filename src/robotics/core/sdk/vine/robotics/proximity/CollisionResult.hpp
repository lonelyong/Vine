#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <chrono>
#include <ostream>
#include <unordered_map>
#include <vector>

#include "CollisionContact.hpp"
#include "CollisionPair.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief Snapshot of one collision query.
 *
 * Contacts are grouped by the unordered owner pair that collided, so the
 * result maps each owner pair to the contacts between their collision bodies.
 */
struct CollisionResult {
    /// Whether the query completed.
    bool is_done{};
    /// Contacts grouped by owner pair.
    std::unordered_map<CollisionPair, std::vector<CollisionContact>, CollisionPairHasher> pairs;
    /// Time spent by the query.
    std::chrono::steady_clock::duration time_cost{};

    /**
     * @brief Returns whether any collision was detected.
     *
     * @return true when at least one pair has contacts.
     */
    bool hasCollision() const
    {
        return !pairs.empty();
    }

    /**
     * @brief Checks whether both results collide the same owner pairs.
     *
     * Contact counts and positions are ignored.
     *
     * @param other The other result.
     * @return true when the colliding pairs match.
     */
    bool hasSameCollisionPairsAs(const CollisionResult& other) const
    {
        if (pairs.size() != other.pairs.size()) {
            return false;
        }
        for (const auto& [pair, contacts] : pairs) {
            const auto it = other.pairs.find(pair);
            if (it == other.pairs.end() || it->second.empty() != contacts.empty()) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Prints the result to a stream.
     *
     * @param os The output stream.
     */
    void print(std::ostream& os) const
    {
        os << "CollisionResult{ done=" << is_done << " pairs=" << pairs.size() << " }\n";
        for (const auto& [pair, contacts] : pairs) {
            os << "  pair: " << (pair.object1 ? pair.object1->name().as_std_str() : "null") << " <-> "
               << (pair.object2 ? pair.object2->name().as_std_str() : "null") << " contacts=" << contacts.size() << "\n";
        }
    }
};

VN_ROBOTICS_PROXIMITY_NS_END
