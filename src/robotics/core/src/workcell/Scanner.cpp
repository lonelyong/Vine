#include <vine/robotics/workcell/Scanner.hpp>

V_ROBOTICS_WORKCELL_NS_BEGIN

namespace
{

/**
 * @brief Finds a frame by name in the given object's frame registry.
 */
kinematics::Frame* findFrameByName(const SceneObject& object, const String& name)
{
    for (auto* const frame : object.frames()) {
        if (frame->name() == name) {
            return frame;
        }
    }
    return nullptr;
}

} // namespace

math::Isometry3d Scanner::Camera::designTransform() const
{
    return frame ? frame->fixedTransform() : math::Isometry3d{};
}

math::Isometry3d Scanner::Projector::designTransform() const
{
    return frame ? frame->fixedTransform() : math::Isometry3d{};
}

Scanner::Scanner()
{
    setDeviceKind(DeviceKind::Scanner);
}

Scanner::Scanner(const String& name)
  : Device(name)
{
    setDeviceKind(DeviceKind::Scanner);
}

Scanner::~Scanner() = default;

std::unique_ptr<DeviceData> ScannerData::clone() const
{
    auto out = std::make_unique<ScannerData>();
    out->copyBaseFrom(*this);
    for (const auto& cam : cameras) {
        auto c = std::make_unique<Scanner::Camera>(*cam);
        c->frame = nullptr; // frame 绑定由设备构建时按 frame_name 重建
        out->cameras.push_back(std::move(c));
    }
    for (const auto& proj : projectors) {
        auto p = std::make_unique<Scanner::Projector>(*proj);
        p->frame = nullptr;
        out->projectors.push_back(std::move(p));
    }
    return out;
}

std::unique_ptr<ScannerData> Scanner::asScannerData(std::unique_ptr<DeviceData> data)
{
    // 定义数据向下转型为 ScannerData; 若为普通 DeviceData 则包装补全
    auto* const raw = data.release();
    std::unique_ptr<ScannerData> sdata(dynamic_cast<ScannerData*>(raw));
    if (!sdata) {
        sdata = std::make_unique<ScannerData>();
        if (raw) {
            sdata->metadata = std::move(raw->metadata);
            sdata->links    = std::move(raw->links);
            sdata->joints   = std::move(raw->joints);
            delete raw;
        }
    }
    return sdata;
}

void Scanner::init(std::unique_ptr<DeviceData> data)
{
    initScanner(asScannerData(std::move(data)));
}

void Scanner::initScanner(std::unique_ptr<ScannerData> data)
{
    // 基础构建(连杆/关节/末端视图), 设备不可变, 仅此一次
    initDevice(std::move(data));

    // 相机/投影仪视图与 frame 绑定(基础构建完成后 TF 树已就绪)
    cameras_.clear();
    projectors_.clear();
    if (!this->data()) {
        return;
    }
    const auto* const sdata = static_cast<const ScannerData*>(this->data());
    for (const auto& cam : sdata->cameras) {
        cameras_.push_back(cam.get());
    }
    for (const auto& proj : sdata->projectors) {
        projectors_.push_back(proj.get());
    }

    // 按 frame_name 重新绑定 frame
    for (auto* const cam : cameras_) {
        if (!cam->frame_name.empty()) {
            cam->frame = findFrameByName(*this, cam->frame_name);
        }
    }
    for (auto* const proj : projectors_) {
        if (!proj->frame_name.empty()) {
            proj->frame = findFrameByName(*this, proj->frame_name);
        }
    }
}

V_ROBOTICS_WORKCELL_NS_END
