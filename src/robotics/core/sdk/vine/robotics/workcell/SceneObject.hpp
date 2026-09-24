#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <memory>
#include <vector>

#include <vine/INameable.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/Frame.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

class Workcell;

/**
 * @brief Kind of a scene object.
 */
enum class SceneObjectKind
{
    Device,
    RigidObject,
};

/**
 * @brief Base class of user-managed objects in a Workcell.
 *
 * A SceneObject is something the user adds/removes/finds in the workcell
 * (a robot, a scanner, a part, a table, ...). The Workcell owns every
 * SceneObject (unique ownership); this base exposes identity (via INameable)
 * plus each object's own coordinate frames. Deriving from vn::Object makes
 * every scene object part of the Object type system (isKindOf / obj_cast).
 *
 * Parent/child relationships are NOT stored explicitly. They are derived from
 * the coordinate-frame tree: every object owns a base frame, and an object is
 * a child of another when its base frame hangs (directly or transitively)
 * from a frame owned by that other object. The owning Workcell resolves these
 * relationships through the frame tree (see parentObject(), childObjects(),
 * Workcell::parentOf(), ...).
 */
class VN_ROBOTICS_CORE_API SceneObject : public vn::Object, public vn::INameable
{
    VN_OBJECT_META(SceneObject, vn::Object, vn::INameable);

  public:
    /**
     * @brief Destroys the scene object.
     */
    ~SceneObject() override = default;

    /**
     * @brief Returns the object kind.
     *
     * @return The kind.
     */
    virtual SceneObjectKind kind() const = 0;

    /**
     * @brief Returns the object name (unique within a Workcell).
     *
     * @return The name.
     */
    const String& name() const noexcept override
    {
        return name_;
    }

    /**
     * @brief Sets the object name.
     *
     * Uniqueness within a Workcell is validated by the Workcell; this setter
     * does not perform that check.
     *
     * @param name The new name.
     */
    void setName(const String& name) override
    {
        name_ = name;
    }

    /**
     * @brief Returns the object's base coordinate frame.
     *
     * This frame is the root of the object's own coordinate system; its fixed
     * transform is the object pose relative to its parent frame.
     *
     * @return The base frame.
     */
    raw_ptr<kinematics::Frame> baseFrame()
    {
        return base_frame_.get();
    }

    /**
     * @brief Returns the object's base coordinate frame.
     *
     * @return The base frame.
     */
    raw_ptr<const kinematics::Frame> baseFrame() const
    {
        return base_frame_.get();
    }

    /**
     * @brief Returns every frame of this object: base + joints + ends.
     *
     * The pointers are non-owning; the frames are owned by this object and
     * remain valid while the object lives.
     *
     * @return The frames.
     */
    const std::vector<raw_ptr<kinematics::Frame>>& frames() const
    {
        return frames_;
    }

    /**
     * @brief Returns the workcell that owns this object.
     *
     * @return The owning workcell, or null when the object is not in one.
     */
    raw_ptr<Workcell> workcell() const
    {
        return workcell_;
    }

    /**
     * @brief Returns the object this object is directly mounted on.
     *
     * Resolved through the coordinate-frame tree: the parent object is the
     * one that owns this object's base-frame parent. Returns null when the
     * object is a top-level object (mounted on the workcell world frame) or
     * is not part of a workcell.
     *
     * @return The parent object, or null.
     */
    raw_ptr<SceneObject> parentObject() const;

    /**
     * @brief Returns the objects directly mounted on this object.
     *
     * @return The child objects.
     */
    std::vector<raw_ptr<SceneObject>> childObjects() const;

    /**
     * @brief Checks whether this object is a direct child of the given object.
     *
     * @param parent The candidate parent object.
     * @return true when the given object is the direct parent.
     */
    bool isChildOf(raw_ptr<const SceneObject> parent) const;

    /**
     * @brief Checks whether this object is a descendant of the given object.
     *
     * A descendant is any object whose base frame hangs transitively from the
     * given object's base frame in the coordinate-frame tree. An object is
     * not considered its own descendant.
     *
     * @param ancestor The candidate ancestor object.
     * @return true when the given object is an ancestor.
     */
    bool isDescendantOf(raw_ptr<const SceneObject> ancestor) const;

    /**
     * @brief Sets the fixed transform of the base frame.
     *
     * @param tf The new pose relative to the parent frame.
     */
    void setBaseTransform(const math::Isometry3d& tf)
    {
        base_frame_->setFixedTransform(tf);
    }

  protected:
    /// Creates the base frame and registers it in the frame registry.
    SceneObject()
      : base_frame_(std::make_unique<kinematics::Frame>())
    {
        frames_.push_back(base_frame_.get());
    }

    /**
     * @brief Constructs a scene object with the given name.
     *
     * @param name The object name.
     */
    explicit SceneObject(const String& name)
      : name_(name)
      , base_frame_(std::make_unique<kinematics::Frame>())
    {
        frames_.push_back(base_frame_.get());
    }

  protected:
    String                              name_;
    std::unique_ptr<kinematics::Frame>  base_frame_;
    std::vector<raw_ptr<kinematics::Frame>> frames_;
    raw_ptr<Workcell>                   workcell_{ nullptr };

    friend class Workcell;
};

VN_ROBOTICS_WORKCELL_NS_END
