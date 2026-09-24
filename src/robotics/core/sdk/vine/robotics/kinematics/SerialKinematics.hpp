#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/robotics/kinematics/KinematicsBase.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Kinematics of a serial (open) chain between a base and an end frame.
 *
 * Walks the frame tree from the end frame up to the base frame, collecting
 * every frame with degrees of freedom (the joints) and deriving the per-dof
 * motion data (types, bounds, velocity/acceleration limits) from the joint
 * DofInfos. Picks an IK solver at construction: Pieper when the chain is a
 * supported 6-DOF spherical-wrist robot, Jacobian (iterative) otherwise.
 */
class VN_ROBOTICS_CORE_API SerialKinematics : public KinematicsBase
{
  public:
    /**
     * @brief Builds the serial-chain model between base and end.
     *
     * @param base The chain root frame; must be an ancestor of end.
     * @param end The chain end frame.
     * @throws std::invalid_argument when either frame is null.
     * @throws std::runtime_error when end is not a descendant of base.
     */
    SerialKinematics(raw_ptr<Frame> base, raw_ptr<Frame> end);

    /**
     * @brief Destroys the kinematics.
     */
    ~SerialKinematics() override;

  public:
    /**
     * @brief Solves IK for a target pose expressed in the chain base.
     *
     * @param pose The target pose of the end frame in the chain base frame.
     * @param guess The initial joint-state guess (unused by the current
     *              solvers, kept for API compatibility).
     * @return All feasible joint-state solutions.
     */
    std::vector<Q> solveIK(const math::Isometry3d& pose, const Q& guess = {}) const override;

    /**
     * @brief Sets the default IK solver type, recreating the underlying
     *        solver to match.
     *
     * None disables solving; Pieper selects the analytic solver (only valid
     * for a supported 6-DOF spherical-wrist robot), Iterative selects the
     * Jacobian numerical solver.
     *
     * @param type The solver type.
     */
    void setIKSolverType(IKSolverType type) override;

  private:
    /// Builds the chain model and picks the default IK solver.
    void init(raw_ptr<Frame> base, raw_ptr<Frame> end);

  private:
    std::vector<DofInfo> ik_dofs_;
};

VN_ROBOTICS_KINEMATICS_NS_END
