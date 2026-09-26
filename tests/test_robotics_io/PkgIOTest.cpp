#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
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
                ("vine_pkg_io_" + std::to_string(counter.fetch_add(1)));
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
 * @brief Builds a 3-link / 2-revolute-joint motion device with a box visual.
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
    joint2->setParentLink(base_ptr);
    joint2->setChildLink(link2_ptr);
    joint2->setDofInfos({ DofInfo{} });
    data->joints.push_back(std::move(joint2));

    auto robot = std::make_unique<MotionDevice>();
    robot->init(std::move(data));
    return robot;
}

/**
 * @brief Builds a motion device whose base link carries a triangle mesh visual.
 */
std::unique_ptr<MotionDevice> makeRobotWithMesh()
{
    auto robot = makeRobot();
    auto mesh  = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    auto* const base = robot->baseLink();
    base->body().visuals().resize(1);
    base->body().visuals()[0].setShape(mesh);
    return robot;
}

/**
 * @brief Builds a scanner with one revolute joint and one camera.
 */
std::unique_ptr<Scanner> makeScanner()
{
    auto data = std::make_unique<ScannerData>();
    data->metadata.name = u8"CamRig";

    auto base_link = std::make_unique<Link>(u8"base_link");
    auto link1     = std::make_unique<Link>(u8"link1");
    Link* const base_ptr  = base_link.get();
    Link* const link1_ptr = link1.get();
    data->links.push_back(std::move(base_link));
    data->links.push_back(std::move(link1));

    auto joint = std::make_unique<Joint>(FrameType::RevoluteJoint);
    joint->setName(u8"joint1");
    joint->setParentLink(base_ptr);
    joint->setChildLink(link1_ptr);
    joint->setDofInfos({ DofInfo{} });
    data->joints.push_back(std::move(joint));

    auto cam = std::make_unique<Scanner::Camera>();
    cam->frame_name              = u8"joint1";
    cam->design_intrinsics.width = 640.0;
    data->cameras.push_back(std::move(cam));

    auto scanner = std::make_unique<Scanner>();
    scanner->init(std::move(data));
    return scanner;
}

/**
 * @brief Builds a rigid table object with a triangle mesh visual.
 */
std::unique_ptr<RigidObject> makeTableWithMesh()
{
    auto table = std::make_unique<RigidObject>(u8"table");
    auto mesh  = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(2.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 2.0f, 0.0f));
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);
    return table;
}

} // namespace

TEST(PkgIOTest, DevicePkgRoundTrip)
{
    const TempDir temp;
    const auto    file = temp.path() / "robot.vdevpkg";

    auto     robot = makeRobot();
    DeviceIO io;
    io.savePkg(*robot, file);
    EXPECT_TRUE(std::filesystem::is_regular_file(file));

    auto loaded = io.loadPkg(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->filePath(), file);
    auto* const r = dynamic_cast<MotionDevice*>(loaded.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->deviceKind(), DeviceKind::Manipulator);
    EXPECT_EQ(r->name(), u8"UR");
    EXPECT_EQ(r->links().size(), 3u);
    EXPECT_EQ(r->joints().size(), 2u);
}

TEST(PkgIOTest, DevicePkgWithMeshRoundTrip)
{
    const TempDir temp;
    const auto    file = temp.path() / "robot.vdevpkg";

    auto     robot = makeRobotWithMesh();
    DeviceIO io;
    io.savePkg(*robot, file);

    auto loaded = io.loadPkg(file);
    ASSERT_NE(loaded, nullptr);
    auto* const r = dynamic_cast<MotionDevice*>(loaded.get());
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->baseLink()->body().visuals().size(), 1u);
    const auto* const m =
        dynamic_cast<const vn::geometry::TriangleMesh*>(r->baseLink()->body().visuals()[0].shape().get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->vertexCount(), 3u);
    EXPECT_EQ(m->triangleCount(), 1u);
    EXPECT_FLOAT_EQ(m->positions()[0].x, 0.0f);
    EXPECT_FLOAT_EQ(m->positions()[2].y, 1.0f);
}

TEST(PkgIOTest, DevicePkgMemoryBytes)
{
    auto     robot = makeRobot();
    DeviceIO io;

    vn::io::ZipArchive vfs;
    io.savePkg(*robot, vfs);
    ASSERT_TRUE(vfs.isFile(std::filesystem::path(u8"device.xml")));

    auto zip_bytes = vfs.toBytes();
    ASSERT_TRUE(zip_bytes.ok());
    auto opened = vn::io::ZipArchive::open(zip_bytes.take(), vn::io::ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());

    auto loaded = io.loadPkg(*opened);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->joints().size(), 2u);
}

