#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <map>
#include <vector>

#include <vine/INameable.hpp>
#include <vine/intrusive_ptr.hpp>

#include "CollisionMatrix.hpp"
#include "CollisionObject.hpp"
#include "CollisionRequest.hpp"
#include "CollisionResult.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief Base class of a collision detector.
 *
 * The base implements the owner bookkeeping (registration, batching and pose
 * update); the actual broadphase / narrowphase queries are delegated to the
 * do* hooks implemented by a backend (e.g. an FCL detector).
 *
 * @warning Not thread-safe: registration, removal and pose updates must not
 *          run concurrently with each other. checkCollision() may run
 *          concurrently with other checkCollision() calls, but not with
 *          registration, removal or pose updates.
 * @warning Does not add references to registered owners or frames; the caller
 *          must keep them alive for the whole lifetime of the registration.
 */
class CollisionDetector {
  protected:
    /**
     * @brief Constructs a collision detector.
     */
    CollisionDetector() = default;

  public:
    /**
     * @brief Destroys the collision detector.
     */
    virtual ~CollisionDetector() = default;

    /**
     * @brief Runs a query over all registered objects.
     *
     * The world poses must already be up to date (e.g. via
     * updateObjectTransform()).
     *
     * @param request The query options.
     * @return The query result.
     */
    CollisionResult checkCollision(const CollisionRequest& request) const
    {
        return doCheckCollision(request);
    }

    /**
     * @brief Evaluates the registered objects from a state, then runs a query.
     *
     * @param request The query options.
     * @param state The scene state.
     * @return The query result.
     */
    CollisionResult checkCollision(const CollisionRequest& request, const kinematics::State& state) const
    {
        return doCheckCollision(request, state);
    }

    /**
     * @brief Runs a query between this detector's objects and another's.
     *
     * Objects inside a single detector are not checked against each other.
     *
     * @param other The other detector.
     * @param request The query options.
     * @param state The scene state.
     * @return The query result.
     */
    CollisionResult checkCollision(const CollisionDetector& other, const CollisionRequest& request,
                                   const kinematics::State& state) const
    {
        return doCheckCollision(other, request, state);
    }

    /**
     * @brief Opens a batch of registration / removal / pose changes.
     *
     * Every beginUpdate() increments an internal counter, every endUpdate()
     * decrements it; the internal structures are rebuilt when the counter
     * reaches zero. beginUpdate() and endUpdate() must be paired.
     */
    void beginUpdate()
    {
        ++update_depth_;
    }

    /**
     * @brief Registers an owner and its collision bodies.
     *
     * @param object The owner; must not be null.
     * @param objects The owner's collision bodies; references are added.
     * @warning No change is made when the owner is already registered.
     */
    void addObject(const vn::INamed* object, const std::vector<vn::intrusive_ptr<CollisionObject>>& objects)
    {
        if (object == nullptr || object_objects_map_.contains(object)) {
            return;
        }
        std::vector<vn::intrusive_ptr<CollisionObject>> accepted;
        accepted.reserve(objects.size());
        for (const auto& cobj : objects) {
            if (cobj != nullptr && doAddCollisionObject(cobj.get())) {
                accepted.push_back(cobj);
            }
        }
        if (!accepted.empty()) {
            object_objects_map_[object] = std::move(accepted);
        }
    }

    /**
     * @brief Removes an owner and all its collision bodies.
     *
     * @param object The owner.
     * @note No change is made when the owner is not registered.
     */
    void removeObject(const vn::INamed* object)
    {
        const auto it = object_objects_map_.find(object);
        if (it == object_objects_map_.end()) {
            return;
        }
        for (const auto& cobj : it->second) {
            doRemoveCollisionObject(cobj.get());
        }
        object_objects_map_.erase(it);
    }

    /**
     * @brief Updates the world poses of an owner's collision bodies from a state.
     *
     * @param object The owner.
     * @param state The scene state.
     */
    void updateObjectTransform(const vn::INamed* object, const kinematics::State& state)
    {
        const auto it = object_objects_map_.find(object);
        if (it == object_objects_map_.end()) {
            return;
        }
        for (const auto& cobj : it->second) {
            cobj->computeWorldTransform(state);
        }
    }

    /**
     * @brief Removes all registered owners and their collision bodies.
     *
     * Does not destroy the owners or the collision objects, only unregisters
     * them from the detector.
     */
    void clear()
    {
        for (const auto& [object, cobjs] : object_objects_map_) {
            (void)object;
            for (const auto& cobj : cobjs) {
                doRemoveCollisionObject(cobj.get());
            }
        }
        object_objects_map_.clear();
    }

    /**
     * @brief Closes a batch of registration / removal / pose changes.
     */
    void endUpdate()
    {
        if (update_depth_ > 0) {
            --update_depth_;
        }
        if (update_depth_ == 0) {
            doEndUpdate();
        }
    }

    /**
     * @brief Rebuilds the engine-side broadphase from the current objects.
     *
     * Required after a registered CollisionObject's geometry was replaced.
     *
     * @return true on success, false otherwise.
     */
    bool rebuild()
    {
        return doRebuild();
    }

  protected:
    /**
     * @brief Adds one collision body to the engine-side structures.
     *
     * @param object The collision body.
     * @return true when the body was accepted.
     */
    virtual bool doAddCollisionObject(CollisionObject* object) = 0;

    /**
     * @brief Removes one collision body from the engine-side structures.
     *
     * @param object The collision body.
     * @return true when the body was removed.
     */
    virtual bool doRemoveCollisionObject(CollisionObject* object) = 0;

    /**
     * @brief Rebuilds the engine-side broadphase.
     *
     * @return true on success, false otherwise.
     */
    virtual bool doRebuild() = 0;

    /**
     * @brief Flushes a finished batch update.
     */
    virtual void doEndUpdate() = 0;

    /**
     * @brief Runs the engine-side query over the registered bodies.
     *
     * @param request The query options.
     * @return The query result.
     */
    virtual CollisionResult doCheckCollision(const CollisionRequest& request) const = 0;

    /**
     * @brief Evaluates the registered bodies from a state, then queries them.
     *
     * @param request The query options.
     * @param state The scene state.
     * @return The query result.
     */
    virtual CollisionResult doCheckCollision(const CollisionRequest& request,
                                             const kinematics::State& state) const = 0;

    /**
     * @brief Runs the engine-side query against another detector's bodies.
     *
     * @param other The other detector.
     * @param request The query options.
     * @param state The scene state.
     * @return The query result.
     */
    virtual CollisionResult doCheckCollision(const CollisionDetector& other, const CollisionRequest& request,
                                             const kinematics::State& state) const = 0;

  protected:
    /// beginUpdate() nesting depth; structures are rebuilt when it reaches zero.
    std::size_t update_depth_{ 0 };
    /// Registered owners and their collision bodies.
    std::map<const vn::INamed*, std::vector<vn::intrusive_ptr<CollisionObject>>> object_objects_map_;
};

VN_ROBOTICS_PROXIMITY_NS_END
