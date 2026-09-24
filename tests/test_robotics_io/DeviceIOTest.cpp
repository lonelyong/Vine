#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vine/geometry/Box.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/robotics/io/DeviceIO.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/robotics/workcell/Scanner.hpp>

using namespace vn::robotics;
using namespace vn::robotics::kinematics;
using namespace vn::robotics::workcell;
using vn::robotics::io::DeviceIO;

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
                ("vine_robotics_io_" + std::to_string(counter.fetch_add(1)));
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
 * @brief Writes float values as a raw little-endian binary file.
 *
 * @param path The file path.
 * @param values The values to write.
 */
void writeFloats(const std::filesystem::path& path, const std::vector<float>& values)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(float)));
    ASSERT_TRUE(out.good());
}

/**
 * @brief A 2-revolute-joint manipulator in the loose .vdev XML format.
 */
const char* const kRobotVdev = R"(<device name="UR" kind="Manipulator" version="1.0">
  <metadata name="UR" model="UR10" length_unit="mm" iksolver="Pieper"/>
  <link name="base_link">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><box size="0.2 0.3 0.4"/></geometry>
    </visual>
    <collision>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><sphere radius="0.15"/></geometry>
    </collision>
  </link>
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
 * @brief A .vdev referencing a loose mesh bin under geoms/.
 */
const char* const kRobotVdevMesh = R"(<device name="UR" kind="Manipulator" version="1.0">
  <metadata name="UR" length_unit="mm"/>
  <link name="base_link">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry>
        <triangle_mesh vertex_count="3" triangle_count="1" positions="geoms/mesh0.positions.bin"/>
      </geometry>
    </visual>
  </link>
  <link name="link1"/>
  <joint name="joint1" type="revolute" parent="base_link" child="link1">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <dof type="revolute" axis="0 0 1" xyz="0 0 0" quat="0 0 0 1" lower="-3.14" upper="3.14" velocity="1.5" acceleration="2"/>
  </joint>
</device>
)";

/**
 * @brief A .vdev with a named device material library and a referencing visual.
 */
const char* const kRobotVdevMaterials = R"(<device name="UR" kind="Manipulator" version="1.0">
  <metadata name="UR" length_unit="mm"/>
  <materials>
    <material name="orange" color="1 0.6666667 0 1"/>
  </materials>
  <link name="base_link">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry><box size="0.2 0.3 0.4"/></geometry>
      <material name="orange"/>
    </visual>
  </link>
  <link name="link1"/>
  <joint name="joint1" type="revolute" parent="base_link" child="link1">
    <origin xyz="0 0 0" quat="0 0 0 1"/>
    <dof type="revolute" axis="0 0 1" xyz="0 0 0" quat="0 0 0 1" lower="-3.14" upper="3.14" velocity="1.5" acceleration="2"/>
  </joint>
</device>
)";

} // namespace

TEST(DeviceIOTest, LoadXmlFromFolder)
{
    const TempDir temp;
    const auto    file = temp.path() / "robot.vdev";
    writeText(file, kRobotVdev);

    DeviceIO io;
    auto loaded = io.loadXml(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->filePath(), file);

    auto* const r = dynamic_cast<MotionDevice*>(loaded.get());
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->isValid());
    EXPECT_EQ(r->deviceKind(), DeviceKind::Manipulator);
    EXPECT_EQ(r->name(), u8"UR");
    EXPECT_EQ(r->links().size(), 3u);
    EXPECT_EQ(r->joints().size(), 2u);
    ASSERT_NE(r->kinematics(), nullptr);
    EXPECT_EQ(r->kinematics()->ikSolverType(), IKSolverType::Pieper);
    EXPECT_EQ(r->lowerBounds().size(), 2u);
    EXPECT_DOUBLE_EQ(r->lowerBounds()[0], -3.14);
    EXPECT_DOUBLE_EQ(r->upperBounds()[1], 2.0);

    // The base link visual / collision load from the folder.
    const auto& body = r->links().front()->body();
    ASSERT_EQ(body.visuals().size(), 1u);
    ASSERT_NE(body.visuals()[0].shape(), nullptr);
    EXPECT_EQ(body.visuals()[0].shape()->shapeType(), vn::geometry::ShapeType::Box);
    EXPECT_DOUBLE_EQ(static_cast<vn::geometry::Box*>(body.visuals()[0].shape().get())->width(), 0.2);
    ASSERT_EQ(body.collisions().size(), 1u);
    EXPECT_EQ(body.collisions()[0].shape()->shapeType(), vn::geometry::ShapeType::Sphere);
}

