#include <vine/robotics/workcell/MotionDevice.hpp>

V_ROBOTICS_WORKCELL_NS_BEGIN

MotionDevice::MotionDevice() = default;

MotionDevice::MotionDevice(const String& name)
  : Device(name)
{}

MotionDevice::~MotionDevice() = default;

std::unique_ptr<DeviceData> MotionDeviceData::clone() const
{
    auto out = std::make_unique<MotionDeviceData>();
    out->copyBaseFrom(*this);
    return out;
}

std::unique_ptr<MotionDeviceData> MotionDevice::asMotionData(std::unique_ptr<DeviceData> data)
{
    // 定义数据向下转型为 MotionDeviceData; 若为普通 DeviceData 则包装补全
    auto* const raw = data.release();
    std::unique_ptr<MotionDeviceData> motion_data(dynamic_cast<MotionDeviceData*>(raw));
    if (!motion_data) {
        motion_data = std::make_unique<MotionDeviceData>();
        if (raw) {
            motion_data->metadata = std::move(raw->metadata);
            motion_data->links    = std::move(raw->links);
            motion_data->joints   = std::move(raw->joints);
            delete raw;
        }
    }
    return motion_data;
}

void MotionDevice::init(std::unique_ptr<DeviceData> data)
{
    initMotion(asMotionData(std::move(data)));
}

void MotionDevice::initMotion(std::unique_ptr<MotionDeviceData> data)
{
    // 基础构建(连杆/关节/末端视图)
    initDevice(std::move(data));

    // 基础构建失败(如无关节或无末端)则清空运动数据缓存, 使空设备状态一致
    if (!isValid() || !getEnd(0)) {
        kinematics_.reset();
        home_q_             = kinematics::Q{};
        lower_bounds_       = kinematics::Q{};
        upper_bounds_       = kinematics::Q{};
        velocity_limits_    = kinematics::Q{};
        acceleration_limits_ = kinematics::Q{};
        return;
    }

    // 目前所有运动设备都按串联链处理: 由基座坐标系到第一个末端建立运动学模型
    kinematics_ = std::make_unique<kinematics::SerialKinematics>(baseFrame(), getEnd(0));

    // 从运动学推导运动范围与速度/加速度上限(逐自由度)
    lower_bounds_        = kinematics_->lowerBounds();
    upper_bounds_        = kinematics_->upperBounds();
    velocity_limits_     = kinematics_->maxVelocityLimits();
    acceleration_limits_ = kinematics_->maxAccelerationLimits();

    // 默认 Home 位: 取最靠近 0 且在运动范围内的关节状态
    home_q_ = kinematics::Q{};
    for (std::size_t i = 0; i < lower_bounds_.size(); ++i) {
        double value = 0.0;
        if (value < lower_bounds_[i] || value > upper_bounds_[i]) {
            value = (lower_bounds_[i] + upper_bounds_[i]) * 0.5;
        }
        home_q_.append(value);
    }
}

V_ROBOTICS_WORKCELL_NS_END
