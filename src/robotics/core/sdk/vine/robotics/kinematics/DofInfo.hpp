#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vine/math/Isometry3.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

enum class DofType
{
    RevoluteJoint,
    /** 1 DoF [x] */
    PrismaticJoint,
};

struct DofInfo {
    DofType          type = DofType::RevoluteJoint;
    math::Isometry3d origin;
    math::Vec3d      axis;
    double           lower{};
    double           upper{};
    double           velocity_limit{};
    double           acceleration_limit{};
};

VN_ROBOTICS_KINEMATICS_NS_END