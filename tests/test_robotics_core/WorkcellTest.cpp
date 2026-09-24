#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/kinematics/State.hpp>
#include <vine/robotics/workcell/Device.hpp>
#include <vine/robotics/workcell/MotionDevice.hpp>
#include <vine/robotics/workcell/RigidObject.hpp>
#include <vine/robotics/workcell/Scanner.hpp>
#include <vine/robotics/workcell/Workcell.hpp>

using namespace vn::math;
using namespace vn::robotics;
using namespace vn::robotics::kinematics;
using namespace vn::robotics::workcell;

namespace
{

/**
 * @brief Compares two isometries within a small tolerance.
 */
void expectNear(const Isometry3d& a, const Isometry3d& b)
{
    EXPECT_LT((a.translation - b.translation).length(), 1e-9);
    EXPECT_EQ(a.rotation, b.rotation);
}

/**
 * @brief Builds a 3-link / 2-revolute-joint device definition.
 *
 * Chain: base_link -j1- link1 -j2- link2.
 */
std::unique_ptr<DeviceData> makeRobotData()
{
    auto data = std::make_unique<DeviceData>();
    data->metadata.name  = u8"UR";
    data->metadata.model = u8"UR10";

    auto base_link = std::make_unique<Link>(u8"base_link");
    auto link1     = std::make_unique<Link>(u8"link1");
    auto link2     = std::make_unique<Link>(u8"link2");
    Link* const base_ptr = base_link.get();
    Link* const link1_ptr = link1.get();
    Link* const link2_ptr = link2.get();

    data->links.push_back(std::move(base_link));
    data->links.push_back(std::move(link1));
    data->links.push_back(std::move(link2));

    auto joint1 = std::make_unique<Joint>(FrameType::RevoluteJoint);
    joint1->setName(u8"joint1");
    joint1->setParentLink(base_ptr);
    joint1->setChildLink(link1_ptr);
    DofInfo dof1;
    dof1.lower = -3.14;
    dof1.upper = 3.14;
    dof1.velocity_limit = 1.5;
    joint1->setDofInfos({ dof1 });

    auto joint2 = std::make_unique<Joint>(FrameType::RevoluteJoint);
    joint2->setName(u8"joint2");
    joint2->setParentLink(link1_ptr);
    joint2->setChildLink(link2_ptr);
    DofInfo dof2;
    dof2.lower = -2.0;
    dof2.upper = 2.0;
    dof2.velocity_limit = 1.0;
    joint2->setDofInfos({ dof2 });

    data->joints.push_back(std::move(joint1));
    data->joints.push_back(std::move(joint2));

    return data;
}

} // namespace

TEST(WorkcellTest, AddAndFindObjects)
{
    Workcell cell;
    cell.setName(u8"Cell");

    auto robot = std::make_unique<MotionDevice>(u8"robot");
    robot->init(makeRobotData());
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell.addSceneObject(std::move(robot)));
    ASSERT_NE(robot_ptr, nullptr);

    EXPECT_TRUE(robot_ptr->isValid());
    EXPECT_EQ(robot_ptr->kind(), SceneObjectKind::Device);
    EXPECT_EQ(robot_ptr->deviceKind(), DeviceKind::Other);
    EXPECT_EQ(robot_ptr->modelName(), u8"UR10");
    EXPECT_EQ(robot_ptr->name(), u8"robot");
    EXPECT_EQ(robot_ptr->links().size(), 3u);
    EXPECT_EQ(robot_ptr->joints().size(), 2u);
    EXPECT_EQ(robot_ptr->getNumEnds(), 1u);
    EXPECT_EQ(robot_ptr->baseLink()->name(), u8"base_link");
    ASSERT_NE(robot_ptr->data(), nullptr);
    EXPECT_EQ(robot_ptr->data()->metadata.name, u8"UR");

    auto table = std::make_unique<RigidObject>(u8"table");
    RigidObject* const table_ptr = static_cast<RigidObject*>(cell.addSceneObject(std::move(table)));
    ASSERT_NE(table_ptr, nullptr);
    EXPECT_EQ(table_ptr->kind(), SceneObjectKind::RigidObject);
    EXPECT_EQ(table_ptr->name(), u8"table");

    EXPECT_EQ(cell.findSceneObject(u8"robot"), static_cast<SceneObject*>(robot_ptr));
    EXPECT_EQ(cell.findSceneObject(u8"table"), static_cast<SceneObject*>(table_ptr));
    EXPECT_EQ(cell.findSceneObject(u8"nope"), nullptr);
    EXPECT_EQ(cell.sceneObjects().size(), 2u);

    // A duplicate object name is rejected and ownership is not taken.
    auto dup = std::make_unique<RigidObject>(u8"robot");
    EXPECT_EQ(cell.addSceneObject(std::move(dup)), nullptr);
    EXPECT_EQ(cell.sceneObjects().size(), 2u);
}

