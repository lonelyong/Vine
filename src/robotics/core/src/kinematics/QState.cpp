#include <vine/robotics/kinematics/QState.hpp>

#include <stdexcept>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

void QState::setup(const Frame* root)
{
    offsets_.clear();
    q_ = Q();

    if (!root) {
        return;
    }

    std::size_t offset = 0;

    auto collect = [&](auto&& self, const Frame* frame) -> void {
        const std::size_t dof = frame->dofInfos().size();
        if (dof > 0) {
            offsets_[frame] = offset;
            offset += dof;
        }
        for (std::size_t i = 0; i < frame->childCount(); ++i) {
            self(self, frame->childAt(i));
        }
    };

    collect(collect, root);

    q_ = Q(offset);
}

Q QState::getQ(const Frame* joint) const
{
    const auto it = offsets_.find(joint);
    if (it == offsets_.end()) {
        return {};
    }
    return q_.subQ(it->second, joint->dofInfos().size());
}

void QState::setQ(const Frame* joint, const Q& q)
{
    const auto it = offsets_.find(joint);
    if (it == offsets_.end()) {
        throw std::invalid_argument("QState::setQ, joint is not registered");
    }
    const std::size_t dof = joint->dofInfos().size();
    if (q.size() != dof) {
        throw std::invalid_argument("QState::setQ, q.size() does not match the joint dof");
    }
    q_.set(it->second, q.data(), dof);
}

void QState::copyFrom(const QState& other)
{
    for (const auto& [frame, other_offset] : other.offsets_) {
        const auto it = offsets_.find(frame);
        if (it != offsets_.end()) {
            q_.set(it->second, other.q_.data() + other_offset, frame->dofInfos().size());
        }
    }
}

VN_ROBOTICS_KINEMATICS_NS_END
