#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <cstddef>
#include <vector>

#include <vine/IHierarchyNode.hpp>
#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/SmallVector.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

enum class FrameType
{
    /** 0 DoF */
    Fixed = 0,
    /** 1 DoF [theta] */
    RevoluteJoint,
    /** 1 DoF [x] */
    PrismaticJoint,
    /** 3 DoF [x y theta] */
    PlanarJoint
};

class State;

/**
 * @brief Kinematic frame (joint coordinate system) of a kinematic chain.
 *
 * Frames form a tree through parent/child links. The ancestor algorithm is
 * inherited from IHierarchyNode and reused instead of being re-implemented. The
 * tree links are non-owning: frames are owned externally (e.g. by the robot
 * model) and must outlive the tree that references them.
 */
class VN_ROBOTICS_CORE_API Frame : public vn::Object,
                                  public vn::INameable,
                                  public vn::IHierarchyNode<Frame> {
    VN_OBJECT_META(Frame, vn::Object, vn::INameable);

  public:
    Frame();

  protected:
    Frame(FrameType type);

  public:
    const String& name() const noexcept override
    {
        return name_;
    }

    void setName(const String& name) override
    {
        name_ = name;
    }

    FrameType frameType() const
    {
        return type_;
    }

    /**
     * @brief Returns the number of degrees of freedom of this frame.
     *
     * Derived from the frame type: Fixed = 0, Revolute/Prismatic = 1,
     * Planar = 3.
     *
     * @return The DoF count.
     */
    std::size_t dof() const noexcept
    {
        return dof_;
    }

    /**
     * @brief Returns the number of degrees of freedom for the given frame
     *        type.
     *
     * @param type The frame type.
     * @return The DoF count.
     */
    static std::size_t dofOfType(FrameType type) noexcept;

    math::Isometry3d fixedTransform() const
    {
        return fixed_tf_;
    }

    void setFixedTransform(const math::Isometry3d& tf)
    {
        fixed_tf_ = tf;
    }

    /**
     * @brief Returns the pose of this frame relative to its parent frame.
     *
     * For a fixed frame the scene state is ignored and the fixed transform is
     * returned; movable frames (e.g. a joint) compose their degrees of freedom
     * read from the state.
     *
     * @param state The scene state, used to query joint values.
     * @return The transform relative to the parent frame.
     */
    virtual math::Isometry3d transform(const State& state) const
    {
        return fixed_tf_;
    }

    virtual const std::vector<DofInfo>& dofInfos() const
    {
        static std::vector<DofInfo> empty;
        return empty;
    }

    /**
     * @brief Returns the parent frame.
     *
     * @return The parent frame, or null for a root frame.
     */
    raw_ptr<Frame> parent() const noexcept override
    {
        return parent_;
    }

    /**
     * @brief Returns the number of child frames.
     *
     * @return The child count.
     */
    std::size_t childCount() const noexcept override
    {
        return children_.size();
    }

    /**
     * @brief Returns the child frame at the given index.
     *
     * @param index The child index.
     * @return The child frame, or null when the index is out of range.
     */
    raw_ptr<Frame> childAt(std::size_t index) const override
    {
        return index < children_.size() ? children_[index] : nullptr;
    }

    /**
     * @brief Appends a child frame.
     *
     * The child must be non-null, must not be this frame and must not
     * already have a parent.
     *
     * @param child The child frame to append.
     * @throws std::invalid_argument on invalid input.
     */
    void addChild(raw_ptr<Frame> child);

    /**
     * @brief Removes a direct child frame.
     *
     * @param child The direct child frame to remove.
     * @throws std::invalid_argument when child is not a direct child.
     */
    void removeChild(raw_ptr<Frame> child);

    /**
     * @brief Returns the direct child frames.
     *
     * @return Read-only reference to the children.
     */
    const std::vector<raw_ptr<Frame>>& children() const noexcept
    {
        return children_;
    }

    /**
     * @brief Checks whether this frame is a root frame.
     *
     * @return true when the frame has no parent.
     */
    bool isRoot() const noexcept
    {
        return parent_ == nullptr;
    }

    /**
     * @brief Computes the transform that maps points from the given frame to
     *        the world (root) frame of the tree.
     *
     * The result is the pose of frame expressed in the root coordinate
     * system: the transforms of frame and all its ancestors are composed from
     * the root down to frame.
     *
     * @param frame The source frame.
     * @param state The scene state, used to query joint values.
     * @return The world transform.
     * @throws std::invalid_argument when frame is null.
     */
    static math::Isometry3d frameInWorld(raw_ptr<const Frame> frame, const State& state);

    /**
     * @brief Computes the transform that maps points from the from frame to
     *        the to frame.
     *
     * The result satisfies: p_to = frameInFrame(from, to, state) * p_from.
     * Both frames must belong to the same frame tree.
     *
     * @param from The source frame.
     * @param to The target frame.
     * @param state The scene state, used to query joint values.
     * @return The relative transform.
     * @throws std::invalid_argument when either frame is null or the frames
     *         do not share the same root.
     */
    static math::Isometry3d frameInFrame(raw_ptr<const Frame> from, raw_ptr<const Frame> to, const State& state);

  private:
    String              name_;
    FrameType           type_{ FrameType::Fixed };
    math::Isometry3d    fixed_tf_;
    std::size_t         dof_{ 0 };
    raw_ptr<Frame>      parent_{ nullptr };
    std::vector<raw_ptr<Frame>> children_;
};

VN_ROBOTICS_KINEMATICS_NS_END