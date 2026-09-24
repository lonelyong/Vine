#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/String.hpp>

#include "RigidBody.hpp"
#include "SceneObject.hpp"

VN_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief A standalone rigid body placed in the workcell (table, part, fence,
 *        fixture, ...).
 *
 * A RigidObject is a scene-managed object that has no device semantics: no
 * joints, no degrees of freedom, no kinematics. Its geometry (visuals +
 * collisions + frames) is carried by an embedded RigidBody.
 */
class VN_ROBOTICS_CORE_API RigidObject : public SceneObject
{
  public:
    /**
     * @brief Constructs a rigid object with the given name.
     *
     * @param name The object name.
     */
    explicit RigidObject(const String& name);

    /**
     * @brief Destroys the rigid object.
     */
    ~RigidObject() override;

  public:
    /**
     * @brief Returns the object kind.
     *
     * @return SceneObjectKind::RigidObject.
     */
    SceneObjectKind kind() const override;

    /**
     * @brief Returns the rigid body (root frame + geometry).
     *
     * @return The rigid body.
     */
    RigidBody& body()
    {
        return body_;
    }

    /**
     * @brief Returns the rigid body (root frame + geometry).
     *
     * @return The rigid body.
     */
    const RigidBody& body() const
    {
        return body_;
    }

  private:
    RigidBody body_;
};

VN_ROBOTICS_WORKCELL_NS_END
