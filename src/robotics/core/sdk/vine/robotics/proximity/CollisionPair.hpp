#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <functional>
#include <utility>

#include <vine/INameable.hpp>

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief An unordered pair of owners.
 *
 * Two pairs are equal regardless of the order of the two owners, so a pair
 * can be used as a key in the collision matrix.
 */
struct CollisionPair {
    /// The first owner.
    const vn::INamed* object1{ nullptr };
    /// The second owner.
    const vn::INamed* object2{ nullptr };

    /**
     * @brief Compares two pairs ignoring the owner order.
     *
     * @param other The other pair.
     * @return true when the pairs hold the same two owners.
     */
    bool operator==(const CollisionPair& other) const
    {
        return (object1 == other.object1 && object2 == other.object2)
               || (object1 == other.object2 && object2 == other.object1);
    }

    /**
     * @brief Checks whether two pairs differ.
     *
     * @param other The other pair.
     * @return true when the pairs differ.
     */
    bool operator!=(const CollisionPair& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief Hasher for CollisionPair, usable in unordered containers.
 */
struct CollisionPairHasher {
    /**
     * @brief Hashes an unordered pair deterministically.
     *
     * @param pair The pair.
     * @return The hash value.
     */
    std::size_t operator()(const CollisionPair& pair) const noexcept
    {
        const void* a = pair.object1;
        const void* b = pair.object2;
        if (std::less<const void*>{}(b, a)) {
            std::swap(a, b);
        }
        const std::size_t h1 = std::hash<const void*>{}(a);
        const std::size_t h2 = std::hash<const void*>{}(b);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

VN_ROBOTICS_PROXIMITY_NS_END
