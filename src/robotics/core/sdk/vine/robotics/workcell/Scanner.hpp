#pragma once

#include <vine/robotics/robot_core_global.hpp>

#include <map>
#include <memory>
#include <vector>

#include <vine/String.hpp>
#include <vine/math/Isometry3.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/robotics/kinematics/Frame.hpp>

#include "Device.hpp"

V_ROBOTICS_WORKCELL_NS_BEGIN

struct ScannerData;

/**
 * @brief A measurement device (laser scanner, camera rig, ...).
 *
 * A Scanner is a Device that carries several cameras and projectors. Cameras
 * and projectors are domain objects inside the scanner (its internal classes),
 * stored as device members like Link/Joint. They do not derive from Link or
 * Frame: they only reference an existing frame of the TF tree (frame) to query
 * poses, keeping the design pose and the calibrated pose separated on the
 * frame.
 *
 * The scanner is configured with init(std::unique_ptr<DeviceData>) exactly
 * like any other device; the definition data must be (or wrap) a ScannerData
 * carrying the cameras and projectors. Frames are bound by name after the
 * base device has built its frame tree.
 */
class V_ROBOTICS_CORE_API Scanner : public Device
{
  public:
    /**
     * @brief Camera intrinsics (value type, passed by value).
     */
    struct V_ROBOTICS_CORE_API CameraIntrinsics
    {
        // 分辨率，单位 px
        double width{}, height{};
        // 主点，单位 px
        double center_x{}, center_y{};
        // 焦距，单位 px
        double focus_x{}, focus_y{};
        // 裁剪面
        double near_{}, far_{};
    };

    /**
     * @brief Projector parameters (value type, passed by value).
     */
    struct V_ROBOTICS_CORE_API ProjectorParams
    {
        // 水平/垂直视场角，单位 rad
        double fov_w{}, fov_h{};
        // 测量景深范围，单位 mm
        double min_dof{}, max_dof{};
        // 测量距离，单位 mm
        double measure_dist{};
        // 裁剪值
        double crop_value{ 0.8 };
    };

    /**
     * @brief A scanner camera.
     *
     * Holds the design and calibrated intrinsics and references an existing
     * frame of the TF tree to query its pose. The design pose is the frame's
     * fixed transform relative to its parent.
     */
    struct V_ROBOTICS_CORE_API Camera
    {
        CameraIntrinsics design_intrinsics;     // 设备定义内参
        CameraIntrinsics calibrated_intrinsics; // 标定后内参

        // 坐标系名，对应 TF 树中的 frame（解析期暂存，绑定后 frame 非空）
        String frame_name;
        // 非拥有：TF 树中相机坐标系
        raw_ptr<kinematics::Frame> frame{};

        /**
         * @brief Returns the camera design pose (relative to its parent frame).
         *
         * @return The design fixed transform, identity when no frame is bound.
         */
        math::Isometry3d designTransform() const;
    };

    /**
     * @brief A scanner projector.
     *
     * Holds the design and calibrated parameters and references an existing
     * frame of the TF tree to query its pose.
     */
    struct V_ROBOTICS_CORE_API Projector
    {
        ProjectorParams design_params;      // 设备定义参数
        ProjectorParams calibrated_params;  // 标定后参数

        // 坐标系名，对应 TF 树中的 frame（解析期暂存，绑定后 frame 非空）
        String frame_name;
        // 非拥有：TF 树中投影仪坐标系
        raw_ptr<kinematics::Frame> frame{};

        /**
         * @brief Returns the projector design pose (relative to its parent frame).
         *
         * @return The design fixed transform, identity when no frame is bound.
         */
        math::Isometry3d designTransform() const;
    };

    /**
     * @brief Miscellaneous scanner parameters (no camera/projector payload).
     */
    struct V_ROBOTICS_CORE_API Parameters
    {
        std::map<String, String> values;
    };

  public:
    /**
     * @brief Constructs an empty scanner; configure it with init().
     */
    Scanner();

    /**
     * @brief Constructs a scanner with the given name.
     *
     * @param name The device name.
     */
    explicit Scanner(const String& name);

    /**
     * @brief Destroys the scanner.
     */
    ~Scanner() override;

  public:
    /**
     * @brief Configures the scanner from definition data (takes ownership).
     *
     * Builds the base device, then rebuilds the camera/projector views and
     * binds their frames by name.
     *
     * @param data The device definition data (must be or wrap a ScannerData).
     */
    void init(std::unique_ptr<DeviceData> data) override;

    /**
     * @brief Returns the cameras (non-owning views into the definition data).
     *
     * @return The cameras.
     */
    const std::vector<raw_ptr<Camera>>& cameras() const
    {
        return cameras_;
    }

    /**
     * @brief Returns the cameras as a mutable view.
     *
     * Camera contents may be edited, but cameras cannot be added/removed here.
     *
     * @return The cameras.
     */
    std::vector<raw_ptr<Camera>>& cameras()
    {
        return cameras_;
    }

    /**
     * @brief Returns the projectors (non-owning views into the definition data).
     *
     * @return The projectors.
     */
    const std::vector<raw_ptr<Projector>>& projectors() const
    {
        return projectors_;
    }

    /**
     * @brief Returns the projectors as a mutable view.
     *
     * Projector contents may be edited, but projectors cannot be added/removed
     * here.
     *
     * @return The projectors.
     */
    std::vector<raw_ptr<Projector>>& projectors()
    {
        return projectors_;
    }

    /**
     * @brief Returns the miscellaneous scanner parameters.
     *
     * @return The parameters.
     */
    const Parameters& parameters() const
    {
        return parameters_;
    }

    /**
     * @brief Sets the miscellaneous scanner parameters.
     *
     * @param params The new parameters.
     */
    void setParameters(const Parameters& params)
    {
        parameters_ = params;
    }

  protected:
    /**
     * @brief Casts/wraps definition data into ScannerData.
     *
     * @param data The definition data.
     * @return The scanner definition data.
     */
    static std::unique_ptr<ScannerData> asScannerData(std::unique_ptr<DeviceData> data);

    /**
     * @brief Builds the base device and rebuilds camera/projector views and
     *        frame bindings.
     *
     * @param data The scanner definition data.
     */
    void initScanner(std::unique_ptr<ScannerData> data);

  private:
    std::vector<raw_ptr<Camera>>    cameras_;
    std::vector<raw_ptr<Projector>> projectors_;
    Parameters                      parameters_;
};

/**
 * @brief Scanner definition data (immutable): extends DeviceData.
 *
 * Adds unique ownership of the cameras and projectors.
 */
struct V_ROBOTICS_CORE_API ScannerData : DeviceData
{
    std::vector<std::unique_ptr<Scanner::Camera>>    cameras;
    std::vector<std::unique_ptr<Scanner::Projector>> projectors;

    /**
     * @brief Creates a deep copy as a ScannerData.
     *
     * Cameras and projectors are deep-copied and their frame bindings are
     * cleared; they are rebound by name when a device is built from the clone.
     *
     * @return The cloned definition data.
     */
    std::unique_ptr<DeviceData> clone() const override;
};

V_ROBOTICS_WORKCELL_NS_END
