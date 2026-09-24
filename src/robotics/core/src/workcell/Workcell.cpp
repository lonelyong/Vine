#include <vine/robotics/workcell/Workcell.hpp>

#include <algorithm>
#include <queue>

VN_ROBOTICS_WORKCELL_NS_BEGIN

Workcell::Workcell()
  : world_frame_(std::make_unique<kinematics::Frame>())
{
    world_frame_->setName(u8"world");
}

Workcell::~Workcell()
{
    // 解除所有对象对工作站的归属
    for (auto& object : objects_) {
        object->workcell_ = nullptr;
    }
}

raw_ptr<SceneObject> Workcell::addSceneObject(std::unique_ptr<SceneObject> object,
                                              raw_ptr<kinematics::Frame>   parent_frame)
{
    if (!object) {
        return nullptr;
    }
    // 已被其它工作站持有
    if (object->workcell_) {
        return nullptr;
    }
    // 名称不能重复(允许空名)
    if (!object->name().empty() && findSceneObject(object->name())) {
        return nullptr;
    }

    const auto attach = parent_frame ? parent_frame : world_frame_.get();

    // 防止出现坐标系环: 不能挂到自身或其子树中的坐标系
    if (attach == object->baseFrame() || object->baseFrame()->isAncestorOf(attach)) {
        return nullptr;
    }

    const auto result = object.get();
    object->workcell_ = this;
    attach->addChild(object->baseFrame());
    objects_.push_back(std::move(object));
    return result;
}

bool Workcell::removeSceneObject(const String& name)
{
    const auto it = std::find_if(objects_.begin(), objects_.end(), [&name](const auto& object)
    {
        return object->name() == name;
    });
    if (it == objects_.end()) {
        return false;
    }

    const auto removed = it->get();

    // 将挂载在 removed 子树上的对象重新挂到 removed 的父坐标系(或 world)
    const auto reparent_frame = removed->baseFrame()->parent()
                                    ? removed->baseFrame()->parent()
                                    : world_frame_.get();
    for (const auto& object : objects_) {
        if (object.get() == removed) {
            continue;
        }
        if (removed->baseFrame()->isAncestorOf(object->baseFrame())) {
            const auto old_parent = object->baseFrame()->parent();
            if (old_parent) {
                old_parent->removeChild(object->baseFrame());
            }
            reparent_frame->addChild(object->baseFrame());
        }
    }

    removed->workcell_ = nullptr;
    if (auto* const old_parent = removed->baseFrame()->parent()) {
        old_parent->removeChild(removed->baseFrame());
    }
    objects_.erase(it);
    return true;
}

raw_ptr<SceneObject> Workcell::findSceneObject(const String& name) const
{
    const auto it = std::find_if(objects_.begin(), objects_.end(), [&name](const auto& object)
    {
        return object->name() == name;
    });
    return it == objects_.end() ? nullptr : it->get();
}

raw_ptr<SceneObject> Workcell::findSceneObjectByFrame(raw_ptr<const kinematics::Frame> frame) const
{
    if (!frame) {
        return nullptr;
    }
    for (const auto& object : objects_) {
        const auto& frames = object->frames();
        if (std::find(frames.begin(), frames.end(), frame) != frames.end()) {
            return object.get();
        }
    }
    return nullptr;
}

std::vector<raw_ptr<SceneObject>> Workcell::sceneObjects() const
{
    std::vector<raw_ptr<SceneObject>> result;
    result.reserve(objects_.size());
    for (const auto& object : objects_) {
        result.push_back(object.get());
    }
    return result;
}

raw_ptr<SceneObject> Workcell::parentOf(raw_ptr<const SceneObject> object) const
{
    if (!object) {
        return nullptr;
    }
    return findSceneObjectByFrame(object->baseFrame()->parent());
}

std::vector<raw_ptr<SceneObject>> Workcell::childrenOf(raw_ptr<const SceneObject> object) const
{
    std::vector<raw_ptr<SceneObject>> children;
    if (!object) {
        return children;
    }
    for (const auto& candidate : objects_) {
        if (candidate.get() != object && parentOf(candidate.get()) == object) {
            children.push_back(candidate.get());
        }
    }
    return children;
}

std::vector<raw_ptr<SceneObject>> Workcell::descendantsOf(raw_ptr<const SceneObject> object) const
{
    std::vector<raw_ptr<SceneObject>> descendants;
    if (!object) {
        return descendants;
    }
    for (const auto& candidate : objects_) {
        if (candidate.get() != object
            && object->baseFrame()->isAncestorOf(candidate->baseFrame())) {
            descendants.push_back(candidate.get());
        }
    }
    return descendants;
}

bool Workcell::isAncestorOf(raw_ptr<const SceneObject> ancestor, raw_ptr<const SceneObject> object) const
{
    return ancestor && object && ancestor != object
           && ancestor->baseFrame()->isAncestorOf(object->baseFrame());
}

std::vector<raw_ptr<kinematics::Frame>> Workcell::allFrames() const
{
    std::vector<raw_ptr<kinematics::Frame>> frames;
    std::queue<raw_ptr<kinematics::Frame>> queue;
    queue.push(world_frame_.get());

    while (!queue.empty()) {
        const auto frame = queue.front();
        queue.pop();
        frames.push_back(frame);

        for (std::size_t i = 0; i < frame->childCount(); ++i) {
            queue.push(frame->childAt(i));
        }
    }
    return frames;
}

bool Workcell::changeObjectParent(raw_ptr<SceneObject> object, raw_ptr<kinematics::Frame> new_parent_frame)
{
    if (!object || !new_parent_frame) {
        return false;
    }
    if (object->workcell_ != this) {
        return false;
    }
    // 新父坐标系必须属于本工作站(或为 world)
    if (new_parent_frame != world_frame_.get() && !findSceneObjectByFrame(new_parent_frame)) {
        return false;
    }
    // 父级未变化
    if (new_parent_frame == object->baseFrame()->parent()) {
        return true;
    }
    // 不能挂到自身或其子树中的坐标系(防止环)
    if (new_parent_frame == object->baseFrame()
        || object->baseFrame()->isAncestorOf(new_parent_frame)) {
        return false;
    }

    const auto old_parent = object->baseFrame()->parent();
    if (old_parent) {
        old_parent->removeChild(object->baseFrame());
    }
    new_parent_frame->addChild(object->baseFrame());
    return true;
}

bool Workcell::changeObjectName(raw_ptr<SceneObject> object, const String& new_name)
{
    if (!object || object->workcell_ != this) {
        return false;
    }
    if (new_name.empty()) {
        return false;
    }
    if (const auto existing = findSceneObject(new_name); existing && existing != object) {
        return false;
    }
    object->setName(new_name);
    return true;
}

VN_ROBOTICS_WORKCELL_NS_END
