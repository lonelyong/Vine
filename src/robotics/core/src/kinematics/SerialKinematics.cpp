#include <vine/robotics/kinematics/SerialKinematics.hpp>

#include <algorithm>
#include <stdexcept>

#include <vine/robotics/kinematics/JacobianIKSolver.hpp>
#include <vine/robotics/kinematics/PieperIKSolver.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

SerialKinematics::SerialKinematics(raw_ptr<Frame> base, raw_ptr<Frame> end)
{
    init(base, end);
}

SerialKinematics::~SerialKinematics() = default;

void SerialKinematics::init(raw_ptr<Frame> base, raw_ptr<Frame> end)
{
    if (!base) {
        throw std::invalid_argument("SerialKinematics::init, the base is null.");
    }
    if (!end) {
        throw std::invalid_argument("SerialKinematics::init, the end is null.");
    }
    if (end == base) {
        return;
    }
    if (!base->isAncestorOf(end)) {
        throw std::runtime_error("SerialKinematics::init, the end is not a descendant of the base.");
    }

    // 遍历运动链收集所有坐标系(运动关节 + 固定坐标系), 按链序(base -> end)
    std::vector<raw_ptr<Frame>> chain;
    std::size_t                 total_dof = 0;
    raw_ptr<Frame>              f = end;
    while (f) {
        chain.push_back(f);
        total_dof += f->dofInfos().size();
        f = f->parent();
        if (f == base) {
            break;
        }
    }
    std::reverse(chain.begin(), chain.end());

    common_base_ = base;

    // 收集运动关节(自由度非空的坐标系)
    joints_.clear();
    for (auto* const frame : chain) {
        if (!frame->dofInfos().empty()) {
            joints_.push_back(frame);
        }
    }

    // 链上没有任何运动关节(纯固定链): 自由度为 0
    if (joints_.empty()) {
        return;
    }

    dofs_.reserve(total_dof);
    lower_bounds_.reserve(total_dof);
    upper_bounds_.reserve(total_dof);
    velocity_limits_.reserve(total_dof);
    acceleration_limits_.reserve(total_dof);
    resolutions_.reserve(total_dof);

    std::vector<DofInfo> ik_dofs;
    ik_dofs.reserve(total_dof);

    // 从每个关节的 DofInfo 推导运动数据; 相邻运动关节之间穿插的固定坐标系
    // 变换累积后乘到子关节的 origin 上(轴不变, 因旋转发生在 origin 的局部坐标系内)
    math::Isometry3d fixed_accum; // 自 base/上一个运动关节到当前坐标系的固定变换
    for (const auto* const frame : chain) {
        if (frame->dofInfos().empty()) {
            fixed_accum = fixed_accum * frame->fixedTransform();
            continue;
        }
        for (const auto& dof : frame->dofInfos()) {
            DofInfo effective = dof;
            effective.origin = fixed_accum * effective.origin;
            dofs_.push_back(effective.type);
            lower_bounds_.append(effective.lower);
            upper_bounds_.append(effective.upper);
            velocity_limits_.append(effective.velocity_limit);
            acceleration_limits_.append(effective.acceleration_limit);
            resolutions_.append(effective.type == DofType::RevoluteJoint ? 0.05 : 10.0);
            ik_dofs.push_back(effective);
        }
        // 该运动关节之后的固定变换重新累积
        fixed_accum = math::Isometry3d{};
    }
    ik_dofs_ = std::move(ik_dofs);

    end_joints_.push_back(joints_.back());

    // 按基类配置的默认求解器类型创建求解器
    setIKSolverType(default_ik_solver_type_);
}

void SerialKinematics::setIKSolverType(IKSolverType type)
{
    // 先配置基类的默认求解器类型, 再据此重建底层求解器
    KinematicsBase::setIKSolverType(type);
    switch (default_ik_solver_type_) {
        case IKSolverType::None:
            ik_solver_.reset();
            break;
        case IKSolverType::Pieper:
            ik_solver_ = std::make_unique<PieperIKSolver>(ik_dofs_);
            break;
        case IKSolverType::Iterative:
        default:
            ik_solver_ = std::make_unique<JacobianIKSolver>(ik_dofs_);
            break;
    }
}

std::vector<Q> SerialKinematics::solveIK(const math::Isometry3d& pose, const Q& guess) const
{
    if (default_ik_solver_type_ == IKSolverType::None || !ik_solver_) {
        return {};
    }
    std::vector<Q> solutions;
    ik_solver_->solve(pose, solutions);
    return solutions;
}

VN_ROBOTICS_KINEMATICS_NS_END
