#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vine/INameable.hpp>

#include "CollisionPair.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief N×N collision matrix managing pairwise collision rules between owners.
 *
 * The matrix maintains three kinds of configuration per owner pair:
 * registration state, the minimum safe distance and the ignore flag. Owners
 * are identified by pointer; the matrix never owns them.
 */
class CollisionMatrix final {
  public:
    /**
     * @brief Pairwise collision options.
     */
    struct CollisionOptions {
        /// Whether the pair is excluded from collision checks.
        bool ignored{ false };
        /// Minimum safe distance; a smaller distance counts as a collision.
        double min_dist{ 0.0 };
    };

  public:
    /**
     * @brief Returns the default options of a pair.
     *
     * @return The default options.
     */
    static const CollisionOptions& defaultCollisionOptions()
    {
        static const CollisionOptions options;
        return options;
    }

    /**
     * @brief Registers an owner so it can participate in collision checks.
     *
     * @param object The owner; must not be null.
     * @throws std::logic_error when the owner is null or already registered.
     */
    void registerObject(const vn::INamed* object)
    {
        if (object == nullptr) {
            throw std::logic_error("CollisionMatrix::registerObject, null object.");
        }
        if (!registered_objects_.insert(object).second) {
            throw std::logic_error("CollisionMatrix::registerObject, object already registered: "
                                   + object->name().as_std_str());
        }
    }

