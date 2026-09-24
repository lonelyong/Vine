#include <vine/robotics/workcell/SceneObject.hpp>

#include <vine/robotics/workcell/Workcell.hpp>

VN_ROBOTICS_WORKCELL_NS_BEGIN

raw_ptr<SceneObject> SceneObject::parentObject() const
{
    if (!workcell_) {
        return nullptr;
    }
    const auto parent_frame = base_frame_->parent();
    if (!parent_frame) {
        return nullptr;
    }
    return workcell_->findSceneObjectByFrame(parent_frame);
}

std::vector<raw_ptr<SceneObject>> SceneObject::childObjects() const
{
    std::vector<raw_ptr<SceneObject>> children;
    if (!workcell_) {
        return children;
    }
    for (const auto object : workcell_->sceneObjects()) {
        if (object != this && object->parentObject() == this) {
            children.push_back(object);
        }
    }
    return children;
}

bool SceneObject::isChildOf(raw_ptr<const SceneObject> parent) const
{
    return parent && parentObject() == parent;
}

bool SceneObject::isDescendantOf(raw_ptr<const SceneObject> ancestor) const
{
    return ancestor && ancestor->baseFrame()->isAncestorOf(base_frame_.get());
}

VN_ROBOTICS_WORKCELL_NS_END