TEST(WorkcellTest, FrameTreeAndHierarchy)
{
    Workcell cell;

    auto robot = std::make_unique<MotionDevice>(u8"robot");
    robot->init(makeRobotData());
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell.addSceneObject(std::move(robot)));

    // A scanner mounted on the robot's end frame becomes its child.
    auto scanner = std::make_unique<Scanner>(u8"scanner");
    Scanner* const scanner_ptr = static_cast<Scanner*>(cell.addSceneObject(std::move(scanner), robot_ptr->getEnd(0)));
    ASSERT_NE(scanner_ptr, nullptr);

    // The robot is a top-level object mounted on the world frame.
    EXPECT_EQ(cell.parentOf(robot_ptr), nullptr);
    EXPECT_EQ(robot_ptr->parentObject(), nullptr);

    // The scanner hangs from the robot's end frame.
    EXPECT_EQ(cell.parentOf(scanner_ptr), static_cast<SceneObject*>(robot_ptr));
    EXPECT_EQ(scanner_ptr->parentObject(), static_cast<SceneObject*>(robot_ptr));
    EXPECT_TRUE(scanner_ptr->isChildOf(robot_ptr));
    EXPECT_TRUE(scanner_ptr->isDescendantOf(robot_ptr));
    EXPECT_TRUE(cell.isAncestorOf(robot_ptr, scanner_ptr));
    EXPECT_FALSE(robot_ptr->isDescendantOf(scanner_ptr));

    // children / descendants queries
    const auto children = cell.childrenOf(robot_ptr);
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0], static_cast<SceneObject*>(scanner_ptr));

    const auto descendants = cell.descendantsOf(robot_ptr);
    ASSERT_EQ(descendants.size(), 1u);
    EXPECT_EQ(descendants[0], static_cast<SceneObject*>(scanner_ptr));

    // Frame-to-object lookup works for device and mounted-object frames.
    EXPECT_EQ(cell.findSceneObjectByFrame(scanner_ptr->baseFrame()),
              static_cast<SceneObject*>(scanner_ptr));
    EXPECT_EQ(cell.findSceneObjectByFrame(robot_ptr->joints()[0]),
              static_cast<SceneObject*>(robot_ptr));
    EXPECT_EQ(cell.findSceneObjectByFrame(cell.worldFrame()), nullptr);
}

TEST(WorkcellTest, DeviceGetSetQ)
{
    Workcell cell;

    auto robot = std::make_unique<MotionDevice>(u8"robot");
    robot->init(makeRobotData());
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell.addSceneObject(std::move(robot)));

    kinematics::State state;
    state.setup(cell.worldFrame());

    // Default joint values are all zero.
    EXPECT_EQ(robot_ptr->getQ(state), (kinematics::Q{ 0.0, 0.0 }));

    kinematics::Q q{ 1.5, -2.0 };
    robot_ptr->setQ(q, state);
    EXPECT_EQ(robot_ptr->getQ(state), q);

    // A joint-value vector with a mismatched size is rejected.
    EXPECT_THROW(robot_ptr->setQ(kinematics::Q{ 1.0 }, state), std::invalid_argument);
    EXPECT_THROW(robot_ptr->setQ(kinematics::Q{ 1.0, 2.0, 3.0 }, state), std::invalid_argument);
}

