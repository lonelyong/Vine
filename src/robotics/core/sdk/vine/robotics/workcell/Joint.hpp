#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <memory>
#include <vector>

#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/Q.hpp>

#include "Link.hpp"

VN_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief A joint connecting two links of a Device.
 *
 * A joint is a kinematic Frame that carries its own degrees of freedom. Each
 * DoF is a DofInfo (motion type, axis, limits, ...); the motion is applied
 * per DoF, so a planar joint stays a single Joint with three DoFs (two
 * prismatic + one revolute) instead of three separate joints.
 *
 * The joint frame doubles as the child link's base frame: it is a child of
 * the parent link's base frame in the coordinate-frame tree, and the child
 * link's geometry is expressed relative to it. The parent/child links are
 * held as non-owning references and describe the kinematic graph used by
 * Device::init.
 *
 * Owned by a Device through a smart pointer.
 */
class VN_ROBOTICS_CORE_API Joint : public kinematics::Frame
{
  public:
    /**
     * @brief Constructs a fixed joint.
     */
    Joint() = default;

    /**
     * @brief Constructs a joint with the given frame type.
     *
     * The DoF infos are pre-sized to the type's dof count (see
     * Frame::dofOfType): 1 for revolute/prismatic, 3 for planar.
     *
     * @param type The joint frame type.
     */
    explicit Joint(kinematics::FrameType type)
      : Frame(type)
      , dof_infos_(dof())
    {}

    /**
     * @brief Destroys the joint.
     */
    ~Joint() override = default;

    /**
     * @brief Returns the joint's degrees of freedom.
     *
     * @return The DoF infos.
     */
    const std::vector<kinematics::DofInfo>& dofInfos() const override
    {
        return dof_infos_;
    }

    /**
     * @brief Sets the joint's degrees of freedom as a whole.
     *
     * @param dof_infos New DoF infos.
     */
    void setDofInfos(const std::vector<kinematics::DofInfo>& dof_infos)
    {
        dof_infos_ = dof_infos;
    }

    /**
     * @brief Returns the joint pose relative to its parent frame.
     *
     * Reads the joint values from the scene state and delegates to
     * transform(const Q&).
     *
     * @param state The scene state, used to query the joint values.
     * @return The transform relative to the parent frame.
     */
    math::Isometry3d transform(const kinematics::State& state) const override;

    /**
     * @brief Returns the joint pose relative to its parent frame, computed
     *        from explicit joint values.
     *
     * Composes the joint's fixed transform with its degrees of freedom: a
     * revolute dof rotates about its axis, a prismatic dof translates along
     * its axis.
     *
     * @param q The joint values, one per dof.
     * @return The transform relative to the parent frame.
     */
    math::Isometry3d transform(const kinematics::Q& q) const;

    /**
     * @brief Returns the parent link (the link this joint hangs from).
     *
     * @return The parent link, or null.
     */
    raw_ptr<Link> parentLink() const
    {
        return parent_link_;
    }

    /**
     * @brief Sets the parent link.
     *
     * @param link The parent link, or null.
     */
    void setParentLink(raw_ptr<Link> link)
    {
        parent_link_ = link;
    }

    /**
     * @brief Returns the child link (the link mounted on this joint).
     *
     * @return The child link, or null.
     */
    raw_ptr<Link> childLink() const
    {
        return child_link_;
    }

    /**
     * @brief Sets the child link.
     *
     * @param link The child link, or null.
     */
    void setChildLink(raw_ptr<Link> link)
    {
        child_link_ = link;
    }

    /**
     * @brief Creates a deep copy of this joint.
     *
     * The frame type, name, fixed transform and DoF infos are copied. The
     * parent/child links are left null; the caller rebinds them against the
     * cloned link set.
     *
     * @return The cloned joint, owned by the caller.
     */
    std::unique_ptr<Joint> clone() const;

  private:
    std::vector<kinematics::DofInfo> dof_infos_;
    raw_ptr<Link>                    parent_link_{ nullptr };
    raw_ptr<Link>                    child_link_{ nullptr };
};

VN_ROBOTICS_WORKCELL_NS_END
