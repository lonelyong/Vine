#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <memory>
#include <vector>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/workcell/SceneObject.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

/**
 * @brief A robotic workcell: the container of all scene objects.
 *
 * The Workcell OWNS every scene object (robot, scanner, part, table, fence,
 * ...) with unique ownership, and owns the world coordinate frame that all
 * top-level objects hang from.
 *
 * Ownership rules:
 *  - addSceneObject() transfers unique ownership to the workcell and returns
 *    a non-owning handle for configuration.
 *  - The Workcell maintains no explicit object tree: the parent/child
 *    relationships are derived from the coordinate-frame tree (see parentOf(),
 *    childrenOf(), descendantsOf(), SceneObject::parentObject()).
 *  - Removing an object re-parents any object mounted on its frame subtree to
 *    the removed object's parent frame, so no dangling frame parent remains.
 *  - Links and joints are internal to Device and never owned here.
 *
 * @note Not thread-safe.
 */
class VN_ROBOTICS_CORE_API Workcell final
{
  public:
    /**
     * @brief Constructs an empty workcell with a "world" root frame.
     */
    Workcell();

    /**
     * @brief Destroys the workcell and all owned objects.
     */
    ~Workcell();

    Workcell(const Workcell&)            = delete;
    Workcell& operator=(const Workcell&) = delete;
    Workcell(Workcell&&)                 = delete;
    Workcell& operator=(Workcell&&)      = delete;

  public:
    /**
     * @brief Returns the workcell name.
     *
     * @return The name.
     */
    const String& name() const
    {
        return name_;
    }

    /**
     * @brief Sets the workcell name.
     *
     * @param name The new name.
     */
    void setName(const String& name)
    {
        name_ = name;
    }

    /**
     * @brief Returns the world (root) coordinate frame.
     *
     * Top-level objects are mounted as children of this frame.
     *
     * @return The world frame.
     */
    raw_ptr<kinematics::Frame> worldFrame() const
    {
        return world_frame_.get();
    }

    /**
     * @brief Adds a scene object, taking ownership.
     *
     * The object's base frame is attached to parent_frame (or the world frame
     * when null), which registers it in the coordinate-frame tree.
     *
     * @param object The object to add; must not be owned by a workcell and
     *               must not reuse an existing object name.
     * @param parent_frame The frame to mount the object on, or null for the
     *                     world frame.
     * @return A non-owning pointer to the added object, or null on failure
     *         (null object, already owned, duplicate name, or would create a
     *         frame cycle).
     */
    raw_ptr<SceneObject> addSceneObject(std::unique_ptr<SceneObject> object,
                                        raw_ptr<kinematics::Frame>   parent_frame = nullptr);

    /**
     * @brief Removes and destroys a scene object by name.
     *
     * Any object mounted on the removed object's frame subtree is re-parented
     * to the removed object's parent frame.
     *
     * @param name The object name.
     * @return true when an object was removed.
     */
    bool removeSceneObject(const String& name);

    /**
     * @brief Finds a scene object by name.
     *
     * @param name The object name.
     * @return The object, or null when not found.
     */
    raw_ptr<SceneObject> findSceneObject(const String& name) const;

    /**
     * @brief Finds the scene object that owns the given frame.
     *
     * @param frame The frame to look up.
     * @return The owning object, or null when the frame is the world frame or
     *         does not belong to this workcell.
     */
    raw_ptr<SceneObject> findSceneObjectByFrame(raw_ptr<const kinematics::Frame> frame) const;

    /**
     * @brief Returns all owned scene objects (non-owning handles).
     *
     * @return The scene objects.
     */
    std::vector<raw_ptr<SceneObject>> sceneObjects() const;

    /**
     * @brief Returns the object that directly owns the given object's parent
     *        frame.
     *
     * @param object The object to inspect.
     * @return The parent object, or null for a top-level object or when the
     *         object is not in this workcell.
     */
    raw_ptr<SceneObject> parentOf(raw_ptr<const SceneObject> object) const;

    /**
     * @brief Returns the objects directly mounted on the given object.
     *
     * @param object The object to inspect.
     * @return The child objects.
     */
    std::vector<raw_ptr<SceneObject>> childrenOf(raw_ptr<const SceneObject> object) const;

    /**
     * @brief Returns every object whose base frame hangs transitively from the
     *        given object's base frame (the object itself excluded).
     *
     * @param object The object to inspect.
     * @return The descendant objects.
     */
    std::vector<raw_ptr<SceneObject>> descendantsOf(raw_ptr<const SceneObject> object) const;

    /**
     * @brief Checks whether ancestor is an ancestor of object in the
     *        coordinate-frame tree.
     *
     * @param ancestor The candidate ancestor.
     * @param object The candidate descendant.
     * @return true when object hangs from ancestor.
     */
    bool isAncestorOf(raw_ptr<const SceneObject> ancestor, raw_ptr<const SceneObject> object) const;

    /**
     * @brief Returns every frame of the workcell (world frame included), in
     *        breadth-first order.
     *
     * @return The frames.
     */
    std::vector<raw_ptr<kinematics::Frame>> allFrames() const;

    /**
     * @brief Re-parents an object onto a new frame.
     *
     * @param object The object to re-parent; must belong to this workcell.
     * @param new_parent_frame The new parent frame; must belong to this
     *                         workcell and must not create a cycle.
     * @return false on invalid arguments or when the frame belongs to the
     *         object itself / its descendants; true on success.
     */
    bool changeObjectParent(raw_ptr<SceneObject> object, raw_ptr<kinematics::Frame> new_parent_frame);

    /**
     * @brief Renames an object, keeping names unique within the workcell.
     *
     * @param object The object to rename; must belong to this workcell.
     * @param new_name The new name; must be non-empty and unused.
     * @return true when the object was renamed.
     */
    bool changeObjectName(raw_ptr<SceneObject> object, const String& new_name);

  private:
    String                                   name_;
    std::vector<std::unique_ptr<SceneObject>> objects_;
    std::unique_ptr<kinematics::Frame>        world_frame_;
};

VN_ROBOTICS_WORKCELL_NS_END