TEST(WorkcellTest, DeviceDataClone)
{
    const auto data  = makeRobotData();
    const auto clone = data->clone();
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->links.size(), data->links.size());
    ASSERT_EQ(clone->joints.size(), data->joints.size());

    // Cloned joints are rebound to the cloned links, preserving structure.
    EXPECT_NE(clone->joints[0]->parentLink(), data->joints[0]->parentLink());
    EXPECT_EQ(clone->joints[0]->parentLink()->name(), data->joints[0]->parentLink()->name());

    // The clone can initialize an independent device.
    MotionDevice robot(u8"robot2");
    robot.init(clone->clone());
    EXPECT_TRUE(robot.isValid());
    EXPECT_EQ(robot.getNumEnds(), 1u);
}

TEST(WorkcellTest, ReparentAndRename)
{
    Workcell cell;

    auto robot = std::make_unique<MotionDevice>(u8"robot");
    robot->init(makeRobotData());
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell.addSceneObject(std::move(robot)));

    auto table = std::make_unique<RigidObject>(u8"table");
    RigidObject* const table_ptr = static_cast<RigidObject*>(cell.addSceneObject(std::move(table)));

    // Mount the table under the robot's end frame.
    EXPECT_TRUE(cell.changeObjectParent(table_ptr, robot_ptr->getEnd(0)));
    EXPECT_EQ(cell.parentOf(table_ptr), static_cast<SceneObject*>(robot_ptr));

    // Move it back to the world frame.
    EXPECT_TRUE(cell.changeObjectParent(table_ptr, cell.worldFrame()));
    EXPECT_EQ(cell.parentOf(table_ptr), nullptr);

    // Renaming works, but duplicate / empty names are rejected.
    EXPECT_TRUE(cell.changeObjectName(table_ptr, u8"workbench"));
    EXPECT_EQ(cell.findSceneObject(u8"workbench"), static_cast<SceneObject*>(table_ptr));
    EXPECT_FALSE(cell.changeObjectName(table_ptr, u8"robot"));
    EXPECT_FALSE(cell.changeObjectName(table_ptr, u8""));

    // Reparenting onto an object's own frame is rejected (cycle guard).
    EXPECT_FALSE(cell.changeObjectParent(robot_ptr, robot_ptr->baseFrame()));
}

TEST(WorkcellTest, RemoveDetachesMounted)
{
    Workcell cell;

    auto robot = std::make_unique<MotionDevice>(u8"robot");
    robot->init(makeRobotData());
    MotionDevice* const robot_ptr = static_cast<MotionDevice*>(cell.addSceneObject(std::move(robot)));

    auto scanner = std::make_unique<Scanner>(u8"scanner");
    Scanner* const scanner_ptr = static_cast<Scanner*>(cell.addSceneObject(std::move(scanner), robot_ptr->getEnd(0)));
    ASSERT_NE(scanner_ptr, nullptr);
    EXPECT_EQ(cell.parentOf(scanner_ptr), static_cast<SceneObject*>(robot_ptr));

    // Removing the robot re-parents the mounted scanner to the world frame.
    EXPECT_TRUE(cell.removeSceneObject(u8"robot"));
    EXPECT_EQ(cell.findSceneObject(u8"robot"), nullptr);
    EXPECT_EQ(cell.parentOf(scanner_ptr), nullptr);
    EXPECT_EQ(cell.findSceneObject(u8"scanner"), static_cast<SceneObject*>(scanner_ptr));

    // The scanner itself can then be removed.
    EXPECT_TRUE(cell.removeSceneObject(u8"scanner"));
    EXPECT_TRUE(cell.sceneObjects().empty());
}

