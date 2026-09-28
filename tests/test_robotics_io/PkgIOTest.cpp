#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
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
    // Mesh files live under geoms/ inside the package, one file per mesh.
    ASSERT_TRUE(vfs.isDirectory(std::filesystem::path(u8"geoms")));
    ASSERT_TRUE(vfs.exists(std::filesystem::path(u8"geoms/mesh0.stl")));

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
    // Texture coordinates are what a mesh has to have for the self-describing form: binary STL has nowhere to put them.
    mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                vn::math::Vec2f(0.0f, 1.0f) });
    mesh->setNormals(vn::geometry::Vec3fArray{ vn::math::Vec3f(0.0f, 0.0f, 1.0f), vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                               vn::math::Vec3f(0.0f, 0.0f, 1.0f) });
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    // One file for the mesh, with the header saying which arrays follow - so every array shares it.
    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 1u);
    EXPECT_EQ(geoms->front().name(), std::filesystem::path(u8"mesh0.vmesh"));
    // header + positions + normals + texcoords + indices
    EXPECT_EQ(geoms->front().size, 24u + 3u * 12u + 3u * 12u + 3u * 8u + 3u * 4u);

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
    ASSERT_EQ(m->texcoords().size(), 3u);
    EXPECT_FLOAT_EQ(m->texcoords()[1].x, 1.0f);
    ASSERT_EQ(m->normals().size(), 3u);
    EXPECT_FLOAT_EQ(m->normals()[2].z, 1.0f);
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

    // The shared mesh is written once: a single mesh file exists, and the second visual points at the same one.
    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 1u);
    EXPECT_EQ(geoms->front().name(), std::filesystem::path(u8"mesh0.stl"));

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


TEST(PkgIOTest, DamagedMeshFileIsRefused)
{
    auto table = std::make_unique<RigidObject>(u8"table");
    auto mesh  = vn::intrusive_ptr<vn::geometry::IndexedTriangleMesh>(
        new vn::geometry::IndexedTriangleMesh());
    const std::uint32_t v0 = mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    const std::uint32_t v1 = mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
    const std::uint32_t v2 = mesh->addVertex(vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(v0, v1, v2);
    // Texture coordinates put the mesh in the self-describing form, which is the form this test damages.
    mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                vn::math::Vec2f(0.0f, 1.0f) });
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    const auto good = vfs.read(std::filesystem::path(u8"geoms/mesh0.vmesh"));
    ASSERT_TRUE(good.ok());
    const std::vector<unsigned char> file = good.value();

    // Replaces the mesh file and reports whether the object still comes back with a shape. A damaged file has to
    // leave the shape out: the alternative - reading as far as the bytes happen to go - is a smaller mesh that
    // nothing downstream can tell from the real one.
    const auto shapeSurvives = [&](const std::span<const unsigned char> bytes) {
        EXPECT_EQ(vfs.addFile(std::filesystem::path(u8"geoms/mesh0.vmesh"), bytes), vn::io::IoError::Ok);
        auto loaded = io.loadPkg(vfs);
        EXPECT_NE(loaded, nullptr);
        if (loaded == nullptr) {
            return false;
        }
        auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
        if (t == nullptr || t->body().visuals().empty()) {
            return false;
        }
        return t->body().visuals()[0].shape() != nullptr;
    };

    EXPECT_TRUE(shapeSurvives(file)); // the package this test builds does load its mesh

    {
        std::vector<unsigned char> damaged = file;
        damaged[0] = 'X'; // not a mesh file at all
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        // The last four bytes are the file's last index: point it at a vertex the mesh does not have, and the claim the
        // header makes ("these indices belong to these vertices") no longer holds.
        std::vector<unsigned char> damaged = file;
        const std::uint32_t        wild    = 0xFFFFFFFFu;
        std::memcpy(damaged.data() + damaged.size() - sizeof(wild), &wild, sizeof(wild));
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged(file.begin(), file.end() - 12); // the index block is missing
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged              = file;
        const std::uint64_t        impossible_triangles = 5; // more than the file has room for
        std::memcpy(damaged.data() + 16, &impossible_triangles, sizeof(impossible_triangles));
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged = file;
        damaged.resize(file.size() + 12u); // bytes the header does not know about: the description is not the file
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged = file;
        std::uint16_t              flags   = 0;
        std::memcpy(&flags, damaged.data() + 6, sizeof(flags)); // the file carries this machine's own byte order
        flags |= 0x0100u;                                       // claims the other one
        std::memcpy(damaged.data() + 6, &flags, sizeof(flags));
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged = file;
        std::uint16_t              flags   = 0;
        std::memcpy(&flags, damaged.data() + 6, sizeof(flags));
        flags |= 0x0020u; // names a block this build has no name for, so its length cannot be worked out
        std::memcpy(damaged.data() + 6, &flags, sizeof(flags));
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged = file;
        const std::uint16_t        version = 9; // a version this build does not know
        std::memcpy(damaged.data() + 4, &version, sizeof(version));
        EXPECT_FALSE(shapeSurvives(damaged));
    }

    EXPECT_TRUE(shapeSurvives(file)); // and the refusal was the file's, not a state the package kept
}

