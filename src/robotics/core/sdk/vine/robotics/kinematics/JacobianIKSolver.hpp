#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/robotics/kinematics/IterativeIKSolver.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

class VN_ROBOTICS_CORE_API JacobianIKSolver : public IterativeIKSolver {

  public:
    JacobianIKSolver(const std::vector<DofInfo>& dofs)
      : IterativeIKSolver(dofs)
    {}

  public:
    bool solve(const math::Isometry3d& target, std::vector<Q>& solutions) const override;
};

VN_ROBOTICS_KINEMATICS_NS_END