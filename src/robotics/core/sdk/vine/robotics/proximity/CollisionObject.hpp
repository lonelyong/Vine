#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/INameable.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/RefCounted.hpp>
#include <vine/robotics/kinematics/Frame.hpp>

#include "CollisionGeometry.hpp"

VN_ROBOTICS_PROXIMITY_NS_BEGIN

/**
 * @brief A collision body: geometry, local pose and owner/frame association.
 *
 * One CollisionObject can be attached to exactly one detector. It holds the
 * geometry's local pose in the owner/frame space and the optional kinematic
 * frame / owner used to compute its world pose and to group pairs in the
 * collision matrix. It is reference counted and owned by the detector.
 *
 * @note The owner and frame are non-owning; the caller must keep them alive.
 */
class CollisionObject : public vn::RefCounted<CollisionObject> {
  protected:
    /**
     * @brief Constructs a collision object.
     *
     * @param geometry The collision geometry.
     * @param local_transform The pose of the geometry in the owner/frame space.
     */
    explicit CollisionObject(const vn::intrusive_ptr<CollisionGeometry>& geometry,
                             const math::Isometry3d&                      local_transform = {})
      : geometry_(geometry)
      , local_transform_(local_transform)
    {
    }

  public:
    /**
     * @brief Destroys the collision object.
     */
    virtual ~CollisionObject() = default;

    /**
     * @brief Sets the owner used for collision-matrix lookups.
     *
     * @param object The owning object, or nullptr.
     */
    void setObject(const vn::INamed* object)
    {
        object_ = object;
    }

    /**
     * @brief Returns the owner used for collision-matrix lookups.
     *
     * @return The owner, or nullptr when not set.
     */
    const vn::INamed* object() const
    {
        return object_;
    }

    /**
     * @brief Sets the kinematic frame the body is attached to.
     *
     * @param frame The frame, or nullptr.
     */
    void setFrame(raw_ptr<const kinematics::Frame> frame)
    {
        frame_ = frame;
    }

    /**
     * @brief Returns the kinematic frame the body is attached to.
     *
     * @return The frame, or nullptr when not set.
     */
    raw_ptr<const kinematics::Frame> frame() const
    {
        return frame_;
    }

    /**
     * @brief Returns the local pose of the geometry.
     *
     * @return The pose in the owner/frame space.
     */
    const math::Isometry3d& localTransform() const
    {
        return local_transform_;
    }

    /**
     * @brief Sets the local pose of the geometry.
     *
     * @param transform The pose in the owner/frame space.
     */
    void setLocalTransform(const math::Isometry3d& transform)
    {
        local_transform_ = transform;
    }

    /**
     * @brief Returns the collision geometry.
     *
     * @return The geometry.
     */
    const vn::intrusive_ptr<CollisionGeometry>& geometry() const
    {
        return geometry_;
    }

    /**
     * @brief Checks whether the object is valid (valid geometry).
     *
     * @return true when valid.
     */
    virtual bool isValid() const = 0;

    /**
     * @brief Computes and applies the world pose of the body.
     *
     * The default implementation composes the frame world pose with the local
     * pose; a backend overrides it to also sync engine-side transforms.
     *
     * @param state The scene state used to evaluate the frame.
     */
    virtual void computeWorldTransform(const kinematics::State& state)
    {
        world_transform_ = frame_ ? kinematics::Frame::frameInWorld(frame_, state) * local_transform_
                                  : local_transform_;
    }

    /**
     * @brief Returns the world pose computed by the last computeWorldTransform().
     *
     * @return The world pose.
     */
    const math::Isometry3d& worldTransform() const
    {
        return world_transform_;
    }

  protected:
    /// Owner used for collision-matrix lookups; not owned.
    const vn::INamed* object_{ nullptr };
    /// Kinematic frame the body is attached to; not owned.
    raw_ptr<const kinematics::Frame> frame_{ nullptr };
    /// Pose of the geometry in the owner/frame space.
    math::Isometry3d local_transform_;
    /// World pose, updated by computeWorldTransform().
    math::Isometry3d world_transform_;
    /// The collision geometry (shared, immutable).
    vn::intrusive_ptr<CollisionGeometry> geometry_;
};

VN_ROBOTICS_PROXIMITY_NS_END