TEST(PkgIOTest, IndexedMeshWithoutTexcoordsIsStoredAsASoup)
{
    // Binary STL has no indices, so a mesh that has no texture coordinates is written as one - which means shared vertices
    // become one copy per triangle. That is the trade: a standard file in exchange for a soup.
    auto mesh = vn::intrusive_ptr<vn::geometry::IndexedTriangleMesh>(new vn::geometry::IndexedTriangleMesh());
    const std::uint32_t v0 = mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
    const std::uint32_t v1 = mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
    const std::uint32_t v2 = mesh->addVertex(vn::math::Vec3f(1.0f, 1.0f, 0.0f));
    const std::uint32_t v3 = mesh->addVertex(vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(v0, v1, v2);
    mesh->addTriangle(v0, v2, v3);
    ASSERT_EQ(mesh->vertexCount(), 4u);

    auto table = std::make_unique<RigidObject>(u8"table");
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 1u);
    EXPECT_EQ(geoms->front().name(), std::filesystem::path(u8"mesh0.stl"));
    EXPECT_EQ(geoms->front().size, 84u + 50u * 2u); // the STL prefix, then one fixed-size record per triangle

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    const auto* const soup = dynamic_cast<const vn::geometry::TriangleMesh*>(t->body().visuals()[0].shape().get());
    ASSERT_NE(soup, nullptr); // a soup, not an indexed mesh, whatever went in
    EXPECT_EQ(soup->vertexCount(), 6u); // four vertices became six, because two of them are shared by both triangles
    EXPECT_EQ(soup->triangleCount(), 2u);
    // The corners stay in the order the triangles named them.
    EXPECT_FLOAT_EQ(soup->positions()[0].x, 0.0f);
    EXPECT_FLOAT_EQ(soup->positions()[3].x, 0.0f); // the shared v0, written again for the second triangle
    EXPECT_FLOAT_EQ(soup->positions()[5].y, 1.0f); // v3
}

TEST(PkgIOTest, DamagedStlFileIsRefused)
{
    auto table = std::make_unique<RigidObject>(u8"table");
    auto mesh  = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(0.0f, 1.0f, 0.0f),
                      vn::math::Vec3f(1.0f, 1.0f, 0.0f));
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    const auto good = vfs.read(std::filesystem::path(u8"geoms/mesh0.stl"));
    ASSERT_TRUE(good.ok());
    const std::vector<unsigned char> file = good.value();
    ASSERT_EQ(file.size(), 84u + 50u * 2u);

    // Writes a triangle count into the prefix the way binary STL defines it, which is little-endian on every machine.
    const auto withCount = [&](std::uint32_t count) {
        std::vector<unsigned char> damaged = file;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            damaged[80u + byte] = static_cast<unsigned char>((count >> (8u * byte)) & 0xFFu);
        }
        return damaged;
    };
    const auto shapeSurvives = [&](const std::span<const unsigned char> bytes) {
        EXPECT_EQ(vfs.addFile(std::filesystem::path(u8"geoms/mesh0.stl"), bytes), vn::io::IoError::Ok);
        auto loaded = io.loadPkg(vfs);
        EXPECT_NE(loaded, nullptr);
        if (loaded == nullptr) {
            return false;
        }
        auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
        if (t == nullptr || t->body().visuals().empty()) {
            return false;
        }
        return t->body().visuals()[0].shape() != nullptr;
    };

    EXPECT_TRUE(shapeSurvives(file)); // the package this test builds does load its mesh

    {
        std::vector<unsigned char> damaged(file.begin(), file.end() - 10u); // the second triangle is cut short
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    EXPECT_FALSE(shapeSurvives(withCount(3))); // one triangle more than the file holds
    EXPECT_FALSE(shapeSurvives(withCount(0))); // a file without triangles holds no mesh
    {
        // Nothing but the prefix, and a count of zero: the length adds up to exactly what the count says, so only the
        // rule that a mesh file has at least one triangle can refuse this one.
        std::vector<unsigned char> damaged = withCount(0);
        damaged.resize(84u);
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        std::vector<unsigned char> damaged = file;
        damaged.resize(file.size() + 4u); // bytes the file's own count does not account for
        EXPECT_FALSE(shapeSurvives(damaged));
    }
    {
        // The text form of STL: its length is not what a binary file's count would make it, so it is refused rather than
        // read as bytes that happen to be there.
        const std::string text = "solid triangle\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nendloop\nendsolid\n";
        EXPECT_FALSE(shapeSurvives(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(text.data()), text.size())));
    }

    EXPECT_TRUE(shapeSurvives(file)); // and the refusal was the file's, not a state the package kept
}

