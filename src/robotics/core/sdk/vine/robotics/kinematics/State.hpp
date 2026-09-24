#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <unordered_map>

#include <vine/raw_ptr.hpp>

#include "QState.hpp"

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Scene state: the joint-space state of every device in a scene.
 *
 * The scene itself is stateless; a State is passed in from outside and
 * carries one QState per device, keyed by the device's base frame. A device
 * reads and writes its joint values through the QState of its own base frame.
 */
class State {
  public:
    /**
     * @brief Constructs an empty scene state.
     */
    State() = default;

  public:
    void setup(const Frame* root_frame)
    {
        qstate_.setup(root_frame);
    }

    /**
     * @brief Returns the QState of the device rooted at root, creating it
     *        when absent.
     *
     * @param root The device's base frame.
     * @return The device's QState.
     */
    QState& qstate(const Frame* root)
    {
        return qstate_;
    }

    const QState& qstate(const Frame* root) const
    {
        return qstate_;
    }

    void copyFrom(const State& other)
    {
        qstate_.copyFrom(other.qstate_);
    }

  private:
    QState qstate_;
};

VN_ROBOTICS_KINEMATICS_NS_END
