#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <vine/geometry/Box.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/io/ZipArchive.hpp>
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
 * @brief Writes one triangle as a binary STL file.
 *
 * The facet normal is written as it is given rather than worked out from the vertices, so a reader that keeps the file's
 * own normal can be told apart from one that recomputes it.
 *
 * @param path The file path.
 * @param normal The facet normal to store.
 * @param a First vertex of the triangle.
 * @param b Second vertex.
 * @param c Third vertex.
 */
void writeBinaryStl(const std::filesystem::path& path, const std::array<float, 3>& normal, const std::array<float, 3>& a,
                    const std::array<float, 3>& b, const std::array<float, 3>& c)
{
    static constexpr char kHeader[] = "binary STL written by the test";
    std::array<char, 84>    prefix{};
    std::memcpy(prefix.data(), kHeader, sizeof(kHeader) - 1);
    const std::uint32_t count = 1;
    std::memcpy(prefix.data() + 80, &count, sizeof(count));

    std::ofstream out(path, std::ios::binary);
    out.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    for (const auto* const values : { &normal, &a, &b, &c }) {
        out.write(reinterpret_cast<const char*>(values->data()), static_cast<std::streamsize>(sizeof(float) * 3));
    }
    const std::uint16_t attribute = 0;
    out.write(reinterpret_cast<const char*>(&attribute), static_cast<std::streamsize>(sizeof(attribute)));
    ASSERT_TRUE(out.good());
}

/**
 * @brief Locates one of the shared test assets.
 *
 * @param relative Path of the asset below the staged test data folder.
 * @return The absolute path of the asset.
 */
std::filesystem::path assetPath(const std::string& relative)
{
    return std::filesystem::path(VINE_TEST_DATA_DIR) / relative;
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
 * @brief A .vdev referencing a loose STL file under geoms/.
 */
const char* const kRobotVdevMesh = R"(<device name="UR" kind="Manipulator" version="1.0">
  <metadata name="UR" length_unit="mm"/>
  <link name="base_link">
    <visual>
      <origin xyz="0 0 0" quat="0 0 0 1"/>
      <geometry>
        <triangle_mesh triangle_count="1" geometry="geoms/mesh0.stl"/>
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
    writeBinaryStl(temp.path() / "geoms" / "mesh0.stl", { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                   { 0.0f, 1.0f, 0.0f });

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
    // The normal the file states is the one that arrives: an STL normal is not recomputed from the winding.
    ASSERT_EQ(m->normals().size(), 3u);
    EXPECT_FLOAT_EQ(m->normals()[0].z, -1.0f);
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

TEST(DeviceIOTest, LoadsCommittedPackage)
{
    // The asset holds one binary STL file per mesh; a single refused entry would show up as a missing shape below.
    DeviceIO   io;
    const auto dev = io.loadPkg(assetPath("robots/irb_1600_10_145.vdev"));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->name(), u8"机械臂1");

    std::size_t meshes    = 0;
    std::size_t vertices  = 0;
    std::size_t triangles = 0;
    for (auto* const link : dev->links()) {
        for (const auto& visual : link->body().visuals()) {
            const auto* const mesh = dynamic_cast<const vn::geometry::Mesh*>(visual.shape().get());
            if (mesh != nullptr) {
                ++meshes;
                vertices += mesh->vertexCount();
                triangles += static_cast<const vn::geometry::TriangleMesh*>(mesh)->triangleCount();
            }
        }
        for (const auto& collision : link->body().collisions()) {
            const auto* const mesh = dynamic_cast<const vn::geometry::Mesh*>(collision.shape().get());
            if (mesh != nullptr) {
                ++meshes;
                vertices += mesh->vertexCount();
                triangles += static_cast<const vn::geometry::TriangleMesh*>(mesh)->triangleCount();
            }
        }
    }
    // Seven links carry a visual and a collision each: fourteen mesh references over thirteen files. The files hold
    // 98674 triangles (the count the asset has always had), and the shared mesh is referenced twice, so the device holds
    // 101312 of them - three vertices each, since an STL file is a soup.
    EXPECT_EQ(meshes, 14u);
    EXPECT_EQ(triangles, 101312u);
    EXPECT_EQ(vertices, triangles * 3u);
}

TEST(DeviceIOTest, ResavingTheCommittedPackageMatchesItByteForByte)
{
    // The asset is what this writer produces: load it, write it out again, and each mesh file comes back identical. That
    // holds only because the facet normals of the file are kept rather than worked out from the winding.
    const auto path = assetPath("robots/irb_1600_10_145.vdev");
    auto       opened = vn::io::ZipArchive::open(path, vn::io::ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    auto&      source = opened.value();
    DeviceIO   io;
    const auto dev = io.loadPkg(source);
    ASSERT_NE(dev, nullptr);

    vn::io::ZipArchive written;
    io.savePkg(*dev, written);
    const auto geoms = written.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 14u); // 13 meshes, one of which two elements name separately
    for (const auto& entry : geoms.value()) {
        EXPECT_EQ(entry.path.extension(), std::filesystem::path(u8".stl")) << entry.path.generic_string();
    }

    const auto before = source.read(std::filesystem::path(u8"geoms/mesh0.stl"));
    const auto after  = written.read(std::filesystem::path(u8"geoms/mesh0.stl"));
    ASSERT_TRUE(before.ok());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(*after, *before);
}