TEST(PkgIOTest, TriangleSoupWithTexcoordsUsesTheSelfDescribingForm)
{
    // A soup has to take the self-describing form when it carries texture coordinates, and it has to come back as a soup:
    // this is the non-indexed half of that form, which nothing else in this file covers.
    auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                vn::math::Vec2f(0.0f, 1.0f) });

    auto table = std::make_unique<RigidObject>(u8"table");
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    const auto geoms = vfs.list(std::filesystem::path(u8"geoms"));
    ASSERT_TRUE(geoms.ok());
    ASSERT_EQ(geoms->size(), 1u);
    EXPECT_EQ(geoms->front().name(), std::filesystem::path(u8"mesh0.vmesh"));
    EXPECT_EQ(geoms->front().size, 24u + 3u * 12u + 3u * 8u); // header + positions + texcoords, and no indices

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    const auto* const soup = dynamic_cast<const vn::geometry::TriangleMesh*>(t->body().visuals()[0].shape().get());
    ASSERT_NE(soup, nullptr);
    EXPECT_EQ(soup->vertexCount(), 3u);
    EXPECT_EQ(soup->triangleCount(), 1u);
    ASSERT_EQ(soup->texcoords().size(), 3u);
    EXPECT_FLOAT_EQ(soup->texcoords()[2].y, 1.0f);
}

/**
 * @brief Renames the mesh element a package's description carries.
 *
 * @param vfs The package to edit.
 * @param from The element name it has now.
 * @param to The element name to write.
 * @return true when the description was found and rewritten.
 */
bool retagMeshElement(vn::io::ZipArchive& vfs, const std::string& from, const std::string& to)
{
    const auto xml = vfs.read(std::filesystem::path(u8"workcell.xml"));
    if (!xml.ok()) {
        return false;
    }
    std::string text(xml->begin(), xml->end());
    const auto  at = text.find(from);
    if (at == std::string::npos) {
        return false;
    }
    text.replace(at, from.size(), to);
    const auto bytes = std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    return vfs.addFile(std::filesystem::path(u8"workcell.xml"), bytes) == vn::io::IoError::Ok;
}

TEST(PkgIOTest, IndexedDescriptionOfAnStlFileIsRefused)
{
    // The element describes the file, and binary STL has no index block: an element that promises indices the file cannot
    // hold is refused rather than read as a soup behind the description's back.
    auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));

    auto table = std::make_unique<RigidObject>(u8"table");
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    ASSERT_TRUE(retagMeshElement(vfs, "<triangle_mesh", "<indexed_triangle_mesh"));

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->body().visuals().size(), 1u);
    EXPECT_EQ(t->body().visuals()[0].shape(), nullptr);
}

TEST(PkgIOTest, IndexedDescriptionOfASoupVmeshIsRefused)
{
    // The same rule inside one form: the description says indexed, the file has no index block, so the two do not agree.
    auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                vn::math::Vec2f(0.0f, 1.0f) });

    auto table = std::make_unique<RigidObject>(u8"table");
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);
    ASSERT_TRUE(retagMeshElement(vfs, "<triangle_mesh", "<indexed_triangle_mesh"));

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->body().visuals().size(), 1u);
    EXPECT_EQ(t->body().visuals()[0].shape(), nullptr);
}

TEST(PkgIOTest, EditedMeshIsRefusedWhenThePackageIsFinallyWritten)
{
    // A ZIP records a content source and pulls it while the package is written, so a mesh edited between savePkg() and
    // the write is a mesh whose promised bytes are gone. Both forms have to refuse it instead of writing the old arrays.
    const auto refusedAfterEdit = [](const bool with_texcoords) {
        auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
        mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                          vn::math::Vec3f(0.0f, 1.0f, 0.0f));
        if (with_texcoords) {
            mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                        vn::math::Vec2f(0.0f, 1.0f) });
        }

        auto table = std::make_unique<RigidObject>(u8"table");
        table->body().visuals().resize(1);
        table->body().visuals()[0].setShape(mesh);

        auto cell = std::make_unique<Workcell>();
        cell->addSceneObject(std::move(table));

        vn::io::ZipArchive vfs;
        WorkcellIO             io;
        io.savePkg(*cell, vfs); // the source is handed over here, and pulled at write time

        // The mesh's own edit contract announces this: a soup is edited by appending a triangle.
        mesh->addTriangle(vn::math::Vec3f(2.0f, 2.0f, 2.0f), vn::math::Vec3f(3.0f, 2.0f, 2.0f),
                          vn::math::Vec3f(2.0f, 3.0f, 2.0f));

        return !vfs.toBytes().ok();
    };

    EXPECT_TRUE(refusedAfterEdit(false)) << "binary STL: the mesh was edited after it was handed over";
    EXPECT_TRUE(refusedAfterEdit(true)) << "self-describing form: the same, and it is the other source that refuses";
}

