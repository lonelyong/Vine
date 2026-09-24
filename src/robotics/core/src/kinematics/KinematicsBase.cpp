#include <vine/robotics/kinematics/KinematicsBase.hpp>

#include <stdexcept>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

void KinematicsBase::setIKSolverType(IKSolverType type)
{
    default_ik_solver_type_ = type;
}

void KinematicsBase::validateQ(const Q& q) const
{
    if (q.size() != dof()) {
        throw std::runtime_error("KinematicsBase::validateQ, q has a different size than the DOF.");
    }
    for (std::size_t i = 0; i < dof(); ++i) {
        if (q[i] < lower_bounds_[i] || q[i] > upper_bounds_[i]) {
            throw std::invalid_argument("KinematicsBase::validateQ, q is out of the allowed bounds.");
        }
    }
}

void KinematicsBase::setLowerBounds(const Q& bounds)
{
    if (bounds.size() != dof()) {
        throw std::invalid_argument("KinematicsBase::setLowerBounds, bounds size does not match the DOF.");
    }
    lower_bounds_ = bounds;
}

void KinematicsBase::setUpperBounds(const Q& bounds)
{
    if (bounds.size() != dof()) {
        throw std::invalid_argument("KinematicsBase::setUpperBounds, bounds size does not match the DOF.");
    }
    upper_bounds_ = bounds;
}

void KinematicsBase::setMaxVelocityLimits(const Q& limits)
{
    if (limits.size() != dof()) {
        throw std::invalid_argument("KinematicsBase::setMaxVelocityLimits, limits size does not match the DOF.");
    }
    velocity_limits_ = limits;
}

void KinematicsBase::setMaxAccelerationLimits(const Q& limits)
{
    if (limits.size() != dof()) {
        throw std::invalid_argument("KinematicsBase::setMaxAccelerationLimits, limits size does not match the DOF.");
    }
    acceleration_limits_ = limits;
}

void KinematicsBase::setJointResolutions(const Q& limits)
{
    if (limits.size() != dof()) {
        throw std::invalid_argument("KinematicsBase::setJointResolutions, limits size does not match the DOF.");
    }
    resolutions_ = limits;
}

std::vector<Q> KinematicsBase::solveIK(const math::Isometry3d& pose,
                                       raw_ptr<const Frame>    end,
                                       raw_ptr<const Frame>    base,
                                       const State&            state,
                                       const Q&                guess)
{
    if (!end || !base || default_ik_solver_type_ == IKSolverType::None || joints_.empty()) {
        return {};
    }
    const auto* const first_joint_parent = joints_.front()->parent();
    const auto* const last_joint         = joints_.back();
    if (!last_joint->isAncestorOf(end)
        || (first_joint_parent != base && !first_joint_parent->isDescendantOf(base))) {
        return {};
    }
    const auto p = Frame::frameInFrame(base, first_joint_parent, state)
                   * pose
                   * Frame::frameInFrame(last_joint, end, state);
    return solveIK(p, guess);
}

void KinematicsBase::setQ(const Q& q, State& state) const
{
    validateQ(q);

    std::size_t offset = 0;
    for (const auto* const joint : joints_) {
        const std::size_t jdof = joint->dofInfos().size();
        state.qstate(common_base_).setQ(joint, q.subQ(offset, jdof));
        offset += jdof;
    }
}

Q KinematicsBase::getQ(const State& state) const
{
    Q q(dof());
    std::size_t offset = 0;
    for (const auto* const joint : joints_) {
        const Q jq = state.qstate(common_base_).getQ(joint);
        q.set(offset, jq.data(), jq.size());
        offset += jq.size();
    }
    return q;
}

VN_ROBOTICS_KINEMATICS_NS_END
