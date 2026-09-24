#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <array>

#include <vine/robotics/kinematics/ClosedFormIKSolver.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/*
 * PieperIKSolver – analytic inverse kinematics for 6-DOF serial robots
 * with spherical wrist, using the Pieper decoupling method under
 * Modified Denavit-Hartenberg (MDH / Craig) convention.
 *
 * Supported robot type
 * --------------------
 *   dofs  = 6
 *   MDH convention  (Tᵢ = Rx(αᵢ₋₁)·Tx(aᵢ₋₁)·Rz(θᵢ)·Tz(dᵢ))
 *
 *   Spherical wrist (axes 4,5,6 intersect at one point):
 *     a₃ = 0,  a₄ = 0,  a₅ = 0
 *
 *   Arm geometry:
 *     α₁ ≈ ±90°   (joint 1 perpendicular to joint 2)
 *     α₂ ≈ 0, π   (joint 2 parallel to joint 3)
 *     a₂ ≠ 0      (non-zero link length)
 *
 *   Max solutions: 8  (2 arm branches × 2 elbow configs × 2 wrist flips)
 *
 * Validation happens in the constructor.  If the robot does not meet
 * the above preconditions, isValid() returns false and every solve()
 * call returns false immediately.
 *
 * The solve(seed) overload sorts results by angular distance from the
 * seed joint configuration, respecting 2π-periodic equivalence.
 */
class VN_ROBOTICS_CORE_API PieperIKSolver : public ClosedFormIKSolver {

  public:
    /*
     * Construct from joint descriptors.
     * Extracts MDH parameters and validates the Pieper preconditions.
     * Use isValid() afterwards to check whether the robot is supported.
     */
    PieperIKSolver(const std::vector<DofInfo>& dofs);

  public:
    /*
     * Solve IK for target pose.  Solutions are returned unsorted.
     * Returns true if at least one solution was found.
     */
    bool solve(const math::Isometry3d& target, std::vector<Q>& solutions) const override;

    /*
     * Solve IK for target pose, sorted by angular distance from seed.
     * Each joint in the seed is compared with 2π-periodic wrapping.
     */
    bool solve(const math::Isometry3d& target, std::vector<Q>& solutions, const Q& seed) const;

  private:
    /* Pre-extracted MDH parameters (computed once in constructor). */
    std::array<DHParameter, 6> mdh_;
};

VN_ROBOTICS_KINEMATICS_NS_END