TEST(PkgIOTest, TheStoredDeviceXmlIsExactlyTheDocument)
{
    // The XML printer's CStrSize() includes the NUL it terminates its buffer with, so what is stored is CStrSize() - 1
    // bytes. A conversion that kept the NUL would put one extra byte in the archive, and every test that merely loads
    // the package back would still pass - so this reads the stored file and says what is in it.
    auto     robot = makeRobot();
    DeviceIO io;

    vn::io::ZipArchive vfs;
    io.savePkg(*robot, vfs);
    const std::filesystem::path xml_path(u8"device.xml");
    ASSERT_TRUE(vfs.isFile(xml_path));

    const auto bytes = vfs.read(xml_path);
    ASSERT_TRUE(bytes.ok());
    const std::vector<unsigned char>& xml = bytes.value();
    ASSERT_FALSE(xml.empty());
    EXPECT_EQ(std::find(xml.begin(), xml.end(), static_cast<unsigned char>(0)), xml.end())
        << "the stored document must not carry the printer's terminating NUL";
    EXPECT_EQ(xml[0], static_cast<unsigned char>('<')) << "the document starts with its XML declaration";
}

TEST(PkgIOTest, WorkcellPkgRoundTrip)
{
    const TempDir temp;
    const auto    file = temp.path() / "cell.vwspkg";

    auto cell = std::make_unique<Workcell>();
    cell->setName(u8"demo");

    auto robot = makeRobotWithMesh();
    robot->setName(u8"robot1");
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell->addSceneObject(std::move(robot)));

    auto scanner = makeScanner();
    scanner->setName(u8"cam");
    cell->addSceneObject(std::move(scanner), robot_ptr->getEnd(0));

    cell->addSceneObject(makeTableWithMesh());

    WorkcellIO io;
    io.savePkg(*cell, file);
    EXPECT_TRUE(std::filesystem::is_regular_file(file));

    auto loaded = io.loadPkg(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name(), u8"demo");

    auto* const r = dynamic_cast<MotionDevice*>(loaded->findSceneObject(u8"robot1"));
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->isValid());
    EXPECT_EQ(r->joints().size(), 2u);
    // The robot's mesh visual round-trips through the package geoms.
    ASSERT_EQ(r->baseLink()->body().visuals().size(), 1u);
    const auto* const rm =
        dynamic_cast<const vn::geometry::TriangleMesh*>(r->baseLink()->body().visuals()[0].shape().get());
    ASSERT_NE(rm, nullptr);
    EXPECT_EQ(rm->vertexCount(), 3u);

    auto* const c = dynamic_cast<Scanner*>(loaded->findSceneObject(u8"cam"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->cameras().size(), 1u);

    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    const auto* const tm = dynamic_cast<const vn::geometry::TriangleMesh*>(t->body().visuals()[0].shape().get());
    ASSERT_NE(tm, nullptr);
    EXPECT_EQ(tm->vertexCount(), 3u);
    EXPECT_FLOAT_EQ(tm->positions()[1].x, 2.0f);

    // Hierarchy preserved.
    EXPECT_EQ(loaded->parentOf(r), nullptr);
    EXPECT_EQ(loaded->parentOf(c), static_cast<SceneObject*>(r));
}

TEST(PkgIOTest, WorkcellPkgInternalPathsAndMemory)
{
    auto cell = std::make_unique<Workcell>();
    cell->setName(u8"demo");
    auto robot = makeRobotWithMesh();
    robot->setName(u8"robot1");
    cell->addSceneObject(std::move(robot));
    cell->addSceneObject(makeTableWithMesh());

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    ASSERT_TRUE(vfs.isFile(std::filesystem::path(u8"workcell.xml")));
    ASSERT_TRUE(vfs.isFile(std::filesystem::path(u8"devices/robot1.vdevpkg")));
    // Mesh bins live under geoms/ inside the package.
    ASSERT_TRUE(vfs.isDirectory(std::filesystem::path(u8"geoms")));
    ASSERT_TRUE(vfs.exists(std::filesystem::path(u8"geoms/mesh0.positions.bin")));

    // Persist to zip bytes and reopen: the whole package round-trips in memory.
    auto zip_bytes = vfs.toBytes();
    ASSERT_TRUE(zip_bytes.ok());
    auto opened = vn::io::ZipArchive::open(zip_bytes.take(), vn::io::ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());

    auto loaded = io.loadPkg(*opened);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name(), u8"demo");
    EXPECT_NE(loaded->findSceneObject(u8"robot1"), nullptr);
    EXPECT_NE(loaded->findSceneObject(u8"table"), nullptr);
}