TEST(DeviceIOTest, LoadXmlFromFolderWithMesh)
{
    const TempDir temp;
    const auto    file = temp.path() / "robot.vdev";
    writeText(file, kRobotVdevMesh);
    std::filesystem::create_directories(temp.path() / "geoms");
    writeFloats(temp.path() / "geoms" / "mesh0.positions.bin",
                { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f });

    DeviceIO io;
    auto loaded = io.loadXml(file);
    ASSERT_NE(loaded, nullptr);
    auto* const r = dynamic_cast<MotionDevice*>(loaded.get());
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->baseLink()->body().visuals().size(), 1u);
    const auto* const m = dynamic_cast<const vn::geometry::TriangleMesh*>(
        r->baseLink()->body().visuals()[0].shape().get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->vertexCount(), 3u);
    EXPECT_EQ(m->triangleCount(), 1u);
    EXPECT_FLOAT_EQ(m->positions()[2].y, 1.0f);
}

TEST(DeviceIOTest, MaterialLibraryRoundTrip)
{
    const TempDir temp;
    const auto    file = temp.path() / "robot.vdev";
    writeText(file, kRobotVdevMaterials);

    DeviceIO io;
    auto loaded = io.loadXml(file);
    ASSERT_NE(loaded, nullptr);
    auto* const r = dynamic_cast<MotionDevice*>(loaded.get());
    ASSERT_NE(r, nullptr);

    // The named library lands on the device data.
    ASSERT_EQ(r->data()->materials.size(), 1u);
    EXPECT_EQ(r->data()->materials[0].name, u8"orange");
    const auto* const lib_color =
        dynamic_cast<const vn::geometry::ColorMaterial*>(r->data()->materials[0].material.get());
    ASSERT_NE(lib_color, nullptr);
    EXPECT_FLOAT_EQ(lib_color->color().r, 1.0f);

    // The base-link visual references the library material by name.
    ASSERT_EQ(r->baseLink()->body().visuals().size(), 1u);
    EXPECT_EQ(r->baseLink()->body().visuals()[0].materialName(), u8"orange");
    const auto* const vis_color = dynamic_cast<const vn::geometry::ColorMaterial*>(
        r->baseLink()->body().visuals()[0].material().get());
    ASSERT_NE(vis_color, nullptr);
    EXPECT_FLOAT_EQ(vis_color->color().r, 1.0f);
    EXPECT_FLOAT_EQ(vis_color->color().b, 0.0f);

    // Round-trips through a package preserving library + reference.
    const auto pkg = temp.path() / "robot.vdevpkg";
    io.savePkg(*loaded, pkg);
    auto reloaded = io.loadPkg(pkg);
    ASSERT_NE(reloaded, nullptr);
    auto* const rr = dynamic_cast<MotionDevice*>(reloaded.get());
    ASSERT_NE(rr, nullptr);
    ASSERT_EQ(rr->data()->materials.size(), 1u);
    EXPECT_EQ(rr->data()->materials[0].name, u8"orange");
    ASSERT_EQ(rr->baseLink()->body().visuals().size(), 1u);
    EXPECT_EQ(rr->baseLink()->body().visuals()[0].materialName(), u8"orange");
}

TEST(DeviceIOTest, MissingLinksThrows)
{
    const TempDir temp;
    const auto    file = temp.path() / "bad.vdev";

    DeviceIO io;
    writeText(file, R"(<device name="x" kind="Manipulator" version="1.0"><metadata/></device>)");
    EXPECT_THROW(io.loadXml(file), std::runtime_error);

    writeText(file, R"(<notdevice/>)");
    EXPECT_THROW(io.loadXml(file), std::runtime_error);

    writeText(file, R"(<device name="x" kind="Manipulator" version="9.9"><link name="a"/></device>)");
    EXPECT_THROW(io.loadXml(file), std::runtime_error);

    EXPECT_THROW(io.loadXml(temp.path() / "missing.vdev"), std::runtime_error);
}
