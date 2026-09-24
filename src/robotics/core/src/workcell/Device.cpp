#include <vine/robotics/workcell/Device.hpp>

#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

VN_ROBOTICS_WORKCELL_NS_BEGIN

const String& Device::modelName() const
{
    static const String s_empty;
    return owned_data_ ? owned_data_->metadata.model : s_empty;
}

const DeviceMetadata& Device::metadata() const
{
    static const DeviceMetadata s_empty;
    return owned_data_ ? owned_data_->metadata : s_empty;
}

void Device::init(std::unique_ptr<DeviceData> data)
{
    initDevice(std::move(data));
}

void Device::initDevice(std::unique_ptr<DeviceData> data)
{
    // 释放旧设备对 link 的归属(重新初始化时)
    for (auto* const link : links_) {
        link->setDevice(nullptr);
    }

    owned_data_ = std::move(data);

    // 定义数据可指定设备类型(如加载出的 Manipulator/ExternalAxis), 未指定时保留子类默认
    if (owned_data_ && owned_data_->kind != DeviceKind::Other) {
        device_kind_ = owned_data_->kind;
    }

    links_.clear();
    joints_.clear();
    ends_.clear();
    is_valid_ = false;

    // 清空所有关节/末端帧，保留 base_frame
    frames_.clear();
    frames_.push_back(base_frame_.get());

    if (!owned_data_) {
        return;
    }

    const auto& def_links  = owned_data_->links;
    const auto& def_joints = owned_data_->joints;

    // ---- 构建连杆图：每个连杆记录入边(指向它的关节)与出边(它指向的关节) ----
    struct Node
    {
        Link* link{ nullptr };
        std::vector<Joint*> incoming;
    };

    std::unordered_map<Link*, Node>               nodes;
    std::unordered_map<Link*, std::vector<Joint*>> outgoing;

    for (const auto& link : def_links) {
        nodes[link.get()].link = link.get();
    }

    for (const auto& joint : def_joints) {
        Link* const plink = joint->parentLink();
        Link* const clink = joint->childLink();
        if (!plink || !clink) {
            throw std::logic_error("Device::initDevice, a joint references a null link.");
        }
        if (!nodes.contains(plink) || !nodes.contains(clink)) {
            throw std::logic_error("Device::initDevice, a joint references an undeclared link.");
        }
        nodes[clink].incoming.push_back(joint.get());
        outgoing[plink].push_back(joint.get());
    }

    // ---- 查找基座连杆：没有被任何关节指向的连杆 ----
    Link* base_link = nullptr;
    for (const auto& [link, node] : nodes) {
        if (node.incoming.empty()) {
            if (base_link) {
                throw std::logic_error("Device::initDevice, multiple base links found.");
            }
            base_link = link;
        } else if (node.incoming.size() > 1) {
            throw std::logic_error("Device::initDevice, a link has multiple parent joints.");
        }
    }
    if (!base_link) {
        throw std::logic_error("Device::initDevice, no base link found.");
    }

    // 空设备(无关节): 全部连杆直接挂到 base_frame
    if (def_joints.empty()) {
        for (const auto& link : def_links) {
            link->setDevice(this);
            link->setParentFrame(base_frame_.get());
            links_.push_back(link.get());
        }
        is_valid_ = true;
        return;
    }

    // ---- BFS 构建设备的连杆/关节链 ----
    std::queue<Link*> link_queue;
    link_queue.push(base_link);

    std::unordered_set<Link*> visited;
    visited.insert(base_link);

    std::vector<Link*> ordered_links;
    std::vector<Joint*> ordered_joints;

    while (!link_queue.empty()) {
        Link* const link = link_queue.front();
        link_queue.pop();

        ordered_links.push_back(link);
        link->setDevice(this);

        for (Joint* const joint : outgoing[link]) {
            Link* const clink = joint->childLink();
            if (visited.contains(clink)) {
                throw std::logic_error("Device::initDevice, cycle detected in the kinematic chain.");
            }
            visited.insert(clink);
            // 关节坐标系即子连杆的基坐标系
            clink->setParentFrame(joint);
            ordered_joints.push_back(joint);
            link_queue.push(clink);
        }
    }

    if (visited.size() != nodes.size()) {
        throw std::logic_error("Device::initDevice, the kinematic chain is not connected.");
    }

    // ---- 构建坐标系树：关节挂到父连杆的基坐标系下 ----
    base_link->setParentFrame(base_frame_.get());
    for (Joint* const joint : ordered_joints) {
        auto* const parent_link_frame = joint->parentLink()->baseFrame();
        parent_link_frame->addChild(joint);
    }

    // ---- 末端：没有出边(子连杆)的连杆的基坐标系 ----
    for (Link* const link : ordered_links) {
        if (outgoing[link].empty()) {
            ends_.push_back(link->baseFrame());
        }
    }

    // ---- 注册坐标系：base + 关节 ----
    frames_.reserve(frames_.size() + ordered_joints.size());
    for (Joint* const joint : ordered_joints) {
        frames_.push_back(joint);
    }

    links_  = std::move(ordered_links);
    joints_ = std::move(ordered_joints);
    is_valid_ = true;
}

kinematics::Q Device::getQ(const kinematics::State& state) const
{
    kinematics::Q q;
    for (const auto* const joint : joints_) {
        q.append(state.qstate(base_frame_.get()).getQ(joint));
    }
    return q;
}

void Device::setQ(const kinematics::Q& q, kinematics::State& state)
{
    if (!is_valid_) {
        throw std::runtime_error("Device::setQ, the device is invalid.");
    }
    std::size_t offset = 0;
    for (auto* const joint : joints_) {
        const std::size_t dof = joint->dofInfos().size();
        if (offset + dof > q.size()) {
            throw std::invalid_argument("Device::setQ, q.size() does not match the device dof.");
        }
        state.qstate(base_frame_.get()).setQ(joint, q.subQ(offset, dof));
        offset += dof;
    }
    if (offset != q.size()) {
        throw std::invalid_argument("Device::setQ, q.size() does not match the device dof.");
    }
}

void DeviceData::copyBaseFrom(const DeviceData& other)
{
    metadata  = other.metadata;
    kind      = other.kind;
    materials = other.materials;

    // 深拷贝连杆并建立 旧->新 映射
    std::unordered_map<const Link*, Link*> link_map;
    links.reserve(other.links.size());
    for (const auto& link : other.links) {
        auto clone = std::make_unique<Link>(link->name());
        clone->copyFrom(*link);
        link_map[link.get()] = clone.get();
        links.push_back(std::move(clone));
    }

    // 深拷贝关节并重绑父/子连杆
    joints.reserve(other.joints.size());
    for (const auto& joint : other.joints) {
        auto clone = joint->clone();
        if (joint->parentLink()) {
            clone->setParentLink(link_map.at(joint->parentLink()));
        }
        if (joint->childLink()) {
            clone->setChildLink(link_map.at(joint->childLink()));
        }
        joints.push_back(std::move(clone));
    }
}

std::unique_ptr<DeviceData> DeviceData::clone() const
{
    auto out = std::make_unique<DeviceData>();
    out->copyBaseFrom(*this);
    return out;
}

VN_ROBOTICS_WORKCELL_NS_END
