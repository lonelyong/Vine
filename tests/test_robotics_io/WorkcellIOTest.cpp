#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vine/geometry/Box.hpp>
#include <vine/robotics/io/DeviceIO.hpp>
#include <vine/robotics/io/WorkcellIO.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/robotics/workcell/RigidObject.hpp>
#include <vine/robotics/workcell/Scanner.hpp>
#include <vine/robotics/workcell/Workcell.hpp>

using namespace vn::robotics;
using namespace vn::robotics::kinematics;
using namespace vn::robotics::workcell;
using vn::robotics::io::DeviceIO;
using vn::robotics::io::WorkcellIO;

namespace
{

/**
 * @brief Creates a unique temporary directory that is removed on destruction.
 */
class TempDir
{
  public:
    TempDir()
    {
        static std::atomic<unsigned long long> counter{ 0 };
        std::error_code                        ec;
        path_ = std::filesystem::temp_directory_path(ec) /
                ("vine_workcell_io_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/**
 * @brief Writes UTF-8 text to a file.
 *
 * @param path The file path.
 * @param text The text to write.
 */
void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(out.good());
}

/**
 * @brief A 2-revolute-joint manipulator in the loose .vdev XML format.
 */
const char* const kRobotVdev = R"(<device name="UR" kind="Manipulator" version="1.0">
  <metadata name="UR" length_unit="mm"/>
  <link name="base_link"/>
  <link name="link1"/>
  <link name="link2"/>
  <joint name="joint1" type="revolute" parent="base_link" child="link1">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <dof type="revolute" axis="0 0 1" xyz="0 0 0" quat="0 0 0 1" lower="-3.14" upper="3.14" velocity="1.5" acceleration="2"/>
  </joint>
  <joint name="joint2" type="revolute" parent="link1" child="link2">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <dof type="revolute" axis="0 1 0" xyz="0 0 0" quat="0 0 0 1" lower="-2" upper="2" velocity="1" acceleration="1"/>
  </joint>
</device>
)";

/**
 * @brief A scanner with one revolute joint and one camera in the .vdev format.
 */
const char* const kScannerVdev = R"(<device name="CamRig" kind="Scanner" version="1.0">
  <metadata name="CamRig" length_unit="mm"/>
  <link name="base_link"/>
  <link name="link1"/>
  <joint name="joint1" type="revolute" parent="base_link" child="link1">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <dof type="revolute" axis="0 0 1" xyz="0 0 0" quat="0 0 0 1" lower="-3.14" upper="3.14" velocity="1.5" acceleration="2"/>
  </joint>
  <camera frame="joint1">
    <design width="640" height="480" center_x="320" center_y="240" focus_x="400" focus_y="400" near="1" far="1000"/>
  </camera>
</device>
)";

/**
 * @brief A workcell referencing the loose device files above.
 *
 * The camera is mounted on the robot's end frame (the child link of joint2)
 * and carries a home pose for the robot.
 */
const char* const kCellVcell = R"(<workcell name="demo" version="1.0">
  <obj name="robot1" type="device" file="devices/robot1.vdev" home="0.1 -0.5">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <children>
      <obj name="cam" type="device" file="devices/cam.vdev" parent_frame="joint2">
        <origin xyz="0 0 0" quat="0 0 0 1"/>
      </obj>
    </children>
  </obj>
  <obj name="table" type="rigid_object">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><box size="1 0.1 0.8"/></geometry>
    </visual>
  </obj>
</workcell>
)";

/**
 * @brief Builds a 2-revolute-joint motion device (used for a .vdevpkg entry).
 */
std::unique_ptr<MotionDevice> makeRobot()
{
    auto data = std::make_unique<MotionDeviceData>();
    data->metadata.name = u8"UR";
    data->kind          = DeviceKind::Manipulator;

    auto base_link = std::make_unique<Link>(u8"base_link");
    auto link1     = std::make_unique<Link>(u8"link1");
    auto link2     = std::make_unique<Link>(u8"link2");
    Link* const base_ptr  = base_link.get();
    Link* const link1_ptr = link1.get();
    Link* const link2_ptr = link2.get();
    data->links.push_back(std::move(base_link));
    data->links.push_back(std::move(link1));
    data->links.push_back(std::move(link2));

    auto joint1 = std::make_unique<Joint>(FrameType::RevoluteJoint);
    joint1->setName(u8"joint1");
    joint1->setParentLink(base_ptr);
    joint1->setChildLink(link1_ptr);
    joint1->setDofInfos({ DofInfo{} });
    data->joints.push_back(std::move(joint1));

    auto joint2 = std::make_unique<Joint>(FrameType::RevoluteJoint);
    joint2->setName(u8"joint2");
    joint2->setParentLink(link1_ptr);
    joint2->setChildLink(link2_ptr);
    joint2->setDofInfos({ DofInfo{} });
    data->joints.push_back(std::move(joint2));

    auto robot = std::make_unique<MotionDevice>();
    robot->init(std::move(data));
    return robot;
}

} // namespace

