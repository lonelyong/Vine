#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/Frame.hpp>

#include "RigidBody.hpp"

VN_ROBOTICS_WORKCELL_NS_BEGIN

class Device;

/**
 * @brief A kinematic link of a Device.
 *
 * A link is a rigid body that participates in the device's kinematics. It is
 * owned by a Device through a smart pointer and is deliberately NOT a
 * SceneObject: the workcell manages device-level objects only, so per-link
 * objects never appear in the scene.
 *
 * A link owns no coordinate system of its own. Its base frame is the frame it
 * hangs from in the kinematic chain: the device base frame for the base link,
 * and the joint frame for every other link. All body geometry (visuals,
 * collisions, auxiliary frames) is expressed relative to that frame.
 */
class VN_ROBOTICS_CORE_API Link : public vn::Object, public vn::INameable
{
    VN_OBJECT_META(Link, vn::Object, vn::INameable);

  public:
    /**
     * @brief Constructs a link with the given name.
     *
     * @param name The link name.
     */
    explicit Link(const String& name);

    /**
     * @brief Destroys the link.
     */
    ~Link() override;

  public:
    /**
     * @brief Returns the link name.
     *
     * @return The name.
     */
    const String& name() const noexcept override;

    /**
     * @brief Sets the link name.
     *
     * @param name The new name.
     */
    void setName(const String& name) override;

    /**
     * @brief Returns the base coordinate frame of this link.
     *
     * This is the frame the link's body geometry is expressed relative to:
     * the device base frame for the base link, or the joint frame that
     * connects this link to its parent link.
     *
     * @return The base frame, or null before the link is attached.
     */
    raw_ptr<kinematics::Frame> baseFrame()
    {
        return parent_frame_;
    }

    /**
     * @brief Returns the base coordinate frame of this link.
     *
     * @return The base frame, or null before the link is attached.
     */
    raw_ptr<const kinematics::Frame> baseFrame() const
    {
        return parent_frame_;
    }

    /**
     * @brief Sets the base coordinate frame of this link.
     *
     * Called by Device::init while building the kinematic chain; the frame is
     * owned by the device and is not a link member.
     *
     * @param frame The new base frame.
     */
    void setParentFrame(raw_ptr<kinematics::Frame> frame)
    {
        parent_frame_ = frame;
    }

    /**
     * @brief Returns the link's rigid body (geometry).
     *
     * @return The rigid body.
     */
    RigidBody& body()
    {
        return body_;
    }

    /**
     * @brief Returns the link's rigid body (geometry).
     *
     * @return The rigid body.
     */
    const RigidBody& body() const
    {
        return body_;
    }

    /**
     * @brief Returns the device that owns this link.
     *
     * @return The device, or null when not attached to one.
     */
    raw_ptr<Device> device() const
    {
        return device_;
    }

    /**
     * @brief Sets the device that owns this link.
     *
     * @param dev The owning device, or null to detach.
     */
    void setDevice(raw_ptr<Device> dev)
    {
        device_ = dev;
    }

    /**
     * @brief Checks whether this link carries no geometry.
     *
     * A virtual link has neither visuals nor collision shapes; it only
     * participates in the kinematic chain.
     *
     * @return true when the link has no geometry.
     */
    bool isVirtual() const
    {
        return body_.visuals().empty() && body_.collisions().empty();
    }

    /**
     * @brief Copies the definition state (name + rigid body) from another link.
     *
     * Used by DeviceData deep-cloning. The parent frame and the owning device
     * are intentionally left null; the caller rebinds them.
     *
     * @param other The source link.
     */
    void copyFrom(const Link& other);

  private:
    String                 name_;
    raw_ptr<kinematics::Frame> parent_frame_{ nullptr };
    RigidBody              body_;
    raw_ptr<Device>        device_{ nullptr };
};

VN_ROBOTICS_WORKCELL_NS_END
