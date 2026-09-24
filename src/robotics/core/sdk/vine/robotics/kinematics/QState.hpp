#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <unordered_map>

#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/Q.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Joint-space state of a kinematic chain.
 *
 * Packs the joint values of every joint frame of a frame tree into one flat
 * Q; each joint is mapped to a contiguous sub-range through its offset, so
 * per-joint reads and writes are O(1) and copying the whole state is cheap.
 *
 * setup() rebuilds the state from a root frame and registers every frame
 * whose dofInfos() is non-empty, zero-filling its joint values.
 *
 * @note Not thread-safe; the caller owns any synchronization. Frames are
 *       referenced by pointer, so the frame tree must outlive the state.
 */
class VN_ROBOTICS_CORE_API QState
{
  public:
    QState() = default;
    QState(const QState&) = default;
    QState(QState&&) noexcept = default;
    QState& operator=(const QState&) = default;
    QState& operator=(QState&&) noexcept = default;

  public:
    /**
     * @brief Rebuilds the state from a root frame.
     *
     * Registers every frame with a non-zero dof and initializes its joint
     * values to zero.
     *
     * @param root The root frame of the kinematic tree.
     */
    void setup(const Frame* root);

    /**
     * @brief Returns the joint values of a frame.
     *
     * @param joint The joint frame.
     * @return The joint sub-vector, or an empty Q when the frame is not a
     *         registered joint (e.g. a fixed frame).
     */
    Q getQ(const Frame* joint) const;

    /**
     * @brief Sets the joint values of a frame.
     *
     * @param joint The joint frame.
     * @param q The joint values; its size must match the joint dof.
     * @throws std::invalid_argument when the frame is not a registered joint
     *         or q.size() differs from the joint dof.
     */
    void setQ(const Frame* joint, const Q& q);

    /**
     * @brief Copies the values of the joints shared with another state.
     *
     * A joint is shared when both states reference the same frame object.
     *
     * @param other The source state.
     */
    void copyFrom(const QState& other);

    /**
     * @brief Returns the total number of joint values.
     *
     * @return The degrees of freedom.
     */
    std::size_t dofCount() const noexcept
    {
        return q_.size();
    }

    /**
     * @brief Returns the number of registered joint frames.
     *
     * @return The joint count.
     */
    std::size_t jointCount() const noexcept
    {
        return offsets_.size();
    }

  private:
    Q                                       q_;
    std::unordered_map<const Frame*, std::size_t> offsets_;
};

VN_ROBOTICS_KINEMATICS_NS_END
