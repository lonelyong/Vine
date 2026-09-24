#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/robotics/kinematics/IKSolver.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

class VN_ROBOTICS_CORE_API ClosedFormIKSolver : public IKSolver {

    using IKSolver::IKSolver;
};

VN_ROBOTICS_KINEMATICS_NS_END