#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <array>

#include <vine/robotics/kinematics/IKSolver.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/**
 * @brief Base class for iterative inverse kinematics solvers and their tuning knobs.
 *
 * Each knob has a default that preserves the behaviour of the concrete solvers, so a caller only
 * needs to touch what it cares about.
 */
class VN_ROBOTICS_CORE_API IterativeIKSolver : public IKSolver {

  public:
    using IKSolver::IKSolver;

  public:
    /**
     * @brief Returns the convergence tolerance applied to every enabled task component.
     *
     * @return Position tolerance in metres / orientation tolerance in radians.
     */
    double maxError() const
    {
        return max_error_;
    }

    /**
     * @brief Sets the convergence tolerance applied to every enabled task component.
     *
     * @param error Position tolerance in metres / orientation tolerance in radians.
     */
    void setMaxError(double error)
    {
        max_error_ = error;
    }

    /**
     * @brief Returns the iteration budget spent on a single seed.
     *
     * @return Maximum number of Newton/DLS iterations per seed.
     */
    int maxIterations() const
    {
        return max_iterations_;
    }

    /**
     * @brief Sets the iteration budget spent on a single seed.
     *
     * @param iterations Maximum number of Newton/DLS iterations per seed.
     */
    void setMaxIterations(int iterations)
    {
        max_iterations_ = iterations;
    }

    /**
     * @brief Returns how many seed configurations the solver may try.
     *
     * @return Number of seeds, including the deterministic zero seed.
     */
    int maxSeeds() const
    {
        return max_seeds_;
    }

    /**
     * @brief Sets how many seed configurations the solver may try.
     *
     * @param seeds Number of seeds, including the deterministic zero seed.
     */
    void setMaxSeeds(int seeds)
    {
        max_seeds_ = seeds;
    }

    /**
     * @brief Reports whether intermediate iterates are clamped into the joint bounds.
     *
     * @return true when the iterate is clamped to the DofInfo bounds after every step.
     */
    bool isClampToBounds() const
    {
        return clamp_to_bounds_;
    }

    /**
     * @brief Enables or disables clamping of intermediate iterates into the joint bounds.
     *
     * @param clamp true to clamp every iterate into the DofInfo bounds (default), false to leave it free.
     */
    void setClampToBounds(bool clamp)
    {
        clamp_to_bounds_ = clamp;
    }

    /**
     * @brief Returns the task-space components the solver is required to reach.
     *
     * @return Flags for x, y, z (indices 0-2) and the rotation vector components (indices 3-5);
     *         a disabled component is ignored by the convergence test.
     */
    const std::array<bool, 6>& constraintMask() const
    {
        return constraint_mask_;
    }

    /**
     * @brief Selects the task-space components the solver must reach.
     *
     * @param mask Flags for x, y, z (indices 0-2) and the rotation vector components (indices 3-5).
     */
    void setConstraintMask(const std::array<bool, 6>& mask)
    {
        constraint_mask_ = mask;
    }

  protected:
    double              max_error_{ 1e-6 };
    int                 max_iterations_{ 150 };
    int                 max_seeds_{ 8 };
    bool                clamp_to_bounds_{ true };
    std::array<bool, 6> constraint_mask_{ { true, true, true, true, true, true } };
};

VN_ROBOTICS_KINEMATICS_NS_END