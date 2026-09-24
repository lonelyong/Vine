#include <vine/robotics/workcell/Joint.hpp>

#include <vine/robotics/kinematics/State.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

std::unique_ptr<Joint> Joint::clone() const
{
    auto out = std::make_unique<Joint>(frameType());
    out->setName(name());
    out->setFixedTransform(fixedTransform());
    out->dof_infos_ = dof_infos_;
    // parent/child links are rebound by the caller (DeviceData::copyBaseFrom)
    return out;
}

math::Isometry3d Joint::transform(const kinematics::State& state) const
{
    return transform(state.qstate(this).getQ(this));
}

math::Isometry3d Joint::transform(const kinematics::Q& q) const
{
    if (q.size() < dof_infos_.size()) {
        return fixedTransform();
    }

    math::Isometry3d tf = fixedTransform();
    for (std::size_t i = 0; i < dof_infos_.size(); ++i) {
        const auto& dof = dof_infos_[i];
        math::Isometry3d motion = dof.origin;
        if (dof.type == kinematics::DofType::RevoluteJoint) {
            math::Isometry3d rot;
            rot.preRotate(dof.axis, q[i]);
            motion = motion * rot;
        } else if (dof.type == kinematics::DofType::PrismaticJoint) {
            math::Isometry3d trans;
            trans.postTranslate(dof.axis * q[i]);
            motion = motion * trans;
        }
        tf = tf * motion;
    }
    return tf;
}

VN_ROBOTICS_WORKCELL_NS_END