    /**
     * @brief Removes an owner and all its pairwise rules.
     *
     * @param object The owner.
     */
    void unregisterObject(const vn::INamed* object)
    {
        registered_objects_.erase(object);
        for (auto it = collision_options_.begin(); it != collision_options_.end();) {
            if (it->first.object1 == object || it->first.object2 == object) {
                it = collision_options_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    /**
     * @brief Checks whether an owner is registered.
     *
     * @param object The owner.
     * @return true when registered.
     */
    bool containsObject(const vn::INamed* object) const
    {
        return registered_objects_.contains(object);
    }

    /**
     * @brief Returns all registered owners, sorted by name.
     *
     * @return The registered owners.
     */
    std::vector<const vn::INamed*> registeredObjects() const
    {
        std::vector<const vn::INamed*> objects(registered_objects_.begin(), registered_objects_.end());
        std::sort(objects.begin(), objects.end(), [](const vn::INamed* a, const vn::INamed* b) {
            return a->name() < b->name();
        });
        return objects;
    }

    /**
     * @brief Sets the minimum safe distance between two owners.
     *
     * @param object_a Owner A.
     * @param object_b Owner B; must differ from A.
     * @param distance The safe distance; a negative value is stored as its
     *        absolute value.
     * @throws std::logic_error when either owner is unregistered or a == b.
     */
    void setMinDistance(const vn::INamed* object_a, const vn::INamed* object_b, double distance)
    {
        validatePair(object_a, object_b);
        options(object_a, object_b).min_dist = std::abs(distance);
    }

    /**
     * @brief Returns the minimum safe distance between two owners.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return >= 0 the configured distance; -1.0 when a == b; -2.0 when
     *         either owner is unregistered.
     */
    double minDistance(const vn::INamed* object_a, const vn::INamed* object_b) const
    {
        if (object_a == object_b) {
            return -1.0;
        }
        if (!containsObject(object_a) || !containsObject(object_b)) {
            return -2.0;
        }
        return findOptions(object_a, object_b).min_dist;
    }

    /**
     * @brief Sets whether a pair of owners is excluded from collision checks.
     *
     * @param object_a Owner A.
     * @param object_b Owner B; must differ from A.
     * @param ignore Whether to exclude the pair.
     * @throws std::logic_error when either owner is unregistered or a == b.
     */
    void setIgnored(const vn::INamed* object_a, const vn::INamed* object_b, bool ignore)
    {
        validatePair(object_a, object_b);
        options(object_a, object_b).ignored = ignore;
    }

    /**
     * @brief Checks whether a pair of owners is excluded from collision checks.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return true when a == b; false when either owner is unregistered;
     *         otherwise the configured ignore flag.
     */
    bool isIgnored(const vn::INamed* object_a, const vn::INamed* object_b) const
    {
        if (object_a == object_b) {
            return true;
        }
        if (!containsObject(object_a) || !containsObject(object_b)) {
            return false;
        }
        return findOptions(object_a, object_b).ignored;
    }

    /**
     * @brief Returns the configured options of a pair.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return The options, or std::nullopt when either owner is unregistered.
     */
    std::optional<CollisionOptions> collisionOptions(const vn::INamed* object_a,
                                                     const vn::INamed* object_b) const
    {
        if (!containsObject(object_a) || !containsObject(object_b)) {
            return std::nullopt;
        }
        return findOptions(object_a, object_b);
    }

    /**
     * @brief Copies the pairwise rules of another matrix for shared owners.
     *
     * A rule is copied only when both owners are registered in this matrix.
     *
     * @param other The source matrix.
     */
    void copyOptionsFrom(const CollisionMatrix& other)
    {
        for (const auto& [pair, options] : other.collision_options_) {
            if (containsObject(pair.object1) && containsObject(pair.object2)) {
                collision_options_.insert_or_assign(pair, options);
            }
        }
    }

    /**
     * @brief Excludes one owner from colliding with every other registered owner.
     *
     * @param object The owner; must be registered.
     * @throws std::logic_error when the owner is unregistered.
     */
    void ignoreAgainstAll(const vn::INamed* object)
    {
        if (!containsObject(object)) {
            throw std::logic_error("CollisionMatrix::ignoreAgainstAll, object not registered: "
                                   + (object ? object->name().as_std_str() : "null"));
        }
        for (const auto* const other : registered_objects_) {
            if (other != object) {
                setIgnored(object, other, true);
            }
        }
    }

    /**
     * @brief Decides whether a pair of owners should be checked.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return true when a != b and the pair is not ignored.
     */
    bool shouldCheckCollision(const vn::INamed* object_a, const vn::INamed* object_b) const
    {
        return object_a != object_b && !isIgnored(object_a, object_b);
    }

  private:
    /**
     * @brief Validates that a pair is registered and distinct.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @throws std::logic_error on invalid input.
     */
    void validatePair(const vn::INamed* object_a, const vn::INamed* object_b) const
    {
        if (object_a == object_b) {
            throw std::logic_error("CollisionMatrix, a == b is not a valid pair.");
        }
        if (!containsObject(object_a) || !containsObject(object_b)) {
            throw std::logic_error("CollisionMatrix, object not registered: "
                                   + (object_a ? object_a->name().as_std_str() : "null") + " / "
                                   + (object_b ? object_b->name().as_std_str() : "null"));
        }
    }

    /**
     * @brief Returns the stored options of a pair, creating defaults when absent.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return The options.
     */
    CollisionOptions& options(const vn::INamed* object_a, const vn::INamed* object_b)
    {
        return collision_options_[CollisionPair{ object_a, object_b }];
    }

    /**
     * @brief Returns the stored options of a pair, or defaults when absent.
     *
     * @param object_a Owner A.
     * @param object_b Owner B.
     * @return The options.
     */
    const CollisionOptions& findOptions(const vn::INamed* object_a, const vn::INamed* object_b) const
    {
        const auto it = collision_options_.find(CollisionPair{ object_a, object_b });
        return it != collision_options_.end() ? it->second : defaultCollisionOptions();
    }

  private:
    /// Owners participating in collision checks.
    std::unordered_set<const vn::INamed*> registered_objects_;
    /// Pairwise collision rules.
    std::unordered_map<CollisionPair, CollisionOptions, CollisionPairHasher> collision_options_;
};

/**
 * @brief Compares two collision option structs.
 *
 * @param lhs The left options.
 * @param rhs The right options.
 * @return true when equal.
 */
inline bool operator==(const CollisionMatrix::CollisionOptions& lhs, const CollisionMatrix::CollisionOptions& rhs)
{
    return lhs.ignored == rhs.ignored && lhs.min_dist == rhs.min_dist;
}

/**
 * @brief Checks whether two collision option structs differ.
 *
 * @param lhs The left options.
 * @param rhs The right options.
 * @return true when different.
 */
inline bool operator!=(const CollisionMatrix::CollisionOptions& lhs, const CollisionMatrix::CollisionOptions& rhs)
{
    return !(lhs == rhs);
}

VN_ROBOTICS_PROXIMITY_NS_END