TEST(WorkcellTest, MotionDeviceBoundsAndHome)
{
    MotionDevice robot(u8"robot");
    robot.init(makeRobotData());

    ASSERT_TRUE(robot.isValid());
    ASSERT_EQ(robot.joints().size(), 2u);

    // The device owns a serial-chain kinematics model built base -> first end.
    ASSERT_NE(robot.kinematics(), nullptr);
    EXPECT_EQ(robot.kinematics()->dof(), 2u);
    ASSERT_EQ(robot.kinematics()->joints().size(), 2u);

    // Bounds are derived from the kinematics, one value per dof.
    ASSERT_EQ(robot.lowerBounds().size(), 2u);
    ASSERT_EQ(robot.upperBounds().size(), 2u);
    EXPECT_DOUBLE_EQ(robot.lowerBounds()[0], -3.14);
    EXPECT_DOUBLE_EQ(robot.upperBounds()[0], 3.14);
    EXPECT_DOUBLE_EQ(robot.lowerBounds()[1], -2.0);
    EXPECT_DOUBLE_EQ(robot.upperBounds()[1], 2.0);
    ASSERT_EQ(robot.maxVelocityLimits().size(), 2u);
    EXPECT_DOUBLE_EQ(robot.maxVelocityLimits()[0], 1.5);
    EXPECT_DOUBLE_EQ(robot.maxVelocityLimits()[1], 1.0);

    // Home defaults to the value closest to zero within the bounds.
    ASSERT_EQ(robot.homeQ().size(), 2u);
    EXPECT_DOUBLE_EQ(robot.homeQ()[0], 0.0);
    EXPECT_DOUBLE_EQ(robot.homeQ()[1], 0.0);

    // The home can be overridden.
    robot.setHomeQ(Q{ 1.0, -1.0 });
    EXPECT_EQ(robot.homeQ(), (Q{ 1.0, -1.0 }));
}

TEST(WorkcellTest, JointDofInfosFromType)
{
    Joint revolute(FrameType::RevoluteJoint);
    Joint prismatic(FrameType::PrismaticJoint);
    Joint planar(FrameType::PlanarJoint);
    Joint fixed;

    // The DoF infos are pre-sized to the type-derived dof count.
    EXPECT_EQ(revolute.dof(), 1u);
    EXPECT_EQ(revolute.dofInfos().size(), 1u);
    EXPECT_EQ(prismatic.dofInfos().size(), 1u);
    EXPECT_EQ(planar.dofInfos().size(), 3u);
    EXPECT_EQ(fixed.dofInfos().size(), 0u);
}

TEST(WorkcellTest, ScannerCamerasAndFrameBinding)
{
    auto data = std::make_unique<ScannerData>();
    data->metadata.name  = u8"scanner_model";
    data->metadata.model = u8"LS-100";

    auto base_link = std::make_unique<Link>(u8"base_link");
    data->links.push_back(std::move(base_link));

    auto cam = std::make_unique<Scanner::Camera>();
    cam->frame_name = u8"camera0";
    cam->design_intrinsics.width  = 1920.0;
    cam->design_intrinsics.height = 1080.0;
    data->cameras.push_back(std::move(cam));

    auto proj = std::make_unique<Scanner::Projector>();
    proj->frame_name = u8"projector0";
    proj->design_params.fov_w = 1.2;
    data->projectors.push_back(std::move(proj));

    Scanner scanner(u8"scanner");
    scanner.baseFrame()->setName(u8"camera0");

    // Clone the definition before it is transferred to the scanner.
    auto cloned = data->clone();
    ASSERT_TRUE(cloned);

    scanner.init(std::move(data));

    EXPECT_EQ(scanner.deviceKind(), DeviceKind::Scanner);
    EXPECT_TRUE(scanner.isValid());

    ASSERT_EQ(scanner.cameras().size(), 1u);
    ASSERT_EQ(scanner.projectors().size(), 1u);
    EXPECT_EQ(scanner.cameras()[0]->frame_name, u8"camera0");
    EXPECT_EQ(scanner.projectors()[0]->frame_name, u8"projector0");

    // The camera frame binds to the base frame by name; the projector frame
    // name has no matching frame in the tree and stays null.
    EXPECT_EQ(scanner.cameras()[0]->frame, scanner.baseFrame());
    EXPECT_EQ(scanner.projectors()[0]->frame, nullptr);

    // The camera design pose is the bound frame's fixed transform (identity
    // by default).
    expectNear(scanner.cameras()[0]->designTransform(), Isometry3d{});

    // Cloning preserves cameras but clears the frame binding.
    auto* const cloned_scanner_data = dynamic_cast<ScannerData*>(cloned.get());
    ASSERT_NE(cloned_scanner_data, nullptr);
    ASSERT_EQ(cloned_scanner_data->cameras.size(), 1u);
    EXPECT_EQ(cloned_scanner_data->cameras[0]->frame, nullptr);
    EXPECT_EQ(cloned_scanner_data->cameras[0]->frame_name, u8"camera0");
}