TEST(PkgIOTest, NestedDevicePackage)
{
    auto cell = std::make_unique<Workcell>();
    cell->setName(u8"demo");
    auto robot = makeRobotWithMesh();
    robot->setName(u8"robot1");
    cell->addSceneObject(std::move(robot));
    cell->addSceneObject(makeTableWithMesh());

    // Devices are always stored as nested .vdevpkg zip entries inside a package.
    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    ASSERT_TRUE(vfs.isFile(std::filesystem::path(u8"workcell.xml")));
    ASSERT_TRUE(vfs.isFile(std::filesystem::path(u8"devices/robot1.vdevpkg")));
    EXPECT_FALSE(vfs.exists(std::filesystem::path(u8"devices/robot1.vdev")));

    std::vector<unsigned char> zip_bytes;
    {
        const auto bytes = vfs.toBytes();
        ASSERT_TRUE(bytes.ok());
        zip_bytes = bytes.value();
    }
    auto opened = vn::io::ZipArchive::open(std::move(zip_bytes), vn::io::ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());

    // The loader dispatches by extension: .vdevpkg opens a nested VFS and the
    // device's geoms resolve relative to the nested package root.
    auto loaded = io.loadPkg(*opened);
    ASSERT_NE(loaded, nullptr);
    auto* const r = dynamic_cast<MotionDevice*>(loaded->findSceneObject(u8"robot1"));
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->baseLink()->body().visuals().size(), 1u);
    const auto* const rm =
        dynamic_cast<const vn::geometry::TriangleMesh*>(r->baseLink()->body().visuals()[0].shape().get());
    ASSERT_NE(rm, nullptr);
    EXPECT_EQ(rm->vertexCount(), 3u);
}

TEST(PkgIOTest, IndexedMeshRoundTrip)
{
    auto table = std::make_unique<RigidObject>(u8"table");
    auto mesh  = vn::intrusive_ptr<vn::geometry::IndexedTriangleMesh>(
        new vn::geometry::IndexedTriangleMesh());
    const std::uint32_t v0 = mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    const std::uint32_t v1 = mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
    const std::uint32_t v2 = mesh->addVertex(vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(v0, v1, v2);
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    // Indexed mesh writes positions + indices bins.
    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    EXPECT_EQ(geoms->size(), 2u);

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    const auto* const m =
        dynamic_cast<const vn::geometry::IndexedTriangleMesh*>(t->body().visuals()[0].shape().get());
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->vertexCount(), 3u);
    EXPECT_EQ(m->triangleCount(), 1u);
    EXPECT_EQ(m->indices().size(), 3u);
    EXPECT_FLOAT_EQ(m->positions()[1].x, 1.0f);
}

TEST(PkgIOTest, SharedMeshStoredOnce)
{
    auto table = std::make_unique<RigidObject>(u8"table");
    auto mesh  = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    // Two visuals share the exact same shape object.
    table->body().visuals().resize(2);
    table->body().visuals()[0].setShape(mesh);
    table->body().visuals()[1].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    // The shared mesh is written once: only a single positions bin exists.
    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 1u);
    EXPECT_EQ(geoms->front().name(), std::filesystem::path(u8"mesh0.positions.bin"));

    // Both visuals round-trip with the mesh.
    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->body().visuals().size(), 2u);
    const auto* const m0 =
        dynamic_cast<const vn::geometry::TriangleMesh*>(t->body().visuals()[0].shape().get());
    const auto* const m1 =
        dynamic_cast<const vn::geometry::TriangleMesh*>(t->body().visuals()[1].shape().get());
    ASSERT_NE(m0, nullptr);
    ASSERT_NE(m1, nullptr);
    EXPECT_EQ(m0->vertexCount(), 3u);
    EXPECT_EQ(m1->vertexCount(), 3u);
    EXPECT_FLOAT_EQ(m0->positions()[2].x, 0.0f);
}