TEST(PkgIOTest, InconsistentMeshesAreRefused)
{
    // Two meshes the model lets you build but a file cannot hold: a normal array shorter than the positions, and an index
    // that names a vertex the mesh does not have. Both are refused rather than written without the offending array.
    const auto saveFails = [](const vn::intrusive_ptr<vn::geometry::Mesh>& mesh) {
        auto table = std::make_unique<RigidObject>(u8"table");
        table->body().visuals().resize(1);
        table->body().visuals()[0].setShape(mesh);

        auto cell = std::make_unique<Workcell>();
        cell->addSceneObject(std::move(table));

        vn::io::ZipArchive vfs;
        WorkcellIO             io;
        try {
            io.savePkg(*cell, vfs);
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };

    {
        auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
        mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                          vn::math::Vec3f(0.0f, 1.0f, 0.0f));
        mesh->setTexcoords(vn::geometry::Vec2fArray{ vn::math::Vec2f(0.0f, 0.0f), vn::math::Vec2f(1.0f, 0.0f),
                                                    vn::math::Vec2f(0.0f, 1.0f) });
        // One normal too few for three vertices: the file would hold a normal array that says something else.
        mesh->setNormals(vn::geometry::Vec3fArray{ vn::math::Vec3f(0.0f, 0.0f, 1.0f),
                                                   vn::math::Vec3f(0.0f, 0.0f, 1.0f) });
        EXPECT_TRUE(saveFails(mesh));
    }
    {
        auto mesh = vn::intrusive_ptr<vn::geometry::IndexedTriangleMesh>(new vn::geometry::IndexedTriangleMesh());
        const std::uint32_t v0 = mesh->addVertex(vn::math::Vec3f(0.0f, 0.0f, 0.0f));
        const std::uint32_t v1 = mesh->addVertex(vn::math::Vec3f(1.0f, 0.0f, 0.0f));
        mesh->addVertex(vn::math::Vec3f(0.0f, 1.0f, 0.0f));
        mesh->setIndices(vn::geometry::UInt32Array{ v0, v1, 99u }); // 99 names no vertex
        EXPECT_TRUE(saveFails(mesh));
    }
}

TEST(PkgIOTest, StlEntryNamedInCapitalsIsReadAsStl)
{
    // The form is what the path says, and the case of an extension is not what anybody means by that: a file named
    // .STL is the same form as one named .stl.
    auto mesh = vn::intrusive_ptr<vn::geometry::TriangleMesh>(new vn::geometry::TriangleMesh());
    mesh->addTriangle(vn::math::Vec3f(0.0f, 0.0f, 0.0f), vn::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vn::math::Vec3f(0.0f, 1.0f, 0.0f));

    auto table = std::make_unique<RigidObject>(u8"table");
    table->body().visuals().resize(1);
    table->body().visuals()[0].setShape(mesh);

    auto cell = std::make_unique<Workcell>();
    cell->addSceneObject(std::move(table));

    vn::io::ZipArchive vfs;
    WorkcellIO             io;
    io.savePkg(*cell, vfs);

    const std::string from = "geoms/mesh0.stl";
    const std::string to   = "geoms/MESH0.STL";
    const auto        xml  = vfs.read(std::filesystem::path(u8"workcell.xml"));
    const auto        stl  = vfs.read(std::filesystem::path(u8"geoms/mesh0.stl"));
    ASSERT_TRUE(xml.ok());
    ASSERT_TRUE(stl.ok());
    std::string text(xml->begin(), xml->end());
    ASSERT_NE(text.find(from), std::string::npos);
    text.replace(text.find(from), from.size(), to);
    ASSERT_EQ(vfs.addFile(std::filesystem::path(u8"geoms/MESH0.STL"),
                          std::span<const unsigned char>(stl->data(), stl->size())),
              vn::io::IoError::Ok);
    ASSERT_EQ(vfs.remove(std::filesystem::path(u8"geoms/mesh0.stl")), vn::io::IoError::Ok);
    ASSERT_EQ(vfs.addFile(std::filesystem::path(u8"workcell.xml"),
                          std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(text.data()), text.size())),
              vn::io::IoError::Ok);

    auto loaded = io.loadPkg(vfs);
    ASSERT_NE(loaded, nullptr);
    auto* const t = dynamic_cast<RigidObject*>(loaded->findSceneObject(u8"table"));
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->body().visuals().size(), 1u);
    EXPECT_NE(t->body().visuals()[0].shape(), nullptr);
}