TEST(FrameTest, FrameInWorldAndFrameInFrame)
{
    Frame world;
    world.setName(u8"world");
    Frame j1;
    j1.setName(u8"j1");
    Frame j2;
    j2.setName(u8"j2");
    Frame tool;
    tool.setName(u8"tool");

    world.addChild(&j1);
    j1.addChild(&j2);
    j2.addChild(&tool);

    // Set fixed transforms: world -> j1 (x+10), j1 -> j2 (y+20),
    // j2 -> tool (z+30).
    Isometry3d t1;
    t1.translation = Point3d{ 10.0, 0.0, 0.0 };
    j1.setFixedTransform(t1);
    Isometry3d t2;
    t2.translation = Point3d{ 0.0, 20.0, 0.0 };
    j2.setFixedTransform(t2);
    Isometry3d t3;
    t3.translation = Point3d{ 0.0, 0.0, 30.0 };
    tool.setFixedTransform(t3);

    State state;

    // tool in world: t1 * t2 * t3.
    const auto tool_in_world = Frame::frameInWorld(&tool, state);
    const auto expected = t1 * t2 * t3;
    expectNear(tool_in_world, expected);

    // j1 in world is just t1.
    expectNear(Frame::frameInWorld(&j1, state), t1);

    // frameInFrame(world, j1) maps a point in world to j1 coords.
    const auto world_in_j1 = Frame::frameInFrame(&world, &j1, state);
    expectNear(world_in_j1, t1.inverted());

    // frameInFrame(tool, j2) maps a point in tool to j2 coords.
    expectNear(Frame::frameInFrame(&tool, &j2, state), t3);

    // frameInFrame with the same frame is identity.
    expectNear(Frame::frameInFrame(&j2, &j2, state), Isometry3d{});

    // Frames from different trees are rejected.
    Frame other_world;
    EXPECT_THROW(Frame::frameInFrame(&j1, &other_world, state), std::invalid_argument);
    EXPECT_THROW(Frame::frameInWorld(nullptr, state), std::invalid_argument);
}

TEST(FrameTest, JointTransformUsesState)
{
    // world -> j1 (revolute about world Z).
    Frame world;
    Joint j1(FrameType::RevoluteJoint);
    DofInfo dof;
    dof.type = DofType::RevoluteJoint;
    dof.axis = Vec3d{ 0.0, 0.0, 1.0 };
    j1.setDofInfos({ dof });
    world.addChild(&j1);

    State state;
    state.setup(&world);
    state.qstate(&world).setQ(&j1, Q{ 3.141592653589793 / 2.0 });

    // Rotating 90° about Z maps the local X axis to world +Y.
    const auto j1_in_world = Frame::frameInWorld(&j1, state);
    const auto x_axis = j1_in_world * Vec3d{ 1.0, 0.0, 0.0 };
    EXPECT_NEAR(x_axis[0], 0.0, 1e-9);
    EXPECT_NEAR(x_axis[1], 1.0, 1e-9);
    EXPECT_NEAR(x_axis[2], 0.0, 1e-9);

    // The explicit-q overload matches the state-based transform.
    expectNear(j1.transform(Q{ 3.141592653589793 / 2.0 }), j1_in_world);

    // The identity joint value leaves the frame unchanged.
    state.qstate(&world).setQ(&j1, Q{ 0.0 });
    expectNear(Frame::frameInWorld(&j1, state), Isometry3d{});
}
