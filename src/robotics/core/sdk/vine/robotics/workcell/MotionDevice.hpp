#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/kinematics/SerialKinematics.hpp>

#include "Device.hpp"

V_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief Motion device definition data (immutable): extends DeviceData.
 *
 * Currently a marker type (motion-specific metadata such as the default IK
 * solver belongs to the not-yet-modeled metadata); it exists so a device can
 * downcast its definition data and so derived clones keep the concrete type.
 */
struct V_ROBOTICS_CORE_API MotionDeviceData : DeviceData
{
    /**
     * @brief Creates a deep copy as a MotionDeviceData.
     *
     * @return The cloned definition data.
     */
    std::unique_ptr<DeviceData> clone() const override;
};

/**
 * @brief A device with actuated kinematics (robot, external axis, ...).
 *
 * A MotionDevice is a Device whose joint values are read from a scene State
 * passed in from outside (see Device::getQ / Device::setQ). It owns a
 * serial-chain kinematics model (SerialKinematics) built at init() time from
 * the device's base frame to its first end frame, and caches device-level
 * data derived from it: the home configuration and the joint lower/upper /
 * velocity/acceleration bounds. Currently every motion device is treated as a
 * single serial chain.
 */
class V_ROBOTICS_CORE_API MotionDevice : public Device
{
  public:
    /**
     * @brief Constructs an empty motion device; configure it with init().
     */
    MotionDevice();

    /**
     * @brief Constructs a motion device with the given name.
     *
     * @param name The device name.
     */
    explicit MotionDevice(const String& name);

    /**
     * @brief Destroys the device.
     */
    ~MotionDevice() override;

  public:
    /**
     * @brief Configures the device from definition data (takes ownership).
     *
     * In addition to Device::init, derives the home configuration and the
     * joint bounds from the joint definitions.
     *
     * @param data The device definition data.
     */
    void init(std::unique_ptr<DeviceData> data) override;

    /**
     * @brief Returns the home configuration.
     *
     * Defaults to the joint value closest to zero that lies within the lower
     * / upper bounds; empty when the device is invalid or not initialized.
     *
     * @return The home joint values.
     */
    const kinematics::Q& homeQ() const
    {
        return home_q_;
    }

    /**
     * @brief Sets the home configuration.
     *
     * @param q The home joint values.
     */
    void setHomeQ(const kinematics::Q& q)
    {
        home_q_ = q;
    }

    /**
     * @brief Sets the home configuration (move overload).
     *
     * @param q The home joint values.
     */
    void setHomeQ(kinematics::Q&& q)
    {
        home_q_ = std::move(q);
    }

    /**
     * @brief Returns the raw joint lower bounds, one value per dof.
     *
     * @return The lower bounds, empty when the device is invalid.
     */
    const kinematics::Q& lowerBounds() const
    {
        return lower_bounds_;
    }

    /**
     * @brief Returns the raw joint upper bounds, one value per dof.
     *
     * @return The upper bounds, empty when the device is invalid.
     */
    const kinematics::Q& upperBounds() const
    {
        return upper_bounds_;
    }

    /**
     * @brief Returns the joint velocity limits, one value per dof.
     *
     * @return The velocity limits, empty when the device is invalid.
     */
    const kinematics::Q& maxVelocityLimits() const
    {
        return velocity_limits_;
    }

    /**
     * @brief Returns the joint acceleration limits, one value per dof.
     *
     * @return The acceleration limits, empty when the device is invalid.
     */
    const kinematics::Q& maxAccelerationLimits() const
    {
        return acceleration_limits_;
    }

    /**
     * @brief Returns the serial-chain kinematics model.
     *
     * @return The kinematics, or null when the device is invalid.
     */
    raw_ptr<kinematics::SerialKinematics> kinematics()
    {
        return kinematics_.get();
    }

    /**
     * @brief Returns the serial-chain kinematics model.
     *
     * @return The kinematics, or null when the device is invalid.
     */
    raw_ptr<const kinematics::SerialKinematics> kinematics() const
    {
        return kinematics_.get();
    }

  protected:
    /**
     * @brief Casts/wraps definition data into MotionDeviceData.
     *
     * @param data The definition data.
     * @return The motion definition data.
     */
    static std::unique_ptr<MotionDeviceData> asMotionData(std::unique_ptr<DeviceData> data);

    /**
     * @brief Builds the base device and derives the motion-specific data.
     *
     * @param data The motion definition data.
     */
    void initMotion(std::unique_ptr<MotionDeviceData> data);

  private:
    std::unique_ptr<kinematics::SerialKinematics> kinematics_;
    kinematics::Q                                 home_q_;
    kinematics::Q                     lower_bounds_;
    kinematics::Q                     upper_bounds_;
    kinematics::Q                     velocity_limits_;
    kinematics::Q                     acceleration_limits_;
};

V_ROBOTICS_WORKCELL_NS_END
