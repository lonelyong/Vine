#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <map>

#include <vine/geometry/Shape.hpp>
#include <vine/intrusive_ptr.hpp>

#include "CollisionGeometry.hpp"
#include "CollisionObject.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief Factory and registry of collision geometries and objects.
 *
 * A CollisionGeometry is immutable and pose-free, so it is created once per
 * Shape and reused; a CollisionObject is a pose-bearing instance and is never
 * shared. The registered Shape must outlive its cached geometry.
 *
 * @warning Not thread-safe.
 */
class CollisionGeometryManager {
  protected:
    /**
     * @brief Constructs a collision geometry manager.
     */
    CollisionGeometryManager() = default;

  public:
    /**
     * @brief Destroys the collision geometry manager.
     */
    virtual ~CollisionGeometryManager() = default;

    /**
     * @brief Ensures a collision geometry exists for a shape.
     *
     * @param shape The source shape.
     * @return true when a valid geometry is available (new or cached).
     */
    bool add(const vn::intrusive_ptr<const vn::geometry::Shape>& shape)
    {
        if (shape == nullptr) {
            return false;
        }
        if (shape_geometry_map_.contains(shape.get())) {
            return true;
        }
        if (auto geometry = createCollisionGeometry(shape)) {
            if (geometry->isValid()) {
                shape_geometry_map_[shape.get()] = std::move(geometry);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Returns the cached geometry of a shape.
     *
     * @param shape The source shape.
     * @return The geometry, or null when not registered.
     */
    vn::intrusive_ptr<CollisionGeometry> get(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) const
    {
        if (shape == nullptr) {
            return {};
        }
        const auto it = shape_geometry_map_.find(shape.get());
        return it != shape_geometry_map_.end() ? it->second : vn::intrusive_ptr<CollisionGeometry>{};
    }

    /**
     * @brief Removes the geometry registered for a shape.
     *
     * @param shape The source shape.
     * @return true when a geometry was removed.
     */
    bool remove(const vn::intrusive_ptr<const vn::geometry::Shape>& shape)
    {
        if (shape == nullptr) {
            return false;
        }
        return shape_geometry_map_.erase(shape.get()) > 0;
    }

    /**
     * @brief Rebuilds the geometry of a shape (e.g. after mesh edits).
     *
     * @param shape The source shape.
     * @return true on success, false when the shape was not registered or the
     *         rebuild failed.
     */
    bool update(const vn::intrusive_ptr<const vn::geometry::Shape>& shape)
    {
        if (shape == nullptr || !shape_geometry_map_.contains(shape.get())) {
            return false;
        }
        if (auto geometry = createCollisionGeometry(shape)) {
            if (geometry->isValid()) {
                shape_geometry_map_[shape.get()] = std::move(geometry);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Removes all managed geometries.
     */
    void clear()
    {
        shape_geometry_map_.clear();
    }

    /**
     * @brief Creates a collision object whose geometry is the shape's geometry.
     *
     * @param shape The shape; must already be registered.
     * @return The new collision object, or null when the shape is unregistered.
     */
    virtual vn::intrusive_ptr<CollisionObject>
        createCollisionObject(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) const = 0;

  protected:
    /**
     * @brief Creates and builds a collision geometry from a shape.
     *
     * @param shape The source shape.
     * @return The new geometry, or null when the shape is unsupported.
     */
    virtual vn::intrusive_ptr<CollisionGeometry>
        createCollisionGeometry(const vn::intrusive_ptr<const vn::geometry::Shape>& shape) const = 0;

  protected:
    /// Registered shapes to their cached geometries.
    std::map<const vn::geometry::Shape*, vn::intrusive_ptr<CollisionGeometry>> shape_geometry_map_;
};

VN_ROBOTICS_PROXIMITY_NS_END