TEST(WorkcellIOTest, LoadXmlFromFolder)
{
    const TempDir temp;
    const auto    file = temp.path() / "cell.vcell";
    std::filesystem::create_directories(temp.path() / "devices");
    writeText(file, kCellVcell);
    writeText(temp.path() / "devices" / "robot1.vdev", kRobotVdev);
    writeText(temp.path() / "devices" / "cam.vdev", kScannerVdev);

    WorkcellIO io;
    auto loaded = io.loadXml(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name(), u8"demo");

    auto* const r = dynamic_cast<MotionDevice*>(loaded->findSceneObject(u8"robot1"));
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->isValid());
    EXPECT_EQ(r->joints().size(), 2u);
    // The home pose from the <obj home> attribute round-trips.
    EXPECT_EQ(r->homeQ().size(), 2u);
    EXPECT_DOUBLE_EQ(r->homeQ()[0], 0.1);
    EXPECT_DOUBLE_EQ(r->homeQ()[1], -0.5);

    auto* const c = dynamic_cast<Scanner*>(loaded->findSceneObject(u8"cam"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->cameras().size(), 1u);
    EXPECT_DOUBLE_EQ(c->cameras()[0]->design_intrinsics.width, 640.0);

    auto* const t = loaded->findSceneObject(u8"table");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind(), SceneObjectKind::RigidObject);

    // Hierarchy preserved: robot top-level, camera nested under it.
    EXPECT_EQ(loaded->parentOf(r), nullptr);
    EXPECT_EQ(loaded->parentOf(c), static_cast<SceneObject*>(r));
    EXPECT_EQ(loaded->childrenOf(r).size(), 1u);
}

TEST(WorkcellIOTest, ParentFrameToDeviceEnd)
{
    const TempDir temp;
    const auto    file = temp.path() / "cell.vcell";
    std::filesystem::create_directories(temp.path() / "devices");
    writeText(file, kCellVcell);
    writeText(temp.path() / "devices" / "robot1.vdev", kRobotVdev);
    writeText(temp.path() / "devices" / "cam.vdev", kScannerVdev);

    WorkcellIO io;
    auto loaded = io.loadXml(file);

    auto* const r = dynamic_cast<MotionDevice*>(loaded->findSceneObject(u8"robot1"));
    ASSERT_NE(r, nullptr);
    auto* const c = dynamic_cast<Scanner*>(loaded->findSceneObject(u8"cam"));
    ASSERT_NE(c, nullptr);

    // The camera's base frame hangs from the robot's end (terminal link) frame.
    const auto parent_frame = c->baseFrame()->parent();
    ASSERT_NE(parent_frame, nullptr);
    EXPECT_EQ(parent_frame, r->getEnd(0));
    EXPECT_NE(parent_frame->name().empty(), true);
}

TEST(WorkcellIOTest, LoadXmlFromFolderWithDevicePkg)
{
    const TempDir temp;
    const auto    file = temp.path() / "cell.vcell";
    std::filesystem::create_directories(temp.path() / "devices");

    // A packaged device file sits in the folder; the folder loader dispatches
    // by extension and opens the nested package in memory.
    auto robot = makeRobot();
    robot->setName(u8"robot2");
    DeviceIO device_io;
    device_io.savePkg(*robot, temp.path() / "devices" / "robot2.vdevpkg");

    const std::string cell_xml =
        "<workcell name=\"pkg\" version=\"1.0\">"
        "  <obj name=\"robot2\" type=\"device\" file=\"devices/robot2.vdevpkg\">"
        "    <origin xyz=\"0 0 0\" quat=\"0 0 0 1\"/>"
        "  </obj>"
        "</workcell>";
    writeText(file, cell_xml);

    WorkcellIO io;
    auto loaded = io.loadXml(file);
    ASSERT_NE(loaded, nullptr);
    auto* const r = dynamic_cast<MotionDevice*>(loaded->findSceneObject(u8"robot2"));
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->isValid());
    EXPECT_EQ(r->joints().size(), 2u);
    EXPECT_EQ(r->deviceKind(), DeviceKind::Manipulator);
}
