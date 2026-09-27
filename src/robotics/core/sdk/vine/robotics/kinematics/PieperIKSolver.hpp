#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <array>

#include <vine/robotics/kinematics/ClosedFormIKSolver.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Analytic inverse kinematics for 6-DOF serial robots with a spherical wrist.
 *
 * Implements the Pieper decoupling method under the modified Denavit-Hartenberg
 * (MDH / Craig) convention: Tᵢ = Rx(αᵢ₋₁)·Tx(aᵢ₋₁)·Rz(θᵢ)·Tz(dᵢ).
 *
 * Supported class - every requirement is validated in the constructor:
 *
 * - exactly 6 joints, each one MDH-representable;
 * - spherical wrist: a₃ = a₄ = a₅ = 0 and d₅ = 0, so that the axes of joints 4, 5 and 6
 *   meet at a single point;
 * - arm geometry: |sin α₁| = 1, sin α₂ = 0 (α₂ = 0 or π), a₂ ≠ 0, sin α₃ ≠ 0 and d₄ ≠ 0,
 *   so that joint 3 observably moves the wrist centre;
 * - wrist twists: sin α₄ ≠ 0 and sin α₅ ≠ 0, i.e. no two consecutive wrist axes may be
 *   parallel or anti-parallel (that would leave the wrist with only two degrees of freedom).
 *
 * At most 8 solutions are produced (2 shoulder × 2 elbow × 2 wrist branches).
 * Branches that are unreachable, violate joint limits or fail the internal
 * forward-kinematics cross-check are discarded, so fewer may be returned.
 *
 * Robots outside the supported class are rejected: isValid() reports false and
 * every solve() call returns false immediately.
 */
class VN_ROBOTICS_CORE_API PieperIKSolver : public ClosedFormIKSolver {

  public:
    /**
     * @brief Builds the solver from the joint descriptors and validates the Pieper preconditions.
     *
     * @param dofs Joint descriptors in base-to-tool order.
     */
    PieperIKSolver(const std::vector<DofInfo>& dofs);

  public:
    /**
     * @brief Solves the inverse kinematics for a target pose.
     *
     * Equivalent to calling solve(target, solutions, Q{}) : the solutions are ordered by
     * angular distance from the zero configuration.
     *
     * @param target Desired pose of the tool frame (frame 6) in the robot base frame.
     * @param solutions Receives the solutions in joint space; left empty when none is found.
     * @return true if at least one solution was found, false otherwise.
     */
    bool solve(const math::Isometry3d& target, std::vector<Q>& solutions) const override;

    /**
     * @brief Solves the inverse kinematics and orders the solutions by distance from a seed.
     *
     * @param target Desired pose of the tool frame (frame 6) in the robot base frame.
     * @param solutions Receives the solutions sorted by increasing angular distance from seed.
     * @param seed Reference joint configuration; joints are compared with 2π-periodic wrapping.
     * @return true if at least one solution was found, false otherwise.
     */
    bool solve(const math::Isometry3d& target, std::vector<Q>& solutions, const Q& seed) const;

  private:
    /** @brief MDH parameters extracted once in the constructor. */
    std::array<DHParameter, 6> mdh_;
};

VN_ROBOTICS_KINEMATICS_NS_END