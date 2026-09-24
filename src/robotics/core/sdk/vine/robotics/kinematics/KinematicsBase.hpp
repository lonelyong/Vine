#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include <vine/raw_ptr.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/IKSolver.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/kinematics/State.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Kind of inverse kinematics solver.
 */
enum class IKSolverType
{
    None,
    Pieper,
    Iterative,
};

/**
 * @brief Base class of a kinematic chain model.
 *
 * Models a chain of joints (frames whose dofInfos() is non-empty) together
 * with the per-dof motion data: types, lower/upper bounds and velocity /
 * acceleration limits. Joint values are read and written through a scene
 * State; the concrete subclass provides the IK solving.
 */
class VN_ROBOTICS_CORE_API KinematicsBase
{
  public:
    virtual ~KinematicsBase() = default;

    /**
     * @brief Validates a joint-space vector against the dof count and the
     *        lower/upper bounds.
     *
     * @param q The joint values.
     * @throws std::runtime_error when q.size() differs from the dof count.
     * @throws std::invalid_argument when a value is outside the bounds.
     */
    void validateQ(const Q& q) const;

    /**
     * @brief Returns the joints of the chain (chain order).
     *
     * @return The joints, non-owning views.
     */
    const std::vector<raw_ptr<Frame>>& joints() const
    {
        return joints_;
    }

    /**
     * @brief Returns the motion type of every dof, one entry per dof.
     *
     * @return The dof types.
     */
    const std::vector<DofType>& dofs() const
    {
        return dofs_;
    }

    /**
     * @brief Returns the number of degrees of freedom.
     *
     * @return The dof count.
     */
    std::size_t dof() const
    {
        return dofs_.size();
    }

    /**
     * @brief Returns the motion type of a single dof.
     *
     * @param idx The dof index.
     * @return The dof type.
     */
    DofType dofType(std::size_t idx) const
    {
        return dofs_.at(idx);
    }

    /**
     * @brief Returns the per-dof lower bounds.
     *
     * @return The lower bounds.
     */
    const Q& lowerBounds() const
    {
        return lower_bounds_;
    }

    /**
     * @brief Returns the per-dof upper bounds.
     *
     * @return The upper bounds.
     */
    const Q& upperBounds() const
    {
        return upper_bounds_;
    }

    /**
     * @brief Returns the per-dof velocity limits.
     *
     * @return The velocity limits.
     */
    const Q& maxVelocityLimits() const
    {
        return velocity_limits_;
    }

    /**
     * @brief Returns the per-dof acceleration limits.
     *
     * @return The acceleration limits.
     */
    const Q& maxAccelerationLimits() const
    {
        return acceleration_limits_;
    }

    /**
     * @brief Returns the per-dof constraint-checking resolution.
     *
     * @return The joint resolutions.
     */
    const Q& jointResolutions() const
    {
        return resolutions_;
    }

    /**
     * @brief Returns the default IK solver type.
     *
     * @return The solver type.
     */
    IKSolverType ikSolverType() const
    {
        return default_ik_solver_type_;
    }

    /**
     * @brief Sets the default IK solver type.
     *
     * Concrete subclasses may recreate the underlying solver to match;
     * None disables IK solving.
     *
     * @param type The solver type.
     */
    virtual void setIKSolverType(IKSolverType type);

    /**
     * @brief Returns the underlying IK solver (non-owning).
     *
     * @return The IK solver, or null when not set.
     */
    raw_ptr<const IKSolver> ikSolver() const
    {
        return ik_solver_.get();
    }

    /**
     * @brief Sets the lower bounds.
     *
     * @param bounds New lower bounds; its size must match the dof count.
     */
    void setLowerBounds(const Q& bounds);

    /**
     * @brief Sets the upper bounds.
     *
     * @param bounds New upper bounds; its size must match the dof count.
     */
    void setUpperBounds(const Q& bounds);

    /**
     * @brief Sets the velocity limits.
     *
     * @param limits New velocity limits; its size must match the dof count.
     */
    void setMaxVelocityLimits(const Q& limits);

    /**
     * @brief Sets the acceleration limits.
     *
     * @param limits New acceleration limits; its size must match the dof count.
     */
    void setMaxAccelerationLimits(const Q& limits);

    /**
     * @brief Sets the joint resolutions.
     *
     * @param limits New resolutions; its size must match the dof count.
     */
    void setJointResolutions(const Q& limits);

    /**
     * @brief Solves IK for a target pose expressed in the chain base.
     *
     * @param pose The target pose of the end frame in the chain base frame.
     * @param guess The initial joint-state guess (used by iterative solvers).
     * @return All feasible joint-state solutions.
     */
    virtual std::vector<Q> solveIK(const math::Isometry3d& pose, const Q& guess = {}) const = 0;

    /**
     * @brief Solves IK with explicit end/base frames.
     *
     * Transforms the target pose into the chain frame using the scene state:
     * the pose is the desired pose of end in base, and the result is the
     * target of the chain's last joint in the first joint's parent.
     *
     * @param pose The target pose of end in base.
     * @param end The end frame.
     * @param base The base frame.
     * @param state The scene state.
     * @param guess The initial joint-state guess.
     * @return All feasible joint-state solutions, empty when the frames do
     *         not match the chain.
     */
    std::vector<Q> solveIK(const math::Isometry3d& pose,
                           raw_ptr<const Frame>    end,
                           raw_ptr<const Frame>    base,
                           const State&            state,
                           const Q&                guess = {});

    /**
     * @brief Writes the joint values of this chain into a scene state.
     *
     * @param q The joint values; must match the dof count and bounds.
     * @param state The scene state to write into.
     */
    void setQ(const Q& q, State& state) const;

    /**
     * @brief Reads the joint values of this chain from a scene state.
     *
     * @param state The scene state.
     * @return The joint values.
     */
    Q getQ(const State& state) const;

  protected:
    std::vector<DofType>          dofs_;
    Q                             lower_bounds_;
    Q                             upper_bounds_;
    Q                             velocity_limits_;
    Q                             acceleration_limits_;
    Q                             resolutions_;
    raw_ptr<Frame>                common_base_{ nullptr };
    std::vector<raw_ptr<Frame>>   end_joints_;
    std::vector<raw_ptr<Frame>>   joints_;
    IKSolverType                  default_ik_solver_type_{ IKSolverType::Iterative };
    std::unique_ptr<IKSolver>     ik_solver_;
};

VN_ROBOTICS_KINEMATICS_NS_END
