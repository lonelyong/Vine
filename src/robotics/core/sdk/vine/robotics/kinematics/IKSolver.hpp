#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <vector>

#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Q.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

class VN_ROBOTICS_CORE_API IKSolver {
  public:
    virtual ~IKSolver() = default;

  protected:
    IKSolver(const std::vector<DofInfo>& dofs)
      : dofs_(dofs)
      , is_valid_(!dofs_.empty())
    {}


  public:
    virtual bool solve(const math::Isometry3d& target, std::vector<Q>& solutions) const = 0;

    const std::vector<DofInfo>& dofs() const
    {
        return dofs_;
    }

    virtual bool isValid() const
    {
        return is_valid_;
    };

    /**
     * @brief Enables or disables joint-limit checking.
     *
     * @param check true to discard solutions outside the DofInfo bounds (default), false to accept them.
     */
    void setCheckJointLimits(bool check)
    {
        check_joint_limits_ = check;
    }

    /**
     * @brief Reports whether solve() discards solutions outside the joint bounds.
     *
     * @return true when solutions violating the DofInfo limits are rejected.
     */
    bool isCheckingJointLimits() const
    {
        return check_joint_limits_;
    }

  protected:
    std::vector<DofInfo> dofs_;
    bool                 is_valid_;
    bool                 check_joint_limits_{ true };
};

VN_ROBOTICS_KINEMATICS_NS_END