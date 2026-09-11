#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraManipulator.hpp>
#include <vine/graphics/OrbitCameraManipulator.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MaterialManager.hpp>
#include <vine/graphics/AxisGizmo.hpp>
#include <vine/graphics/CameraMirror.hpp>
#include <vine/graphics/Ray.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/Colorf.hpp>
#include <vine/math/Transform3.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <set>

using namespace vine::graphics;
using vine::intrusive_ptr;
using vine::Colorf;
using vine::Color;
using vine::math::Vec2d;
using vine::math::Vec3d;
using vine::math::Mat4d;
using vine::math::Aabbd;
using vine::math::Aabbf;

namespace
{

/**
 * @brief Builds a unit triangle mesh in the XY plane.
 *
 * Vertices: (0,0,0), (1,0,0), (0,1,0).
 */
intrusive_ptr<vine::geometry::TriangleMesh> makeUnitTriangle()
{
    auto mesh = intrusive_ptr<vine::geometry::TriangleMesh>(new vine::geometry::TriangleMesh());
    mesh->addTriangle(vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                      vine::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vine::math::Vec3f(0.0f, 1.0f, 0.0f));
    return mesh;
}

/**
 * @brief Builds a node holding a single triangle at a world-space position.
 *
 * @param position World-space translation of the node.
 * @param material Material to apply to the geometry (may be null).
 * @param name     Name assigned to the geometry drawable.
 * @return Node with one triangle drawable.
 */
intrusive_ptr<MatrixTransform> makeTriangleNode(const Vec3d& position, intrusive_ptr<Material> material,
                                                const vine::String& name = {})
{
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);
    geom->setMaterial(std::move(material));
    geom->setName(name);
    node->setMatrix(vine::math::translate(position));
    node->addChild(geom);
    return node;
}

/**
 * @brief Sets an identity Group as the scene's single root.
 *
 * A scene renders exactly one root subtree; tests that need several
 * world-space top-level objects compose them as children of this root.
 *
 * @param scene Scene receiving the root.
 * @return The root Group (tests attach content to it).
 */
intrusive_ptr<Group> setIdentityRoot(Scene& scene)
{
    auto root = intrusive_ptr<Group>(new Group());
    scene.setRoot(root);
    return root;
}

}  // namespace

// ============ Scene ============

TEST(SceneTest, RootSetClearAndFind)
{
    Scene scene;
    scene.setName(u8"test-scene");
    EXPECT_EQ(scene.name(), u8"test-scene");
    EXPECT_EQ(scene.root(), nullptr);

    auto root = intrusive_ptr<Group>(new Group());
    auto node = intrusive_ptr<Node>(new Node());
    node->setName(u8"node1");
    root->addChild(node);
    scene.setRoot(root);

    EXPECT_EQ(scene.root().get(), root.get());
    auto found = scene.findNode(u8"node1");
    EXPECT_EQ(found.get(), node.get());
    EXPECT_EQ(scene.findNode(u8"nope"), nullptr);

    scene.clear();
    EXPECT_EQ(scene.root(), nullptr);
    EXPECT_EQ(scene.findNode(u8"node1"), nullptr);
}

TEST(SceneTest, RootlessSceneIsEmpty)
{
    Scene scene;
    EXPECT_EQ(scene.root(), nullptr);
    EXPECT_EQ(scene.findNode(u8"anything"), nullptr);
    EXPECT_TRUE(scene.boundingBox().isEmpty());

    // Setting a null root leaves the scene empty.
    scene.setRoot(nullptr);
    EXPECT_EQ(scene.root(), nullptr);
}

TEST(SceneTest, BoundingBoxAggregates)
{
    Scene scene;
    auto node = intrusive_ptr<Group>(new Group());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);
    node->addChild(geom);
    scene.setRoot(node);

    Aabbd box = scene.boundingBox();
    EXPECT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().x, 0.0, 1e-9);
    EXPECT_NEAR(box.max().x, 1.0, 1e-9);
    EXPECT_NEAR(box.max().y, 1.0, 1e-9);
}

TEST(SceneTest, InvisibleNodeExcludedFromBoundingBox)
{
    Scene scene;
    auto node = intrusive_ptr<Group>(new Group());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);
    node->addChild(geom);
    node->setVisible(false);
    scene.setRoot(node);

    Aabbd box = scene.boundingBox();
    EXPECT_TRUE(box.isEmpty());
}

// ============ RenderCommand collection / culling ============

/**
 * @brief Configures a camera at (0,0,5) looking at the origin.
 *
 * @param cam Camera to configure in place (avoids the deleted copy ctor).
 */
void setupLookAtCamera(Camera& cam)
{
    cam.setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam.setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);
}

TEST(SceneTest, CollectCommandsSortsOpaqueFrontToBack)
{
    Scene scene;
    // Nearer triangle (z=-2) and farther triangle (z=-5).
    auto near_node = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"near");
    auto far_node = makeTriangleNode(Vec3d(0, 0, -5), nullptr, u8"far");
    auto root = setIdentityRoot(scene);
    root->addChild(far_node);
    root->addChild(near_node);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].geometry->name(), u8"near");
    EXPECT_EQ(commands[1].geometry->name(), u8"far");
    EXPECT_FALSE(commands[0].isTransparent);
}

TEST(SceneTest, CollectCommandsSortsTransparentBackToFrontAfterOpaque)
{
    Scene scene;
    // Opaque far, transparent far, transparent near. Transparency is a leaf
    // MatrixTransform-opacity property, not a material one.
    auto opaque_far = makeTriangleNode(Vec3d(0, 0, -10), nullptr, u8"opaque-far");
    auto trans_far = makeTriangleNode(Vec3d(0, 0, -6), nullptr, u8"trans-far");
    auto trans_near = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"trans-near");
    trans_far->setOpacity(0.5f);
    trans_near->setOpacity(0.5f);
    auto root = setIdentityRoot(scene);
    root->addChild(trans_near);
    root->addChild(opaque_far);
    root->addChild(trans_far);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 3u);
    // Opaque batch first (regardless of depth).
    EXPECT_EQ(commands[0].geometry->name(), u8"opaque-far");
    EXPECT_FALSE(commands[0].isTransparent);
    // Transparent drawn back-to-front.
    EXPECT_EQ(commands[1].geometry->name(), u8"trans-far");
    EXPECT_TRUE(commands[1].isTransparent);
    EXPECT_EQ(commands[2].geometry->name(), u8"trans-near");
    EXPECT_TRUE(commands[2].isTransparent);
}

TEST(SceneTest, CollectCommandsCullsOutOfView)
{
    Scene scene;
    // A node far outside the frustum (behind the camera) must be culled.
    auto visible = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"visible");
    auto behind = makeTriangleNode(Vec3d(0, 0, 100), nullptr, u8"behind");
    auto root = setIdentityRoot(scene);
    root->addChild(behind);
    root->addChild(visible);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"visible");
}

TEST(SceneTest, CollectCommandsCullsOffToTheSide)
{
    Scene scene;
    // Far off to the side, outside the horizontal FOV.
    auto visible = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"visible");
    auto side = makeTriangleNode(Vec3d(50, 0, -3), nullptr, u8"side");
    auto root = setIdentityRoot(scene);
    root->addChild(side);
    root->addChild(visible);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"visible");
}

TEST(SceneTest, CollectCommandsNullCameraYieldsEmpty)
{
    Scene scene;
    auto node = makeTriangleNode(Vec3d(0, 0, -3), nullptr);
    scene.setRoot(node);

    auto commands = scene.collectRenderCommands(nullptr);
    EXPECT_TRUE(commands.empty());
}

TEST(SceneTest, CollectCommandsHidesWholeScene)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    scene.setVisible(false);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    EXPECT_TRUE(commands.empty());
}

TEST(SceneTest, CollectCommandsHidesNode)
{
    Scene scene;
    auto hidden = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"hidden");
    auto shown = makeTriangleNode(Vec3d(0, 0, -5), nullptr, u8"shown");
    hidden->setVisible(false);
    auto root = setIdentityRoot(scene);
    root->addChild(hidden);
    root->addChild(shown);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"shown");
}

TEST(SceneTest, CollectCommandsHidesDrawable)
{
    Scene scene;
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto g1 = intrusive_ptr<Geometry>(new Geometry());
    g1->setShape(makeUnitTriangle());
    g1->setName(u8"g1");
    auto g2 = intrusive_ptr<Geometry>(new Geometry());
    g2->setShape(makeUnitTriangle());
    g2->setName(u8"g2");
    g1->setVisible(false);
    node->addChild(g1);
    node->addChild(g2);
    node->setMatrix(vine::math::translate(Vec3d(0, 0, -3)));
    scene.setRoot(node);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].geometry->name(), u8"g2");
}

TEST(SceneTest, CollectCommandsEffectiveOpacity)
{
    Scene scene;
    auto node = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    scene.setRoot(node);
    scene.setOpacity(0.5f);
    node->setOpacity(0.5f);
    dynamic_cast<Geometry*>(node->children().front().get())->setOpacity(0.5f);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(commands[0].opacity, 0.5f * 0.5f * 0.5f, 1e-5f);
    EXPECT_TRUE(commands[0].isTransparent);
}

TEST(SceneTest, CollectCommandsOpacityLeafAndNode)
{
    Scene scene;
    // Leaf geometry opacity x ancestor (MatrixTransform) opacity; no material
    // involvement.
    auto node = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    node->setOpacity(0.5f);
    dynamic_cast<Geometry*>(node->children().front().get())->setOpacity(0.5f);
    scene.setRoot(node);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(commands[0].opacity, 0.5f * 0.5f, 1e-5f);
    EXPECT_TRUE(commands[0].isTransparent);
}

TEST(SceneTest, CollectCommandsOpacityMultipliesAlongHierarchy)
{
    Scene scene;
    auto parent = intrusive_ptr<Group>(new Group());
    auto child = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    parent->addChild(child);
    parent->setOpacity(0.5f);
    child->setOpacity(0.5f);
    scene.setRoot(parent);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(commands[0].opacity, 0.25f, 1e-5f);
    EXPECT_TRUE(commands[0].isTransparent);
}

// ============ Camera ============

TEST(CameraTest, Defaults)
{
    Camera cam;
    EXPECT_EQ(cam.projectionType(), Camera::ProjectionType::Perspective);
    EXPECT_NEAR(cam.fieldOfView(), 45.0, 1e-9);
    EXPECT_NEAR(cam.nearPlane(), 0.1, 1e-9);
    EXPECT_NEAR(cam.farPlane(), 1000.0, 1e-9);
    EXPECT_EQ(cam.eye(), Vec3d(0, 0, 5));
    EXPECT_EQ(cam.target(), Vec3d(0, 0, 0));
    EXPECT_EQ(cam.up(), Vec3d(0, 1, 0));
}

TEST(CameraTest, ViewMatrixLooksAtTarget)
{
    Camera cam;
    cam.setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    Mat4d view = cam.viewMatrix();
    // Camera looking down -Z: the origin should be at z = -5 in view space.
    const auto origin_in_view = view * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin_in_view.x, 0.0, 1e-6);
    EXPECT_NEAR(origin_in_view.y, 0.0, 1e-6);
    EXPECT_NEAR(origin_in_view.z, -5.0, 1e-6);
}

TEST(CameraTest, ScreenToWorldRayCenter)
{
    Camera cam;
    cam.setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam.setProjectionMatrixAsPerspective(60.0, 800.0 / 600.0, 0.1, 1000.0);

    // Screen center maps to a ray through the target.
    Ray ray = cam.screenToWorldRay(Vec2d(0.5, 0.5));
    EXPECT_NEAR(ray.origin.x, 0.0, 1e-6);
    EXPECT_NEAR(ray.origin.y, 0.0, 1e-6);
    EXPECT_NEAR(ray.origin.z, 5.0, 1e-6);
    // Direction should point towards -Z.
    EXPECT_NEAR(ray.direction.z, -1.0, 1e-6);
    EXPECT_NEAR(ray.direction.x, 0.0, 1e-6);
}

// ============ CameraManipulator ============

TEST(CameraManipulatorTest, OrbitKeepsTarget)
{
    auto cam = intrusive_ptr<Camera>(new Camera());
    // Z-up framing: eye in the x = 0 plane, elevated above the horizon, so a
    // yaw sweep rotates the eye off the original azimuth.
    cam->setViewMatrixAsLookAt(Vec3d(0, 10, 6), Vec3d(0, 0, 0), Vec3d(0, 0, 1));

    OrbitCameraManipulator manip(cam.get());
    manip.orbit(0.5, 0.0);

    // Target remains fixed at origin.
    EXPECT_NEAR(cam->target().x, 0.0, 1e-6);
    EXPECT_NEAR(cam->target().y, 0.0, 1e-6);
    EXPECT_NEAR(cam->target().z, 0.0, 1e-6);
    // Eye moved off the original axis (x was 0 before the orbit).
    EXPECT_GT(std::abs(cam->eye().x), 1e-3);
}

TEST(CameraManipulatorTest, ZoomChangesRadius)
{
    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 10), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    OrbitCameraManipulator manip(cam.get());
    const double before = manip.orbitRadius();
    manip.zoom(0.5);
    EXPECT_LT(manip.orbitRadius(), before);
    const double distance = (cam->eye() - cam->target()).length();
    EXPECT_NEAR(distance, manip.orbitRadius(), 1e-6);
}

TEST(CameraManipulatorTest, PressOnGeometryKeepsCentre)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(0, 0, 0), nullptr, u8"tri"));

    auto cam = intrusive_ptr<Camera>(new Camera());
    // Aim at a point BEHIND the triangle plane, so the centre-pixel ray hits
    // the triangle surface at a point different from the current target.
    const Vec3d eye(0, 0, 5);
    const Vec3d target(0.3, 0.3, -6.0);
    cam->setViewMatrixAsLookAt(eye, target, Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vine::window::ResizeEvent{ 800, 600 });

    Vec3d hit;
    ASSERT_TRUE(manip.pickAt(400.0, 300.0, hit));
    EXPECT_NEAR(hit.z, 0.0, 1e-6);  // on the triangle plane (in front of target)

    vine::window::MouseEvent press;
    press.button = vine::window::MouseButton::Left;
    press.x = 400.0;
    press.y = 300.0;
    press.pressed = true;
    manip.onMousePress(press);

    // A rotate press on the model must not re-aim or jump: the orbit centre
    // and the eye both stay put until the user actually drags (the pressed
    // point is then used as the rotation pivot).
    EXPECT_NEAR(manip.orbitCenter().x, target.x, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().y, target.y, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().z, target.z, 1e-6);
    EXPECT_NEAR(cam->eye().x, eye.x, 1e-6);
    EXPECT_NEAR(cam->eye().y, eye.y, 1e-6);
    EXPECT_NEAR(cam->eye().z, eye.z, 1e-6);
}

TEST(CameraManipulatorTest, PressOnEmptyKeepsCentre)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(2, 0, -3), nullptr, u8"tri"));

    auto cam = intrusive_ptr<Camera>(new Camera());
    // Look at the origin; the triangle sits off to the right, so the centre
    // pixel ray misses it.
    const Vec3d eye(0, 0, 5);
    cam->setViewMatrixAsLookAt(eye, Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vine::window::ResizeEvent{ 800, 600 });

    Vec3d hit;
    EXPECT_FALSE(manip.pickAt(400.0, 300.0, hit));

    vine::window::MouseEvent press;
    press.button = vine::window::MouseButton::Left;
    press.x = 400.0;
    press.y = 300.0;
    press.pressed = true;
    manip.onMousePress(press);

    // Pressing empty space must not move the centre either.
    EXPECT_NEAR(manip.orbitCenter().x, 0.0, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().y, 0.0, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().z, 0.0, 1e-6);
    EXPECT_NEAR(cam->eye().x, eye.x, 1e-6);
    EXPECT_NEAR(cam->eye().y, eye.y, 1e-6);
    EXPECT_NEAR(cam->eye().z, eye.z, 1e-6);
}

TEST(CameraManipulatorTest, RotateDragPivotsAboutAnchorPinnedUnderCursor)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(2, 0, -3), nullptr, u8"tri"));
    const vine::math::Point3d box_c = scene.boundingBox().center();
    // The rotate pivot for a press on empty space is the scene centre.
    const Vec3d anchor(box_c.x, box_c.y, box_c.z);

    auto cam = intrusive_ptr<Camera>(new Camera());
    // Look at the origin; the triangle sits off to the right, so the centre
    // pixel ray misses it (the rotate press happens on empty space).
    const Vec3d eye(0, 0, 5);
    cam->setViewMatrixAsLookAt(eye, Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vine::window::ResizeEvent{ 800, 600 });

    Vec3d hit;
    EXPECT_FALSE(manip.pickAt(400.0, 300.0, hit));

    // Project a world point to viewport pixels using the camera.
    const auto project = [&](const Vec3d& p) {
        const double w = 800.0;
        const double h = 600.0;
        const Vec3d fwd = (cam->target() - cam->eye()).normalized();
        const Vec3d right = fwd.cross(cam->up()).normalized();
        const Vec3d up = right.cross(fwd).normalized();
        const Vec3d d = p - cam->eye();
        const double lz = d.dot(fwd);
        const double lx = d.dot(right);
        const double ly = d.dot(up);
        const double tan_half = std::tan(cam->fieldOfView() * vine::math::DEG_TO_RAD * 0.5);
        const double aspect = cam->aspectRatio();
        const double ndc_x = lx / (lz * tan_half * aspect);
        const double ndc_y = ly / (lz * tan_half);
        return std::make_pair((ndc_x * 0.5 + 0.5) * w, (0.5 - ndc_y * 0.5) * h);
    };

    vine::window::MouseEvent press;
    press.button = vine::window::MouseButton::Left;
    press.x = 400.0;
    press.y = 300.0;
    press.pressed = true;
    manip.onMousePress(press);

    const auto before = project(anchor);
    const double dist0 = (cam->eye() - cam->target()).length();

    // A long, same-direction rotate drag on the empty area.
    const std::pair<double, double> steps[] = {
        { 410.0, 300.0 }, { 420.0, 301.0 }, { 432.0, 303.0 }, { 446.0, 305.0 },
        { 462.0, 308.0 }, { 480.0, 310.0 }, { 498.0, 312.0 }, { 520.0, 315.0 },
    };
    for (const auto& [x, y] : steps) {
        vine::window::MouseEvent move;
        move.x = x;
        move.y = y;
        move.pressed = true;
        manip.onMouseMove(move);
    }

    // The rigid pivot rotation keeps the anchor (scene centre) pinned at its
    // original screen position and the eye-to-target radius constant, so the
    // scene turns rigidly about the grabbed point - no drift, no size change.
    const auto after = project(anchor);
    EXPECT_NEAR(after.first, before.first, 2.0);
    EXPECT_NEAR(after.second, before.second, 2.0);
    EXPECT_NEAR((cam->eye() - cam->target()).length(), dist0, 1e-6);
}

TEST(CameraManipulatorTest, PivotDragReachesTopAndRollsOverWithoutFlip)
{
    auto cam = intrusive_ptr<Camera>(new Camera());
    // Start looking at the origin from above at a ~63 deg elevation; no scene
    // is bound, so the anchor (and pivot) is the origin on the view axis.
    cam->setViewMatrixAsLookAt(Vec3d(0.0, 3.0, 1.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get());
    manip.onResize(vine::window::ResizeEvent{ 800, 600 });

    vine::window::MouseEvent press;
    press.button = vine::window::MouseButton::Left;
    press.x = 400.0;
    press.y = 300.0;
    press.pressed = true;
    manip.onMousePress(press);

    const Vec3d world_up(0.0, 1.0, 0.0);

    vine::window::MouseEvent move;
    move.pressed = true;
    move.x = 400.0;

    // A long downward drag sweeps over the top. The rigid pivot drag carries
    // the up with it, so the view reaches the true top (fwd.y = -1) and is
    // free to keep going past the pole onto the far side; throughout, the up
    // must stay continuous (never a sudden 180 deg flip).
    double min_fwd_up = 1.0;
    Vec3d prev_up = cam->up();
    for (int i = 0; i < 300; ++i) {
        move.y = 300.0 + (i + 1);
        manip.onMouseMove(move);
        const Vec3d f = (cam->target() - cam->eye()).normalized();
        if (f.dot(world_up) < min_fwd_up) {
            min_fwd_up = f.dot(world_up);
        }
        const Vec3d u = cam->up();
        EXPECT_GT(u.dot(prev_up), -0.5);  // up never flips 180 deg
        prev_up = u;
    }

    // A true top-down view (looking straight down at the anchor) was reached.
    EXPECT_LE(min_fwd_up, -0.9999);
    // ... and the drag kept going past the pole to the far side: the defining
    // freedom of the roll-capable pivot (a level turntable would hold at the
    // pole instead).
    const Vec3d fwd_end = (cam->target() - cam->eye()).normalized();
    EXPECT_GT(fwd_end.dot(world_up), -0.5);

    // Reversing the drag rolls back over the pole towards the horizon.
    for (int i = 0; i < 400; ++i) {
        move.y -= 1.0;
        manip.onMouseMove(move);
        const Vec3d u = cam->up();
        EXPECT_GT(u.dot(prev_up), -0.5);
        prev_up = u;
    }
    const Vec3d fwd = (cam->target() - cam->eye()).normalized();
    EXPECT_GT(fwd.dot(world_up), -0.5);  // no longer looking straight down
}

TEST(CameraManipulatorTest, SetCenterFromScreenRecentersOnPick)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(0, 0, 0), nullptr, u8"tri"));

    auto cam = intrusive_ptr<Camera>(new Camera());
    // Camera target is behind the triangle; explicit re-centring should move
    // the orbit centre onto the picked surface point while the eye stays.
    const Vec3d eye(0, 0, 5);
    cam->setViewMatrixAsLookAt(eye, Vec3d(0.3, 0.3, -6.0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    manip.onResize(vine::window::ResizeEvent{ 800, 600 });

    EXPECT_TRUE(manip.setCenterFromScreen(400.0, 300.0));
    // The centre snapped to the surface hit (on the z=0 plane), not the target.
    EXPECT_NEAR(manip.orbitCenter().z, 0.0, 1e-6);
    EXPECT_GT(manip.orbitCenter().x, 0.05);
    EXPECT_NEAR(cam->eye().x, eye.x, 1e-6);
    EXPECT_NEAR(cam->eye().y, eye.y, 1e-6);
    EXPECT_NEAR(cam->eye().z, eye.z, 1e-6);
}

TEST(CameraManipulatorTest, ZoomClampsAtMinimumDistance)
{
    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 10), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    OrbitCameraManipulator manip(cam.get());
    manip.setMinDistance(2.0);
    manip.zoom(1e-6);
    EXPECT_NEAR(manip.orbitRadius(), 2.0, 1e-6);
    EXPECT_NEAR((cam->eye() - cam->target()).length(), manip.orbitRadius(), 1e-6);
}

TEST(CameraManipulatorTest, FitToScreenFramesScene)
{
    Scene scene;
    scene.setRoot(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    const Aabbd box = scene.boundingBox();

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 10), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    cam->setProjectionMatrixAsPerspective(60.0, 1.0, 0.1, 1000.0);

    OrbitCameraManipulator manip(cam.get(), &scene);
    EXPECT_TRUE(manip.fitToScreen());
    EXPECT_NEAR(manip.orbitCenter().x, box.center().x, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().y, box.center().y, 1e-6);
    EXPECT_NEAR(manip.orbitCenter().z, box.center().z, 1e-6);
    const double half_diag = (box.size() * 0.5).length();
    EXPECT_GT(manip.orbitRadius(), half_diag);
    EXPECT_NEAR((cam->eye() - cam->target()).length(), manip.orbitRadius(), 1e-6);
}

TEST(CameraManipulatorTest, HomeRestoresInitialView)
{
    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 10), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    OrbitCameraManipulator manip(cam.get());
    manip.orbit(1.1, -0.4);
    manip.zoom(0.4);
    manip.home();

    EXPECT_NEAR(cam->target().x, 0.0, 1e-6);
    EXPECT_NEAR(cam->target().y, 0.0, 1e-6);
    EXPECT_NEAR(cam->target().z, 0.0, 1e-6);
    EXPECT_NEAR(manip.orbitRadius(), 10.0, 1e-6);
    EXPECT_NEAR(cam->eye().x, 0.0, 1e-6);
    EXPECT_NEAR(cam->eye().y, 0.0, 1e-6);
    EXPECT_NEAR(cam->eye().z, 10.0, 1e-6);
}

// ============ Ray ============

TEST(RayTest, PointAt)
{
    Ray ray(Vec3d(1, 2, 3), Vec3d(1, 0, 0));
    Vec3d p = ray.pointAt(4.0);
    EXPECT_NEAR(p.x, 5.0, 1e-9);
    EXPECT_NEAR(p.y, 2.0, 1e-9);
    EXPECT_NEAR(p.z, 3.0, 1e-9);
}

TEST(RayTest, DistanceToPoint)
{
    Ray ray(Vec3d(0, 0, 0), Vec3d(1, 0, 0));
    EXPECT_NEAR(ray.distanceToPoint(Vec3d(5, 3, 0)), 3.0, 1e-9);
    // Point behind the ray origin: distance to origin.
    EXPECT_NEAR(ray.distanceToPoint(Vec3d(-5, 0, 0)), 5.0, 1e-9);
}

// ============ RayIntersection ============

TEST(RayIntersectionTest, HitsTriangle)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);

    Ray ray(Vec3d(0.25, 0.25, 1.0), Vec3d(0, 0, -1));
    RayIntersectionResult result = RayIntersection::intersect(ray, geom.get(), Mat4d());
    EXPECT_TRUE(result.hit);
    EXPECT_NEAR(result.distance, 1.0, 1e-6);
    EXPECT_NEAR(result.point.x, 0.25, 1e-6);
    EXPECT_NEAR(result.point.y, 0.25, 1e-6);
    // Normal of CCW triangle in XY plane points +Z.
    EXPECT_NEAR(result.normal.z, 1.0, 1e-6);
}

TEST(RayIntersectionTest, MissesTriangle)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);

    // Ray aimed outside the triangle.
    Ray ray(Vec3d(5, 5, 1), Vec3d(0, 0, -1));
    RayIntersectionResult result = RayIntersection::intersect(ray, geom.get(), Mat4d());
    EXPECT_FALSE(result.hit);
}

TEST(RayIntersectionTest, SceneQuery)
{
    auto scene = intrusive_ptr<Scene>(new Scene());
    auto node = intrusive_ptr<Group>(new Group());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setName(u8"tri");
    geom->setShape(mesh);
    node->addChild(geom);
    scene->setRoot(node);

    Ray ray(Vec3d(0.25, 0.25, 1.0), Vec3d(0, 0, -1));
    RayIntersectionResult result = RayIntersection::intersectScene(ray, scene.get());
    EXPECT_TRUE(result.hit);
    EXPECT_EQ(result.geometry.get(), geom.get());
}

TEST(RayIntersectionTest, AllHitsCollectsEveryTriangleSortedByDepth)
{
    // One geometry with two front-facing triangles stacked along -Z: a ray
    // through their centres pierces both, so AllHits returns both hits sorted
    // near to far while Nearest returns only the closer one.
    auto scene = intrusive_ptr<Scene>(new Scene());
    auto node = intrusive_ptr<Group>(new Group());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = intrusive_ptr<vine::geometry::TriangleMesh>(new vine::geometry::TriangleMesh());
    mesh->addTriangle(vine::math::Vec3f(0.0f, 0.0f, 0.0f),
                      vine::math::Vec3f(1.0f, 0.0f, 0.0f),
                      vine::math::Vec3f(0.0f, 1.0f, 0.0f));
    mesh->addTriangle(vine::math::Vec3f(0.0f, 0.0f, -2.0f),
                      vine::math::Vec3f(1.0f, 0.0f, -2.0f),
                      vine::math::Vec3f(0.0f, 1.0f, -2.0f));
    geom->setShape(mesh);
    node->addChild(geom);
    scene->setRoot(node);

    Ray ray(Vec3d(0.25, 0.25, 1.0), Vec3d(0, 0, -1));

    // Default AllHits: every triangle hit, sorted by depth.
    const auto all = RayIntersection::intersectSceneAll(ray, scene.get());
    ASSERT_EQ(all.size(), 2u);
    EXPECT_NEAR(all[0].distance, 1.0, 1e-6);
    EXPECT_NEAR(all[0].point.z, 0.0, 1e-6);
    EXPECT_NEAR(all[1].distance, 3.0, 1e-6);
    EXPECT_NEAR(all[1].point.z, -2.0, 1e-6);
    EXPECT_EQ(all[0].geometry.get(), geom.get());
    EXPECT_EQ(all[1].geometry.get(), geom.get());

    // Nearest mode keeps only the closest hit per geometry.
    const auto nearest = RayIntersection::intersectSceneAll(ray, scene.get(),
                                                            RayIntersection::Mode::Nearest);
    ASSERT_EQ(nearest.size(), 1u);
    EXPECT_NEAR(nearest[0].distance, 1.0, 1e-6);

    RayIntersectionResult single = RayIntersection::intersectScene(ray, scene.get());
    EXPECT_TRUE(single.hit);
    EXPECT_NEAR(single.distance, 1.0, 1e-6);
}

// ============ Material ============

TEST(MaterialTest, Defaults)
{
    Material mat;
    EXPECT_NEAR(mat.shininess(), 32.0f, 1e-6f);
    EXPECT_NEAR(mat.diffuse().r, 0.8f, 1e-6f);
}

TEST(MaterialTest, Setters)
{
    Material mat;
    mat.setName(u8"red");
    mat.setDiffuse(Colorf(1.0f, 0.0f, 0.0f, 1.0f));
    mat.setShininess(64.0f);
    mat.setTextureFile(u8"tex.png");

    EXPECT_EQ(mat.name(), u8"red");
    EXPECT_NEAR(mat.diffuse().r, 1.0f, 1e-6f);
    EXPECT_NEAR(mat.shininess(), 64.0f, 1e-6f);
    EXPECT_EQ(mat.textureFile(), u8"tex.png");
}

// ============ Geometry ============

TEST(GeometryTest, MeshCounts)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);

    EXPECT_EQ(geom->vertexCount(), 3u);
}

TEST(GeometryTest, NoShapeYieldsZero)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    EXPECT_EQ(geom->vertexCount(), 0u);
    EXPECT_FALSE(geom->hasPositions());
    EXPECT_FALSE(geom->hasNormals());
    EXPECT_FALSE(geom->hasIndices());
    EXPECT_TRUE(geom->boundingBox().isEmpty());
}

// ============ Node ============

TEST(NodeTest, LeafGeometryBinding)
{
    Group node;
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    node.addChild(geom);
    EXPECT_EQ(node.children().size(), 1u);
    EXPECT_EQ(node.children()[0].get(), geom.get());
    node.removeChild(geom.get());
    EXPECT_EQ(node.children().size(), 0u);
}

TEST(NodeTest, MultipleLeafGeometries)
{
    Group node;
    auto geom1 = intrusive_ptr<Geometry>(new Geometry());
    auto geom2 = intrusive_ptr<Geometry>(new Geometry());
    node.addChild(geom1);
    node.addChild(geom2);

    EXPECT_EQ(node.children().size(), 2u);
    EXPECT_EQ(node.children()[0].get(), geom1.get());
    EXPECT_EQ(node.children()[1].get(), geom2.get());
}

TEST(NodeTest, ChildHierarchy)
{
    auto parent = intrusive_ptr<Group>(new Group());
    auto child = intrusive_ptr<Node>(new Node());
    parent->addChild(child);

    EXPECT_EQ(parent->children().size(), 1u);
    EXPECT_EQ(child->parent(), parent.get());

    parent->removeChild(child.get());
    EXPECT_EQ(parent->children().size(), 0u);
    EXPECT_EQ(child->parent(), nullptr);
}

TEST(NodeTest, WorldTransformCascades)
{
    auto parent = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto child = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    parent->addChild(child);

    // Parent translates by (1, 0, 0), child by (2, 0, 0).
    parent->setMatrix(vine::math::translate(Vec3d(1, 0, 0)));
    child->setMatrix(vine::math::translate(Vec3d(2, 0, 0)));

    // Child world origin = parent translation * child translation = (3, 0, 0).
    const auto origin = child->worldMatrix() * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin.x, 3.0, 1e-9);
    EXPECT_NEAR(origin.y, 0.0, 1e-9);
    EXPECT_NEAR(origin.z, 0.0, 1e-9);
}

TEST(NodeTest, BoundingBoxWithTransform)
{
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();
    geom->setShape(mesh);
    node->addChild(geom);
    node->setMatrix(vine::math::translate(Vec3d(10, 0, 0)));

    Aabbd box = node->boundingBox();
    // Local box [0,1]x[0,1] translated by +10 on X.
    EXPECT_NEAR(box.min().x, 10.0, 1e-9);
    EXPECT_NEAR(box.max().x, 11.0, 1e-9);
    EXPECT_NEAR(box.max().y, 1.0, 1e-9);
}

// ============ MatrixTransform / Group ============

TEST(MatrixTransformTest, MatrixRoundTrip)
{
    MatrixTransform mt;
    // Default matrix is identity.
    EXPECT_TRUE(mt.matrix().isIdentity());

    const Mat4d m = vine::math::translate(Vec3d(1, 2, 3));
    mt.setMatrix(m);
    EXPECT_NEAR(mt.matrix().element(0, 3), 1.0, 1e-9);
    EXPECT_NEAR(mt.matrix().element(1, 3), 2.0, 1e-9);
    EXPECT_NEAR(mt.matrix().element(2, 3), 3.0, 1e-9);
}

TEST(MatrixTransformTest, WorldMatrixAccumulatesNested)
{
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vine::math::translate(Vec3d(1, 0, 0)));
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vine::math::translate(Vec3d(0, 2, 0)));
    outer->addChild(inner);

    // Outer translation then inner translation: (1, 2, 0).
    const auto p = inner->worldMatrix() * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(p.x, 1.0, 1e-9);
    EXPECT_NEAR(p.y, 2.0, 1e-9);
    EXPECT_NEAR(p.z, 0.0, 1e-9);
}

TEST(MatrixTransformTest, PlainGroupPassesTransformThrough)
{
    auto root = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    root->setMatrix(vine::math::translate(Vec3d(1, 0, 0)));
    auto group = intrusive_ptr<Group>(new Group());
    auto leaf = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    leaf->setMatrix(vine::math::translate(Vec3d(0, 0, 3)));
    root->addChild(group);
    group->addChild(leaf);

    // A plain Group contributes no matrix of its own.
    const auto p = leaf->worldMatrix() * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(p.x, 1.0, 1e-9);
    EXPECT_NEAR(p.y, 0.0, 1e-9);
    EXPECT_NEAR(p.z, 3.0, 1e-9);
}

TEST(MatrixTransformTest, NestedBoundingBoxIsWorld)
{
    auto outer = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outer->setMatrix(vine::math::translate(Vec3d(10, 0, 0)));
    auto inner = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inner->setMatrix(vine::math::translate(Vec3d(0, 5, 0)));
    outer->addChild(inner);

    auto geom = intrusive_ptr<Geometry>(new Geometry());
    geom->setShape(makeUnitTriangle());
    inner->addChild(geom);

    // World box: unit triangle [0,1]^2 placed by both translations.
    const Aabbd box = outer->boundingBox();
    EXPECT_NEAR(box.min().x, 10.0, 1e-9);
    EXPECT_NEAR(box.max().x, 11.0, 1e-9);
    EXPECT_NEAR(box.min().y, 5.0, 1e-9);
    EXPECT_NEAR(box.max().y, 6.0, 1e-9);
    EXPECT_NEAR(box.min().z, 0.0, 1e-9);
    EXPECT_NEAR(box.max().z, 0.0, 1e-9);
}

TEST(MatrixTransformTest, WorldMatrixIsOwnMatrixForSingleNode)
{
    // A lone MatrixTransform's worldMatrix is exactly its own matrix (no
    // self-doubling when folding the parent chain).
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    node->setMatrix(vine::math::translate(Vec3d(4, 0, 0)));
    const auto p = node->worldMatrix() * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(p.x, 4.0, 1e-9);
}

TEST(GroupTest, ReParentDetachesFromOldParent)
{
    auto first = intrusive_ptr<Group>(new Group());
    auto second = intrusive_ptr<Group>(new Group());
    auto child = intrusive_ptr<Node>(new Node());
    first->addChild(child);
    second->addChild(child);

    EXPECT_EQ(first->children().size(), 0u);
    EXPECT_EQ(second->children().size(), 1u);
    EXPECT_EQ(second->children()[0].get(), child.get());
    EXPECT_EQ(child->parent(), second.get());
}

TEST(SceneTest, CollectCommandsBakesNestedWorldMatrix)
{
    Scene scene;
    auto root = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    root->setMatrix(vine::math::translate(Vec3d(0, 0, -5)));
    auto holder = makeTriangleNode(Vec3d(0, 0, 0), nullptr, u8"tri");
    root->addChild(holder);
    scene.setRoot(root);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    // The command's model matrix is the folded world transform of the leaf.
    const auto origin = commands[0].modelMatrix * vine::math::Point3d(0, 0, 0);
    EXPECT_NEAR(origin.x, 0.0, 1e-9);
    EXPECT_NEAR(origin.y, 0.0, 1e-9);
    EXPECT_NEAR(origin.z, -5.0, 1e-9);
}

TEST(GeometryTest, CountsStayDataStatsUnderPointsTopology)
{
    // Topology lives on StateNode render state, NOT on the geometry data:
    // the vertex count is a pure data statistic and must not change when an
    // enclosing StateNode overrides the draw topology.
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    geom->setShape(makeUnitTriangle());  // 3 vertices
    EXPECT_EQ(geom->vertexCount(), 3u);

    Scene scene;
    auto state = intrusive_ptr<StateNode>(new StateNode());
    state->setTopology(Topology::Points);
    auto holder = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    state->addChild(holder);
    scene.setRoot(state);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    // The draw topology rides the resolved state; the data counts stay.
    EXPECT_EQ(commands[0].renderState.topology, Topology::Points);
    EXPECT_EQ(commands[0].geometry->vertexCount(), 3u);
}

TEST(ShaderProgramTest, AggregatesStages)
{
    ShaderProgram program;
    program.setName(u8"custom");
    EXPECT_EQ(program.name(), u8"custom");
    EXPECT_EQ(program.stageCount(), 0u);
    EXPECT_TRUE(program.stages().empty());

    ShaderStage vs;
    vs.type = ShaderStageType::Vertex;
    vs.source = u8"#version 450\nvoid main(){ gl_Position = vec4(0.0); }\n";
    program.addStage(vs);

    ShaderStage fs;
    fs.type = ShaderStageType::Fragment;
    fs.source = u8"#version 450\nvoid main(){ outColor = vec4(1.0); }\n";
    fs.entryPoint = u8"custom_main";
    program.addStage(fs);

    EXPECT_EQ(program.stageCount(), 2u);
    ASSERT_NE(program.stage(0), nullptr);
    EXPECT_EQ(program.stage(0)->type, ShaderStageType::Vertex);
    EXPECT_EQ(program.stage(0)->entryPoint, u8"main");
    EXPECT_FALSE(program.stage(0)->source.empty());
    ASSERT_NE(program.stage(1), nullptr);
    EXPECT_EQ(program.stage(1)->type, ShaderStageType::Fragment);
    EXPECT_EQ(program.stage(1)->entryPoint, u8"custom_main");
    EXPECT_EQ(program.stage(99), nullptr);

    const auto& all = program.stages();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].source, vs.source);
}

TEST(GeometryTest, ProgramSlotDefaultsNull)
{
    Geometry geom;
    EXPECT_EQ(geom.program(), nullptr);

    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"flat-red");
    geom.setProgram(program);
    EXPECT_EQ(geom.program(), program.get());

    geom.setProgram(nullptr);
    EXPECT_EQ(geom.program(), nullptr);
}


TEST(MeshTest, AabbCacheFromPositions)
{
    auto mesh = makeUnitTriangle();
    // Cache starts empty (inverted sentinel) until set or computed.
    EXPECT_FALSE(mesh->aabb().isValid());
    EXPECT_TRUE(mesh->aabb().isEmpty());

    const Aabbf box = mesh->computeAabb();
    EXPECT_TRUE(box.isValid());
    EXPECT_FLOAT_EQ(box.min().x, 0.0f);
    EXPECT_FLOAT_EQ(box.min().y, 0.0f);
    EXPECT_FLOAT_EQ(box.min().z, 0.0f);
    EXPECT_FLOAT_EQ(box.max().x, 1.0f);
    EXPECT_FLOAT_EQ(box.max().y, 1.0f);
    EXPECT_FLOAT_EQ(box.max().z, 0.0f);
    // computeAabb caches the result for aabb().
    EXPECT_TRUE(mesh->aabb() == box);
}

TEST(MeshTest, AabbCacheManualSet)
{
    auto mesh = makeUnitTriangle();
    const Aabbf custom(-2.0f, -3.0f, -4.0f, 5.0f, 6.0f, 7.0f);
    mesh->setAabb(custom);
    EXPECT_TRUE(mesh->aabb() == custom);

    // Geometry edits do not touch the manually cached box.
    mesh->setPositions({ vine::math::Vec3f(9.0f, 9.0f, 9.0f) });
    EXPECT_TRUE(mesh->aabb() == custom);
}

TEST(MeshTest, AttributesSharedOnBase)
{
    auto mesh = makeUnitTriangle();
    EXPECT_EQ(mesh->vertexCount(), 3u);
    EXPECT_EQ(mesh->positions().size(), 3u);
    EXPECT_EQ(mesh->triangleCount(), 1u);

    // Position/normal/texcoord storage lives on the Mesh base.
    mesh->setPositions({ vine::math::Vec3f(0, 0, 0), vine::math::Vec3f(2, 0, 0),
                         vine::math::Vec3f(0, 2, 0) });
    EXPECT_EQ(mesh->vertexCount(), 3u);
    EXPECT_EQ(mesh->positions()[1].x, 2.0f);

    mesh->setNormals({ vine::math::Vec3f(0, 0, 1), vine::math::Vec3f(0, 0, 1),
                      vine::math::Vec3f(0, 0, 1) });
    EXPECT_EQ(mesh->normals().size(), 3u);
}

TEST(GeometryTest, BoundingBoxComputedFromBuffers)
{
    auto geom = intrusive_ptr<Geometry>(new Geometry());
    auto mesh = makeUnitTriangle();

    // setShape fills the geometry's buffers; the box derives from them.
    geom->setShape(mesh);
    const Aabbd box = geom->boundingBox();
    EXPECT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().x, 0.0, 1e-9);
    EXPECT_NEAR(box.min().y, 0.0, 1e-9);
    EXPECT_NEAR(box.min().z, 0.0, 1e-9);
    EXPECT_NEAR(box.max().x, 1.0, 1e-9);
    EXPECT_NEAR(box.max().y, 1.0, 1e-9);
    EXPECT_NEAR(box.max().z, 0.0, 1e-9);
}


// ============ RenderTarget ============

TEST(RenderTargetTest, OffscreenDescription)
{
    RenderTarget rt;
    EXPECT_FALSE(rt.valid());
    EXPECT_FALSE(rt.hasColor());
    EXPECT_FALSE(rt.hasDepth());

    rt.setSize(320, 240);
    rt.attachColor(RenderTarget::ColorFormat::RGBA8);
    rt.attachDepth(RenderTarget::DepthFormat::D24);

    EXPECT_TRUE(rt.hasColor());
    EXPECT_TRUE(rt.hasDepth());
    EXPECT_EQ(rt.colorFormat(), RenderTarget::ColorFormat::RGBA8);
    EXPECT_EQ(rt.depthFormat(), RenderTarget::DepthFormat::D24);
    EXPECT_TRUE(rt.valid());
    EXPECT_EQ(rt.width(), 320);
    EXPECT_EQ(rt.height(), 240);

    // Zero-size target is never usable, even with attachments attached.
    rt.setSize(0, 0);
    EXPECT_FALSE(rt.valid());
}

TEST(RenderTargetTest, MultipleColorAttachments)
{
    RenderTarget rt;
    EXPECT_EQ(rt.colorCount(), 0);
    EXPECT_FALSE(rt.hasColor());
    // Out-of-range format access falls back to the default RGBA8.
    EXPECT_EQ(rt.colorFormat(0), RenderTarget::ColorFormat::RGBA8);

    // A G-buffer / MRT target: several colour attachments, one per texture.
    rt.setSize(640, 360);
    rt.attachColor(RenderTarget::ColorFormat::RGBA8);   // att 0: albedo
    rt.attachColor(RenderTarget::ColorFormat::RGBA16F); // att 1: normal
    rt.attachColor(RenderTarget::ColorFormat::RGBA32F); // att 2: view position
    rt.attachDepth(RenderTarget::DepthFormat::D24);

    EXPECT_TRUE(rt.hasColor());
    EXPECT_EQ(rt.colorCount(), 3);
    EXPECT_EQ(rt.colorFormat(), RenderTarget::ColorFormat::RGBA8); // attachment 0 unchanged
    EXPECT_EQ(rt.colorFormat(0), RenderTarget::ColorFormat::RGBA8);
    EXPECT_EQ(rt.colorFormat(1), RenderTarget::ColorFormat::RGBA16F);
    EXPECT_EQ(rt.colorFormat(2), RenderTarget::ColorFormat::RGBA32F);
    // Out-of-range index keeps the safe default.
    EXPECT_EQ(rt.colorFormat(3), RenderTarget::ColorFormat::RGBA8);
    EXPECT_EQ(rt.colorFormat(-1), RenderTarget::ColorFormat::RGBA8);
    EXPECT_TRUE(rt.hasDepth());
    EXPECT_TRUE(rt.valid());
}

// ============ RenderEngine ============

/**
 * @brief Recording backend that counts calls for verification.
 */
class MockBackend : public RenderBackend {
  public:
    bool ok = false;
    int begin_calls = 0;
    int end_calls = 0;
    int swap_calls = 0;
    int render_calls = 0;
    int clear_calls = 0;
    int viewport_sets = 0;
    const Camera* last_camera = nullptr;
    int last_viewport[4] = { 0, 0, 0, 0 };
    // Programs of the commands last passed to render() (per command).
    std::vector<const ShaderProgram*> last_programs;
    int screen_draws = 0;
    RenderTarget* last_screen_source = nullptr;
    int last_screen_attachment = 0;
    int light_sets = 0;
    std::size_t last_light_count = 0;
    const Light* last_light = nullptr;
    int preset_sets = 0;
    ShaderPreset last_preset = ShaderPreset::StandardPhong;

    bool initialize() override { ok = true; return true; }
    void setShaderPreset(ShaderPreset preset) override
    {
        last_preset = preset;
        ++preset_sets;
    }
    void shutdown() override { ok = false; }
    void beginFrame() override { ++begin_calls; }
    void endFrame() override { ++end_calls; }
    void setRenderTarget(RenderTarget* target) override
    {
        target_history.push_back(target);
    }
    std::vector<RenderTarget*> target_history;
    void setLights(const std::vector<vine::raw_ptr<const Light>>& lights) override
    {
        ++light_sets;
        last_light_count = lights.size();
        last_light = lights.empty() ? nullptr : lights.front();
    }
    void drawScreenTexture(RenderTarget* source, int attachment) override
    {
        ++screen_draws;
        last_screen_source = source;
        last_screen_attachment = attachment;
    }
    int program_draws = 0;
    RenderTarget* last_program_source = nullptr;
    const ShaderProgram* last_program = nullptr;
    const Camera* last_program_camera = nullptr;
    void drawScreenProgram(RenderTarget* source, const ShaderProgram* program, const Camera* camera) override
    {
        ++program_draws;
        last_program_source = source;
        last_program = program;
        last_program_camera = camera;
    }
    void render(const std::vector<RenderCommand>& commands, const Camera* camera) override
    {
        ++render_calls;
        last_camera = camera;
        last_programs.clear();
        last_programs.reserve(commands.size());
        for (const auto& command : commands) {
            last_programs.push_back(command.program.get());
        }
    }
    void setViewport(int x, int y, int width, int height) override
    {
        ++viewport_sets;
        last_viewport[0] = x;
        last_viewport[1] = y;
        last_viewport[2] = width;
        last_viewport[3] = height;
    }
    void clear(const Color&, bool) override { ++clear_calls; }
    void swapBuffers() override { ++swap_calls; }

    // Pass-scope recording: the engine opens/closes one scope per executed
    // pass, announcing the pass itself (the backend's per-pass state key).
    std::vector<const RenderPass*> began;
    std::vector<const RenderPass*> ended;
    int begin_passes = 0;
    int end_passes = 0;
    void beginPass(vine::raw_ptr<const RenderPass> pass) override
    {
        ++begin_passes;
        began.push_back(pass);
    }
    void endPass() override { ++end_passes; }

    // Closed-loop release recording (removePass/clearPasses -> backend).
    int layer_releases = 0;
    int target_releases = 0;
    int pass_releases = 0;
    const Camera* last_released_layer = nullptr;
    int last_released_slot = 0;
    RenderTarget* last_released_target = nullptr;
    const RenderPass* last_released_pass = nullptr;
    void releaseWindowLayer(vine::raw_ptr<const Camera> camera, int slot) override
    {
        ++layer_releases;
        last_released_layer = camera;
        last_released_slot = slot;
    }
    void releasePass(vine::raw_ptr<const RenderPass> pass) override
    {
        ++pass_releases;
        last_released_pass = pass;
    }
    void releaseRenderTarget(RenderTarget* target) override
    {
        ++target_releases;
        last_released_target = target;
    }
};

TEST(RenderEngineTest, PassScopeOpensAndClosesOncePerExecutedPass)
{
    // Every executed pass runs inside its own pass scope: the backend is told
    // which pass is running (its identity for retained per-pass GPU state and
    // the marker of "active this frame"), and the scope is closed afterwards
    // so un-consumed per-pass state cannot leak into the next pass. A disabled
    // pass is not executed and therefore opens no scope.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    ASSERT_TRUE(engine->initialize());

    auto camera = intrusive_ptr<Camera>(new Camera());
    auto scene  = intrusive_ptr<Scene>(new Scene());
    auto pass_a = intrusive_ptr<RenderPass>(new RenderPass());
    pass_a->setCamera(camera.get());
    auto pass_b = intrusive_ptr<RenderPass>(new RenderPass());
    pass_b->setCamera(camera.get());
    auto hidden = intrusive_ptr<RenderPass>(new RenderPass());
    hidden->setCamera(camera.get());
    hidden->setEnabled(false);

    engine->addPass(pass_a, scene, 0);
    engine->addPass(pass_b, scene, 1);
    engine->addPass(hidden, scene, 2);

    engine->frame(0.016);

    ASSERT_EQ(backend->began.size(), 2u);
    EXPECT_EQ(backend->began[0], pass_a.get());
    EXPECT_EQ(backend->began[1], pass_b.get());
    EXPECT_EQ(backend->began.back(), pass_b.get());
    EXPECT_EQ(backend->end_passes, 2);
    EXPECT_EQ(backend->begin_passes, backend->end_passes);

    // Enabling the pass again brings its scope (and drawing) back.
    hidden->setEnabled(true);
    engine->frame(0.016);
    ASSERT_EQ(backend->began.size(), 5u);
    EXPECT_EQ(backend->began[2], pass_a.get());
    EXPECT_EQ(backend->began[3], pass_b.get());
    EXPECT_EQ(backend->began[4], hidden.get());
    EXPECT_EQ(backend->end_passes, 5);
}

TEST(RenderEngineTest, RemovePassReleasesBackendPerPassState)
{
    // The primary closed loop: a removed pass' retained GPU state is released
    // by identity (releasePass), not only through the legacy camera-keyed
    // window-layer call.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(pass, 0);
    engine->removePass(pass.get());

    EXPECT_EQ(backend->pass_releases, 1);
    EXPECT_EQ(backend->last_released_pass, pass.get());
}

TEST(RenderEngineTest, RemovePassReleasesBackendRenderTarget)
{
    // Closed loop: dropping a pass that owns an off-screen target must tell
    // the backend to free that target's GPU resources before the target dies.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass   = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setRenderTarget(target);
    engine->addPass(pass, 1);

    engine->removePass(pass.get());
    EXPECT_EQ(backend->target_releases, 1);
    EXPECT_EQ(backend->last_released_target, target.get());
}

TEST(RenderEngineTest, RemovePassReleasesBackendWindowLayerAndTarget)
{
    // Closed loop: dropping a pass must tell the backend to stop drawing its
    // window layer (keyed by the pass camera) AND free any off-screen target
    // the pass owns.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    auto camera  = intrusive_ptr<Camera>(new Camera());
    auto target  = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass    = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera.get());
    pass->setRenderTarget(target);
    engine->addPass(pass, 1);

    engine->removePass(pass.get());
    EXPECT_EQ(backend->layer_releases, 1);
    EXPECT_EQ(backend->last_released_layer, camera.get());
    EXPECT_EQ(backend->target_releases, 1);
    EXPECT_EQ(backend->last_released_target, target.get());
    EXPECT_EQ(backend->pass_releases, 1);
    EXPECT_EQ(backend->last_released_pass, pass.get());
}

TEST(RenderEngineTest, ClearPassesReleasesEveryRegisteredPass)
{
    // Clearing the pipeline releases the backend resources of each removed
    // pass exactly once (a pass is registered at most once, so no dedupe is
    // needed).
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    auto cam_a   = intrusive_ptr<Camera>(new Camera());
    auto pass_a  = intrusive_ptr<RenderPass>(new RenderPass());
    pass_a->setCamera(cam_a.get());
    engine->addPass(pass_a, 0);

    auto cam_b   = intrusive_ptr<Camera>(new Camera());
    auto target  = intrusive_ptr<RenderTarget>(new RenderTarget());
    auto pass_b  = intrusive_ptr<RenderPass>(new RenderPass());
    pass_b->setCamera(cam_b.get());
    pass_b->setRenderTarget(target);
    engine->addPass(pass_b, 5);

    EXPECT_EQ(engine->passCount(), 2u);
    engine->clearPasses();
    EXPECT_EQ(engine->passCount(), 0u);
    EXPECT_EQ(backend->layer_releases, 2);
    EXPECT_EQ(backend->target_releases, 1);
    EXPECT_EQ(backend->last_released_target, target.get());
    EXPECT_EQ(backend->pass_releases, 2);
}

TEST(RenderEngineTest, HasWindowPassReflectsCameraPresentation)
{
    // hasWindowPass(camera) tells a view whether its camera is already
    // presented to the window, so a view only registers its default window
    // pass when none exists (helper / HUD passes draw through their own
    // cameras and must not suppress it).
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view_cam = intrusive_ptr<Camera>(new Camera());
    EXPECT_FALSE(engine->hasWindowPass(view_cam.get()));

    // A HUD pass drawing through its own camera is not a window pass for the
    // view camera.
    auto hud_cam = intrusive_ptr<Camera>(new Camera());
    auto hud     = intrusive_ptr<RenderPass>(new RenderPass());
    hud->setCamera(hud_cam.get());
    engine->addPass(hud, 10);
    EXPECT_FALSE(engine->hasWindowPass(view_cam.get()));

    // A pass presenting the view camera to the backbuffer is.
    auto window = intrusive_ptr<RenderPass>(new RenderPass());
    window->setCamera(view_cam.get());
    engine->addPass(window, 0);
    EXPECT_TRUE(engine->hasWindowPass(view_cam.get()));

    // A disabled window pass is not counted (it draws nothing).
    window->setEnabled(false);
    EXPECT_FALSE(engine->hasWindowPass(view_cam.get()));
}
TEST(RenderEngineTest, AddSamePassTwiceKeepsSingleSlot)
{
    // Registering the same extra pass twice must not run it twice per frame.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto pass   = intrusive_ptr<RenderPass>(new RenderPass());
    engine->addPass(pass, 1);
    engine->addPass(pass, 2);  // duplicate add is ignored
    EXPECT_EQ(engine->passCount(), 1u);
}

TEST(RenderEngineTest, InitializeCallsBackend)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    EXPECT_TRUE(engine->initialize());
    EXPECT_TRUE(backend->ok);
    EXPECT_EQ(engine->backend(), backend.get());
}

TEST(RenderEngineTest, FrameRunsPipeline)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // An explicit pipeline: a window pass (order 0, null render target)
    // carrying a camera and an explicit content scene, drawing to the
    // backbuffer.
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam = intrusive_ptr<Camera>(new Camera());
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(cam.get());
    engine->addPass(pass, content, 0);

    engine->frame();

    EXPECT_EQ(backend->begin_calls, 1);
    EXPECT_EQ(backend->end_calls, 1);
    EXPECT_EQ(backend->swap_calls, 1);
    EXPECT_GE(backend->clear_calls, 1);
}

TEST(RenderEngineTest, FrameBeforeInitializeIsNoOp)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    // Not initialized: frame should do nothing.
    engine->frame();
    EXPECT_EQ(backend->begin_calls, 0);
    EXPECT_EQ(backend->swap_calls, 0);
}

TEST(RenderEngineTest, EngineStartsEmpty)
{
    // Design C: the engine imposes no pipeline - no content scene, no camera,
    // no registered pass until the caller binds everything explicitly.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);

    EXPECT_EQ(engine->passCount(), 0u);
}

TEST(SceneViewTest, OwnsCameraAndContentScene)
{
    // A SceneView owns a camera and a content scene; the engine it borrows
    // stays a pure scheduler (no content, no camera).
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());

    ASSERT_NE(view->camera(), nullptr);
    ASSERT_NE(view->scene().get(), nullptr);

    view->setEngine(engine.get());
    EXPECT_EQ(view->engine(), engine.get());
    EXPECT_EQ(engine->passCount(), 0u);  // no pass auto-registered yet
}

TEST(SceneViewTest, SurfaceLayoutStepsRunOnResize)
{
    // Creator-managed layout: a registered callback receives the surface size
    // on every resize and updates whatever that code owns (targets /
    // viewports). The view only dispatches; it does not decide layout.
    SceneView view;
    bool called = false;
    int  got_w  = 0;
    int  got_h  = 0;
    view.addSurfaceLayout([&](int w, int h) {
        called = true;
        got_w  = w;
        got_h  = h;
    });

    view.onSurfaceResized(1600, 900);
    EXPECT_TRUE(called);
    EXPECT_EQ(got_w, 1600);
    EXPECT_EQ(got_h, 900);

    // A second resize re-runs the step with the new size.
    view.onSurfaceResized(1280, 720);
    EXPECT_EQ(got_w, 1280);
    EXPECT_EQ(got_h, 720);

    // Clearing drops the registered steps.
    called = false;
    view.clearSurfaceLayouts();
    view.onSurfaceResized(640, 480);
    EXPECT_FALSE(called);
}

TEST(SceneViewTest, EnsureWindowPassRegistersOrder0WhenCameraNotPresented)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    EXPECT_FALSE(engine->hasWindowPass(view->camera()));
    view->ensureWindowPass();
    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_TRUE(engine->hasWindowPass(view->camera()));

    // Registering again is a no-op while the window pass is present.
    view->ensureWindowPass();
    EXPECT_EQ(engine->passCount(), 1u);
}

TEST(SceneViewTest, EnsureWindowPassSkipsWhenCameraAlreadyPresented)
{
    // An application main pass carrying the view's camera (e.g. a deferred
    // lighting pass to the window) suppresses the default window pass.
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto view   = intrusive_ptr<SceneView>(new SceneView());
    view->setEngine(engine.get());

    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(view->camera());
    engine->addPass(main, 0);

    view->ensureWindowPass();
    EXPECT_EQ(engine->passCount(), 1u);
    EXPECT_TRUE(engine->hasWindowPass(view->camera()));
}

TEST(SceneViewTest, DefaultManipulatorBindsViewCameraAndScene)
{
    auto view = intrusive_ptr<SceneView>(new SceneView());

    // The default orbit manipulator is created lazily on first access and is
    // bound to the view's camera.
    CameraManipulator* manip = view->manipulator();
    ASSERT_NE(manip, nullptr);
    EXPECT_EQ(manip->camera(), view->camera());
    EXPECT_EQ(view->manipulator(), manip);  // cached

    // A custom manipulator replaces the default.
    auto cam     = intrusive_ptr<Camera>(new Camera());
    auto custom  = intrusive_ptr<CameraManipulator>(new OrbitCameraManipulator(cam.get()));
    view->setManipulator(custom);
    EXPECT_EQ(view->manipulator(), custom.get());
}

TEST(RenderEngineTest, ShaderPresetForwardedToBackend)
{
    // The shading preset is held by the engine (render config) and forwarded
    // to the backend before initialize(); the backend maps it to its shader
    // set (vsg: Phong vs flat).
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    EXPECT_EQ(engine->shaderPreset(), ShaderPreset::StandardPhong);

    engine->setShaderPreset(ShaderPreset::FlatShaded);
    EXPECT_EQ(engine->shaderPreset(), ShaderPreset::FlatShaded);

    engine->setBackend(backend);
    EXPECT_TRUE(engine->initialize());
    EXPECT_EQ(backend->preset_sets, 1);
    EXPECT_EQ(backend->last_preset, ShaderPreset::FlatShaded);
}

TEST(RenderEngineTest, RegisteredPassesRunInAscendingOrder)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // Passes are explicit: one content scene bound to three registered scene
    // passes (pre order<0, window pass order 0, post order>0).
    auto content = intrusive_ptr<Scene>(new Scene());
    auto window_cam = intrusive_ptr<Camera>(new Camera());
    window_cam->setName(u8"window");
    auto pre_cam = intrusive_ptr<Camera>(new Camera());
    pre_cam->setName(u8"shadow/pre");
    auto post_cam = intrusive_ptr<Camera>(new Camera());
    post_cam->setName(u8"postfx");

    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(window_cam.get());
    auto pre = intrusive_ptr<RenderPass>(new RenderPass());
    pre->setCamera(pre_cam.get());
    auto post = intrusive_ptr<RenderPass>(new RenderPass());
    post->setCamera(post_cam.get());

    engine->addPass(main, content, 0);   // window-present pass.
    engine->addPass(pre, content, -5);   // shadow-map style pass before the window pass.
    engine->addPass(post, content, 5);   // post-processing pass after the window pass.
    EXPECT_EQ(engine->passCount(), 3u);

    const int before = backend->render_calls;
    engine->frame();
    // pre + main + post = three render calls per frame.
    EXPECT_EQ(backend->render_calls - before, 3);
    // The last rendered pass is the highest-order (post) pass.
    ASSERT_NE(backend->last_camera, nullptr);
    EXPECT_EQ(backend->last_camera->name(), u8"postfx");

    engine->removePass(post.get());
    EXPECT_EQ(engine->passCount(), 2u);
    const int before2 = backend->render_calls;
    engine->frame();
    // Only pre + main now.
    EXPECT_EQ(backend->render_calls - before2, 2);

    engine->clearPasses();
    EXPECT_EQ(engine->passCount(), 0u);
}

TEST(RenderEngineTest, PassContentBindingManagedByEngine)
{
    // Content is bound per pass; the engine has no default content, so an
    // unbound pass has none and draws nothing.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    auto sceneA = intrusive_ptr<Scene>(new Scene());
    auto sceneB = intrusive_ptr<Scene>(new Scene());
    auto cam    = intrusive_ptr<Camera>(new Camera());
    cam->setName(u8"pass-view");

    // Explicit content: pass A draws sceneA; pass B has no content.
    auto passA = intrusive_ptr<RenderPass>(new RenderPass());
    passA->setCamera(cam.get());
    engine->addPass(passA, sceneA, 5);
    auto passB = intrusive_ptr<RenderPass>(new RenderPass());
    passB->setCamera(cam.get());
    engine->addPass(passB, -5);

    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_EQ(engine->contentOf(passA.get()), sceneA.get());
    EXPECT_EQ(engine->contentOf(passB.get()), nullptr);
    // Unknown passes have no content.
    EXPECT_EQ(engine->contentOf(nullptr), nullptr);

    // Runtime rebind of the registered pass.
    engine->bindPassContent(passA.get(), sceneB);
    EXPECT_EQ(engine->contentOf(passA.get()), sceneB.get());

    // Bind the previously content-less pass.
    auto sceneC = intrusive_ptr<Scene>(new Scene());
    engine->bindPassContent(passB.get(), sceneC);
    EXPECT_EQ(engine->contentOf(passB.get()), sceneC.get());
    EXPECT_EQ(engine->contentOf(passA.get()), sceneB.get());

    // Clearing a binding leaves the pass content-less (draws nothing).
    engine->bindPassContent(passB.get(), nullptr);
    EXPECT_EQ(engine->contentOf(passB.get()), nullptr);
    EXPECT_EQ(engine->contentOf(passA.get()), sceneB.get());
}

TEST(RenderPassTest, NamedOutputAndInputSlots)
{
    RenderPass pass;
    EXPECT_TRUE(pass.outputName().empty());
    EXPECT_TRUE(pass.inputNames().empty());

    pass.setOutputName(u8"SceneColor");
    EXPECT_EQ(pass.outputName(), u8"SceneColor");
    // Empty names never publish / are ignored.
    pass.setOutputName(u8"");
    EXPECT_TRUE(pass.outputName().empty());
    pass.setOutputName(u8"GBuffer");
    EXPECT_EQ(pass.outputName(), u8"GBuffer");

    pass.addInputName(u8"ShadowMap");
    pass.addInputName(u8"SceneColor");
    // Empty names are ignored.
    pass.addInputName(u8"");
    ASSERT_EQ(pass.inputNames().size(), 2u);
    EXPECT_EQ(pass.inputNames()[0], u8"ShadowMap");
    EXPECT_EQ(pass.inputNames()[1], u8"SceneColor");

    pass.clearInputNames();
    EXPECT_TRUE(pass.inputNames().empty());
}

TEST(RenderPassTest, ViewportObjectDefaultsAndRoundTrip)
{
    // A pass holds a Viewport object; unset = full surface (default).
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    EXPECT_FALSE(pass->hasViewport());

    pass->setViewport(Viewport{ 4, 5, 96, 96 });
    ASSERT_TRUE(pass->hasViewport());
    const Viewport vp = pass->viewport();
    EXPECT_EQ(vp.x, 4);
    EXPECT_EQ(vp.y, 5);
    EXPECT_EQ(vp.width, 96);
    EXPECT_EQ(vp.height, 96);
    EXPECT_TRUE(vp.valid());

    pass->clearViewport();
    EXPECT_FALSE(pass->hasViewport());
}

TEST(RenderEngineTest, ResizeDoesNotManagePassLayout)
{
    // The engine only rebuilds its own swapchain on a surface resize; it does
    // not touch target sizes or pass viewports - their creators maintain them
    // (e.g. via SceneView layout steps).
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    auto target = intrusive_ptr<RenderTarget>(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(320, 180);
    auto cam  = intrusive_ptr<Camera>(new Camera());
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(cam.get());
    pass->setRenderTarget(target);
    pass->setViewport(10, 20, 100, 50);
    engine->addPass(pass, 5);

    engine->resize(1600, 900);

    // The pass's viewport and the target size are untouched by the engine.
    EXPECT_EQ(target->width(), 320);
    EXPECT_EQ(target->height(), 180);
    int x = 0, y = 0, w = 0, h = 0;
    ASSERT_TRUE(pass->hasViewport());
    pass->getViewport(x, y, w, h);
    EXPECT_EQ(x, 10);
    EXPECT_EQ(y, 20);
    EXPECT_EQ(w, 100);
    EXPECT_EQ(h, 50);
}

TEST(RenderEngineTest, NamedOutputRegistryPublishResolve)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    auto rt = intrusive_ptr<RenderTarget>(new RenderTarget());
    EXPECT_EQ(engine->resolve(u8"SceneColor"), nullptr);

    engine->publish(u8"SceneColor", rt);
    EXPECT_EQ(engine->resolve(u8"SceneColor"), rt.get());

    // Publishing under the same name replaces the entry.
    auto rt2 = intrusive_ptr<RenderTarget>(new RenderTarget());
    engine->publish(u8"SceneColor", rt2);
    EXPECT_EQ(engine->resolve(u8"SceneColor"), rt2.get());

    engine->unpublish(u8"SceneColor");
    EXPECT_EQ(engine->resolve(u8"SceneColor"), nullptr);
}

TEST(RenderEngineTest, OffscreenPassPublishesThenScreenPassSamples)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // An explicit producer pass: it renders into the off-screen target it
    // publishes as "SceneColor" (its content is null here; publishing only
    // needs the non-null target).
    auto cam = intrusive_ptr<Camera>(new Camera());

    // Producer: an off-screen pass rendering into a target it publishes as
    // "SceneColor" before the window pass.
    auto rt = intrusive_ptr<RenderTarget>(new RenderTarget());
    rt->setSize(640, 360);
    rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    rt->attachDepth(RenderTarget::DepthFormat::D24);

    auto off = intrusive_ptr<RenderPass>(new RenderPass());
    off->setCamera(cam.get());
    off->setRenderTarget(rt);
    off->setOutputName(u8"SceneColor");
    engine->addPass(off, -2);

    // Consumer: a ScreenPass declaring it wants the published "SceneColor".
    auto screen = intrusive_ptr<ScreenPass>(new ScreenPass());
    screen->addInputName(u8"SceneColor");
    // A screen pass composites over existing content, so it never clears.
    EXPECT_FALSE(screen->clearEnabled());
    engine->addPass(screen, 100);

    const int before = backend->screen_draws;
    engine->frame();

    // The ScreenPass resolved "SceneColor" to the off-screen target and asked
    // the backend to composite it exactly once this frame.
    EXPECT_EQ(backend->screen_draws - before, 1);
    EXPECT_EQ(backend->last_screen_source, rt.get());
    EXPECT_EQ(screen->sourceTarget(), rt.get());
    // The registry holds the off-screen target after the producer ran.
    EXPECT_EQ(engine->resolve(u8"SceneColor"), rt.get());

    // Without a producer nothing resolves, so the ScreenPass draws nothing.
    auto engine2  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto backend2 = intrusive_ptr<MockBackend>(new MockBackend());
    engine2->setBackend(backend2);
    engine2->initialize();
    auto orphan = intrusive_ptr<ScreenPass>(new ScreenPass());
    orphan->addInputName(u8"SceneColor");
    engine2->addPass(orphan, 100);
    engine2->frame();
    EXPECT_EQ(backend2->screen_draws, 0);
    EXPECT_EQ(orphan->sourceTarget(), nullptr);
}

TEST(RenderEngineTest, ShutdownReleasesBackend)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();
    EXPECT_TRUE(backend->ok);

    engine->shutdown();
    EXPECT_FALSE(backend->ok);
}

TEST(RenderEngineTest, ScreenPassSamplesSelectedMrtAttachment)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();
    auto cam = intrusive_ptr<Camera>(new Camera());

    // Producer: an MRT (multi-attachment) target published as one name.
    auto rt = intrusive_ptr<RenderTarget>(new RenderTarget());
    rt->setSize(640, 360);
    rt->attachColor(RenderTarget::ColorFormat::RGBA8);   // att 0: albedo
    rt->attachColor(RenderTarget::ColorFormat::RGBA16F); // att 1: normal
    rt->attachDepth(RenderTarget::DepthFormat::D24);
    EXPECT_EQ(rt->colorCount(), 2);

    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setCamera(cam.get());
    producer->setRenderTarget(rt);
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, -2);

    // Two consumers sampling different colour attachments of the same target.
    auto albedo = intrusive_ptr<ScreenPass>(new ScreenPass());
    albedo->addInputName(u8"GBuffer");
    albedo->setSourceAttachment(0);
    engine->addPass(albedo, 100);

    auto normal = intrusive_ptr<ScreenPass>(new ScreenPass());
    normal->addInputName(u8"GBuffer");
    normal->setSourceAttachment(1);
    engine->addPass(normal, 101);

    // Negative selection clamps to attachment 0.
    normal->setSourceAttachment(-3);
    EXPECT_EQ(normal->sourceAttachment(), 0);
    normal->setSourceAttachment(1);
    EXPECT_EQ(normal->sourceAttachment(), 1);

    const int before = backend->screen_draws;
    engine->frame();

    // Each ScreenPass asked the backend to sample the published target at the
    // attachment it selected (the MRT target is resolved, not a copy).
    EXPECT_EQ(albedo->sourceTarget(), rt.get());
    EXPECT_EQ(normal->sourceTarget(), rt.get());
    // Two screen draws happened this frame (deltas avoid warm-up coupling);
    // the later pass sampled colour attachment 1.
    EXPECT_EQ(backend->screen_draws, before + 2);
    EXPECT_EQ(backend->last_screen_source, rt.get());
    EXPECT_EQ(backend->last_screen_attachment, 1); // last draw sampled att 1
}

TEST(RenderEngineTest, ScreenPassProgramSamplesMrtForDeferredLighting)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();
    auto cam = intrusive_ptr<Camera>(new Camera());

    // Producer: an MRT G-buffer target published as one name.
    auto rt = intrusive_ptr<RenderTarget>(new RenderTarget());
    rt->setSize(640, 360);
    rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    rt->attachColor(RenderTarget::ColorFormat::RGBA16F);
    rt->attachColor(RenderTarget::ColorFormat::RGBA16F);
    rt->attachDepth(RenderTarget::DepthFormat::D24);
    EXPECT_EQ(rt->colorCount(), 3);
    auto producer = intrusive_ptr<RenderPass>(new RenderPass());
    producer->setCamera(cam.get());
    producer->setRenderTarget(rt);
    producer->setOutputName(u8"GBuffer");
    engine->addPass(producer, -3);

    // Lighting consumer: a ScreenPass carrying a fragment program samples the
    // whole MRT source (all attachments) through the backend.
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    ShaderStage fs;
    fs.type = ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec2 v_uv;\n"
                u8"layout(location = 0) out vec4 out_color;\n"
                u8"void main() { out_color = vec4(v_uv, 0.0, 1.0); }\n";
    program->addStage(fs);

    auto light = intrusive_ptr<ScreenPass>(new ScreenPass());
    light->addInputName(u8"GBuffer");
    light->setCamera(cam.get());
    light->setProgram(program);
    EXPECT_EQ(light->program(), program.get());
    engine->addPass(light, 100);

    const int before = backend->program_draws;
    engine->frame();

    // The fullscreen program pass asked the backend to draw through the
    // lighting program, sampling the MRT source under the pass camera.
    EXPECT_EQ(backend->program_draws, before + 1);
    EXPECT_EQ(backend->last_program_source, rt.get());
    EXPECT_EQ(backend->last_program, program.get());
    EXPECT_EQ(backend->last_program_camera, cam.get());
}

// ============ Light ============

TEST(LightTest, AmbientFactoryDefaults)
{
    auto light = Light::createAmbient();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type(), LightType::Ambient);
    EXPECT_TRUE(light->isEnabled());
    EXPECT_FALSE(light->hasDirection());
    // White, full intensity by default.
    EXPECT_FLOAT_EQ(light->color().r, 1.0f);
    EXPECT_FLOAT_EQ(light->intensity(), 1.0f);
    EXPECT_FALSE(light->castShadow());
}

TEST(LightTest, DirectionalFactoryCarriesDirection)
{
    const vine::math::Vec3d dir(0.2, -0.5, -0.8);
    auto light = Light::createDirectional(dir);
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type(), LightType::Directional);
    EXPECT_TRUE(light->hasDirection());
    EXPECT_EQ(light->direction(), dir);

    light->setDirection(vine::math::Vec3d(1.0, 0.0, 0.0));
    EXPECT_EQ(light->direction(), vine::math::Vec3d(1.0, 0.0, 0.0));
}

TEST(LightTest, Setters)
{
    auto light = Light::createAmbient();
    light->setName(u8"key_light");
    EXPECT_EQ(light->name(), u8"key_light");
    light->setEnabled(false);
    EXPECT_FALSE(light->isEnabled());
    light->setColor(Colorf(0.2f, 0.4f, 0.6f));
    EXPECT_FLOAT_EQ(light->color().r, 0.2f);
    light->setIntensity(2.5f);
    EXPECT_FLOAT_EQ(light->intensity(), 2.5f);
    light->setCastShadow(true);
    EXPECT_TRUE(light->castShadow());
}

TEST(SceneTest, LightSlots)
{
    Scene scene;
    EXPECT_FALSE(scene.hasLights());
    EXPECT_TRUE(scene.lights().empty());

    auto ambient = Light::createAmbient();
    auto sun     = Light::createDirectional(vine::math::Vec3d(0.0, -1.0, 0.0));
    scene.addLight(ambient);
    scene.addLight(sun);
    EXPECT_TRUE(scene.hasLights());
    ASSERT_EQ(scene.lights().size(), 2u);
    EXPECT_EQ(scene.lights()[0].get(), ambient.get());
    EXPECT_EQ(scene.lights()[1].get(), sun.get());

    // Null lights are ignored.
    scene.addLight(nullptr);
    EXPECT_EQ(scene.lights().size(), 2u);

    scene.removeLight(sun.get());
    EXPECT_EQ(scene.lights().size(), 1u);
    scene.clearLights();
    EXPECT_FALSE(scene.hasLights());
    EXPECT_TRUE(scene.lights().empty());
}

TEST(RenderPassTest, ExecuteForwardsSceneLights)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto scene   = intrusive_ptr<Scene>(new Scene());
    auto camera  = intrusive_ptr<Camera>(new Camera());
    auto light   = Light::createAmbient();

    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera.get());
    pass->execute(scene.get(), backend.get());
    // No lights on the scene -> an empty light set is forwarded.
    EXPECT_GE(backend->light_sets, 1);
    EXPECT_EQ(backend->last_light_count, 0u);

    scene->addLight(light);
    const int before = backend->light_sets;
    pass->execute(scene.get(), backend.get());
    EXPECT_GT(backend->light_sets, before);
    EXPECT_EQ(backend->last_light_count, 1u);
    EXPECT_EQ(backend->last_light, light.get());
}

TEST(LightTest, ShadowSettingsDefaultsAndSetters)
{
    auto sun = Light::createDirectional(vine::math::Vec3d(0.0, -1.0, 0.0));
    EXPECT_FALSE(sun->castShadow());

    const auto& defaults = sun->shadowSettings();
    EXPECT_EQ(defaults.resolution, 1024u);
    EXPECT_FLOAT_EQ(defaults.bias, 0.002f);
    EXPECT_EQ(defaults.filter, ShadowFilter::Hard);

    sun->setCastShadow(true);
    EXPECT_TRUE(sun->castShadow());
    sun->setShadowResolution(2048);
    sun->setShadowBias(0.01f);
    sun->setShadowFilter(ShadowFilter::PCF);
    EXPECT_EQ(sun->shadowSettings().resolution, 2048u);
    EXPECT_FLOAT_EQ(sun->shadowSettings().bias, 0.01f);
    EXPECT_EQ(sun->shadowSettings().filter, ShadowFilter::PCF);
}

namespace
{

/** @brief Finds the first depth-only target in the recorded target history. */
bool hasDepthOnlyShadow(const std::vector<RenderTarget*>& history)
{
    for (const auto* target : history) {
        if (target != nullptr && target->hasDepth() && !target->hasColor()) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(RenderEngineTest, ShadowPassIsJustARegisteredScenePass)
{
    // Design B: the engine has no shadow subsystem and no auto scheduling. A
    // depth-only "shadow" pass is expressed like any other scene pass - a
    // RenderPass with its own light-view camera, a depth-only render target,
    // a negative order and the shadow-casting content - and is ordered before
    // the window pass by order alone.
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    content->setRoot(makeTriangleNode(Vec3d(0.0, 0.0, 0.0), nullptr));

    auto cam = intrusive_ptr<Camera>(new Camera());
    auto main = intrusive_ptr<RenderPass>(new RenderPass());
    main->setCamera(cam.get());
    engine->addPass(main, content, 0);   // window pass (null target).

    // Explicit depth-only shadow pass: a light-view camera + depth-only
    // target, running before the window pass (order < 0). No engine magic.
    auto light_cam = intrusive_ptr<Camera>(new Camera());
    light_cam->setViewMatrixAsLookAt(Vec3d(5, 5, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    light_cam->setProjectionMatrixAsOrtho(-3, 3, -3, 3, 0.1, 20.0);
    auto depth = intrusive_ptr<RenderTarget>(new RenderTarget());
    depth->setSize(1024, 1024);
    depth->attachDepth(RenderTarget::DepthFormat::D24);
    auto shadow = intrusive_ptr<RenderPass>(new RenderPass());
    shadow->setName(u8"shadow");
    shadow->setCamera(light_cam.get());
    shadow->setRenderTarget(depth);
    shadow->setShouldClearDepth(true);
    engine->addPass(shadow, content, -5);

    EXPECT_EQ(engine->passCount(), 2u);

    const int renders_before = backend->render_calls;
    engine->frame();
    // Depth-only shadow pass + window pass.
    EXPECT_EQ(backend->render_calls - renders_before, 2);
    EXPECT_TRUE(hasDepthOnlyShadow(backend->target_history));
    // The depth-only pass ran before the window (null target) pass.
    ASSERT_GE(backend->target_history.size(), 2u);
    EXPECT_NE(backend->target_history[0], nullptr);
    EXPECT_TRUE(backend->target_history[0]->hasDepth() && !backend->target_history[0]->hasColor());
    EXPECT_EQ(backend->target_history[1], nullptr);
}

TEST(RenderPipelineBuilderTest, OffscreenToScreenBuildsExpectedPipeline)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // The off-screen recipe needs an explicit view camera and explicit content
    // for its scene pass.
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto* screen = builder.addOffscreenToScreen(
        u8"SceneColor", 640, 360,
        RenderTarget::ColorFormat::RGBA8,
        RenderTarget::DepthFormat::D24,
        8, 8, 320, 180);
    ASSERT_NE(screen, nullptr);
    // The recipe adds the off-screen (order < 0) + screen (order > 0) passes.
    EXPECT_EQ(engine->passCount(), 2u);

    const int draws_before = backend->screen_draws;
    engine->frame();
    // The screen pass sampled the published off-screen target exactly once.
    EXPECT_EQ(backend->screen_draws - draws_before, 1);
    EXPECT_EQ(screen->sourceTarget(), engine->resolve(u8"SceneColor"));
    ASSERT_NE(engine->resolve(u8"SceneColor"), nullptr);
    EXPECT_TRUE(engine->resolve(u8"SceneColor")->hasColor());
}

TEST(RenderPipelineBuilderTest, ForwardPresetBuildsWindowPass)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelinePreset::Forward);

    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_EQ(engine->passCount(), 1u);
    // The single order-0 window pass presents the view camera and draws the
    // bound content, exactly like the SceneView default viewer.
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));
    EXPECT_EQ(engine->contentOf(pipeline->windowPass()), content.get());
    EXPECT_EQ(pipeline->offscreenTarget(), nullptr);
}

TEST(RenderPipelineBuilderTest, DeferredPresetBuildsGbufferAndLightingPasses)
{
    auto engine     = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content    = intrusive_ptr<Scene>(new Scene());
    auto cam        = intrusive_ptr<Camera>(new Camera());
    auto gbuf_prog  = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    auto light_prog = intrusive_ptr<ShaderProgram>(new ShaderProgram());

    PipelineOptions opts;
    opts.offscreen_width   = 640;
    opts.offscreen_height  = 360;
    opts.gbuffer_program   = gbuf_prog;
    opts.lighting_program  = light_prog;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelinePreset::Deferred, opts);

    ASSERT_NE(pipeline, nullptr);
    // G-buffer (order < 0) + fullscreen lighting window pass (order 0).
    EXPECT_EQ(engine->passCount(), 2u);
    // The lighting pass presents the view camera, so RenderControl / SceneView
    // must not add a second forward window pass.
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));

    RenderPass* window = pipeline->windowPass();
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->camera(), cam.get());
    EXPECT_EQ(window->renderTarget(), nullptr);

    RenderTarget* gbuffer = pipeline->offscreenTarget();
    ASSERT_NE(gbuffer, nullptr);
    EXPECT_EQ(gbuffer->width(), 640);
    EXPECT_EQ(gbuffer->height(), 360);
    EXPECT_TRUE(gbuffer->hasColor());
    EXPECT_TRUE(gbuffer->hasDepth());

    // Creator-maintained sizing: the handle resizes the G-buffer target.
    pipeline->resize(1280, 720);
    EXPECT_EQ(gbuffer->width(), 1280);
    EXPECT_EQ(gbuffer->height(), 720);
}

TEST(RenderPipelineBuilderTest, DeferredPresetFallsBackToBuiltinPrograms)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    // No programs supplied: the builder supplies its built-in temporary
    // G-buffer geometry + deferred-lighting programs.
    PipelineOptions opts;
    opts.offscreen_width  = 640;
    opts.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelinePreset::Deferred, opts);

    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));
    // The lighting window pass received the built-in default fragment program.
    auto* light = dynamic_cast<ScreenPass*>(pipeline->windowPass());
    ASSERT_NE(light, nullptr);
    EXPECT_NE(light->program(), nullptr);
}

TEST(RenderPipelineBuilderTest, DeferredPresetRefusesWithoutContent)
{
    auto engine = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto cam    = intrusive_ptr<Camera>(new Camera());

    // A Deferred main pass needs a content scene to fill its G-buffer.
    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    auto pipeline = builder.build(PipelinePreset::Deferred);

    EXPECT_EQ(pipeline, nullptr);
    EXPECT_EQ(engine->passCount(), 0u);
}

TEST(RenderPipelineBuilderTest, DeferredPresetWithTransparentContentBuildsComposite)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto overlay = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    PipelineOptions opts;
    opts.offscreen_width  = 640;
    opts.offscreen_height = 360;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    builder.setTransparentContent(overlay);
    auto pipeline = builder.build(PipelinePreset::Deferred, opts);

    ASSERT_NE(pipeline, nullptr);
    // G-buffer + lighting INTO the composite + forward transparent + present:
    // four registered passes (the opaque scene is rasterised once — the
    // composite reuses the G-buffer depth instead of re-rendering it).
    EXPECT_EQ(engine->passCount(), 4u);
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));

    // Both off-screen stages exist: the G-buffer (sampled by the lighting) and
    // the composite (baked off-screen, then presented to the window).
    RenderTarget* gbuffer = pipeline->offscreenTarget();
    RenderTarget* composite = pipeline->compositeTarget();
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_NE(composite, nullptr);
    EXPECT_NE(composite, gbuffer);
    // The composite borrows the G-buffer depth (single opaque rasterisation),
    // which forces the G-buffer depth to stay an attachment (not sampleable).
    EXPECT_TRUE(composite->hasDepth());
    EXPECT_EQ(composite->depthSource(), gbuffer);
    EXPECT_FALSE(gbuffer->depthPromotion());
    EXPECT_TRUE(composite->depthPromotion()); // (irrelevant: composite has no own depth)

    // The window pass is the PRESENT pass: it carries the view camera (so
    // RenderControl / SceneView add no default forward pass), targets the
    // window, does not clear, and consumes the published composite.
    auto* present = dynamic_cast<ScreenPass*>(pipeline->windowPass());
    ASSERT_NE(present, nullptr);
    EXPECT_EQ(present->camera(), cam.get());
    EXPECT_EQ(present->renderTarget(), nullptr);
    EXPECT_FALSE(present->clearEnabled());
    const auto& inputs = present->inputNames();
    EXPECT_NE(std::find(inputs.begin(), inputs.end(), vine::String(u8"Composite")), inputs.end());

    // Sizing is creator-maintained for both off-screen targets together.
    pipeline->resize(1280, 720);
    EXPECT_EQ(pipeline->offscreenTarget()->width(), 1280);
    EXPECT_EQ(pipeline->compositeTarget()->width(), 1280);
    EXPECT_EQ(pipeline->offscreenTarget()->height(), 720);
    EXPECT_EQ(pipeline->compositeTarget()->height(), 720);
}

TEST(RenderPipelineBuilderTest, ForwardPresetWithTransparentContentStacksDepthOnPass)
{
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    auto content = intrusive_ptr<Scene>(new Scene());
    auto overlay = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    builder.setTransparentContent(overlay);
    auto pipeline = builder.build(PipelinePreset::Forward);

    ASSERT_NE(pipeline, nullptr);
    // Main order-0 window content pass + order-1 transparent content pass.
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));
    ASSERT_NE(pipeline->windowPass(), nullptr);
    EXPECT_EQ(engine->contentOf(pipeline->windowPass()), content.get());
    // Forward composites nothing off-screen: no composite target.
    EXPECT_EQ(pipeline->compositeTarget(), nullptr);
}

TEST(RenderPassTest, OcclusionIsExplicitAndIndependentOfClear)
{
    RenderPass pass;
    // Depth style is explicit and defaults on; it is NOT inferred from the
    // clear flag (a depth-on pass may skip clearing).
    EXPECT_TRUE(pass.occlusionEnabled());
    EXPECT_TRUE(pass.clearEnabled());
    pass.setOcclusionEnabled(false);
    EXPECT_FALSE(pass.occlusionEnabled());
    // The two flags are orthogonal: disabling the clear does not flip depth.
    pass.setClearEnabled(false);
    EXPECT_FALSE(pass.clearEnabled());
    EXPECT_FALSE(pass.occlusionEnabled());
    pass.setOcclusionEnabled(true);
    EXPECT_TRUE(pass.occlusionEnabled());
    EXPECT_FALSE(pass.clearEnabled());
}

TEST(RenderPipelineBuilderTest, DefaultGbufferTargetIsCanonicalLayout)
{
    auto gbuffer = RenderPipelineBuilder::defaultGbufferTarget(320, 180);
    ASSERT_NE(gbuffer, nullptr);
    EXPECT_EQ(gbuffer->width(), 320);
    EXPECT_EQ(gbuffer->height(), 180);
    EXPECT_EQ(gbuffer->colorCount(), 4);  // albedo / normal+shininess / spec / pos
    EXPECT_TRUE(gbuffer->hasDepth());

    // Non-positive sizes fall back to the default resolution.
    auto fallback = RenderPipelineBuilder::defaultGbufferTarget(0, 0);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->width(), 640);
    EXPECT_EQ(fallback->height(), 360);
}

TEST(RenderPipelineBuilderTest, GizmoOverlayConfiguredOnPreset)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    auto content = intrusive_ptr<Scene>(new Scene());
    auto cam     = intrusive_ptr<Camera>(new Camera());

    PipelineOptions opts;
    opts.gizmo.source_camera = cam.get();
    opts.gizmo.pixel_ratio   = 2.0;
    opts.gizmo.box_size      = 128;

    RenderPipelineBuilder builder(engine.get());
    builder.setCamera(cam.get());
    builder.setContent(content);
    auto pipeline = builder.build(PipelinePreset::Forward, opts);

    ASSERT_NE(pipeline, nullptr);
    ASSERT_NE(pipeline->gizmo(), nullptr);
    // Window pass + the gizmo HUD pass.
    EXPECT_EQ(engine->passCount(), 2u);
    EXPECT_TRUE(engine->hasWindowPass(cam.get()));

    // resize() re-anchors the gizmo even without an off-screen target; a
    // frame draws both the window and the gizmo without crashing.
    pipeline->resize(1600, 900);
    engine->frame();
}

// ============ HUD passes (top / overlay content) ============

TEST(HudPassTest, EngineDrawsPassesInOrderAndSkipsDisabled)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // A clearing window pass (explicit content scene) below the HUD passes, as
    // a real viewer would register; HUD passes draw on top.
    auto win_content = intrusive_ptr<Scene>(new Scene());
    auto master_cam = intrusive_ptr<Camera>(new Camera());
    auto window = intrusive_ptr<RenderPass>(new RenderPass());
    window->setCamera(master_cam.get());
    engine->addPass(window, win_content, 0);

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    auto scene = intrusive_ptr<Scene>(new Scene());

    // Two enabled HUD passes at ascending orders plus a disabled one; every
    // HUD pass draws over the previous content (clearing disabled).
    auto low = intrusive_ptr<RenderPass>(new RenderPass());
    low->setCamera(cam.get());
    low->setClearEnabled(false);
    engine->addPass(low, scene, 5);

    auto high = intrusive_ptr<RenderPass>(new RenderPass());
    high->setCamera(cam.get());
    high->setClearEnabled(false);
    engine->addPass(high, scene, 10);

    auto hidden = intrusive_ptr<RenderPass>(new RenderPass());
    hidden->setCamera(cam.get());
    hidden->setClearEnabled(false);
    hidden->setEnabled(false);
    engine->addPass(hidden, scene, 7);

    const int before = backend->render_calls;
    engine->frame();
    // Window pass + the two enabled HUD passes (the disabled one is skipped).
    EXPECT_EQ(backend->render_calls - before, 3);
}

TEST(HudPassTest, SubViewportAndClearPolicy)
{
    auto backend = intrusive_ptr<MockBackend>(new MockBackend());
    auto engine  = intrusive_ptr<RenderEngine>(new RenderEngine());
    engine->setBackend(backend);
    engine->initialize();

    // A clearing window pass below the HUD pass (as a real viewer registers).
    auto win_content = intrusive_ptr<Scene>(new Scene());
    auto master_cam = intrusive_ptr<Camera>(new Camera());
    auto window = intrusive_ptr<RenderPass>(new RenderPass());
    window->setCamera(master_cam.get());
    engine->addPass(window, win_content, 0);

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    auto scene = intrusive_ptr<Scene>(new Scene());

    auto hud = intrusive_ptr<RenderPass>(new RenderPass());
    hud->setCamera(cam.get());
    // A top pass never clears the surface it draws over.
    hud->setClearEnabled(false);
    EXPECT_FALSE(hud->clearEnabled());
    hud->setViewport(4, 5, 96, 96);
    engine->addPass(hud, scene, 10);

    const int before = backend->clear_calls;
    engine->frame();
    // The HUD sub-viewport was forwarded to the backend.
    EXPECT_EQ(backend->viewport_sets, 1);
    EXPECT_EQ(backend->last_viewport[0], 4);
    EXPECT_EQ(backend->last_viewport[1], 5);
    EXPECT_EQ(backend->last_viewport[2], 96);
    EXPECT_EQ(backend->last_viewport[3], 96);
    // Only the window pass cleared (HUD clear is disabled), so exactly one
    // clear happens for this frame.
    EXPECT_EQ(backend->clear_calls - before, 1);
}

TEST(CameraMirrorTest, OrientationKeepsFraming)
{
    auto src = intrusive_ptr<Camera>(new Camera());
    src->setViewMatrixAsLookAt(Vec3d(3, 4, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 4), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    applyCameraMirror(cam.get(), src.get(), MirrorMode::Orientation);

    const Vec3d sf = (src->target() - src->eye()).normalized();
    const Vec3d of = (cam->target() - cam->eye()).normalized();
    EXPECT_NEAR(of.x, sf.x, 1e-6);
    EXPECT_NEAR(of.y, sf.y, 1e-6);
    EXPECT_NEAR(of.z, sf.z, 1e-6);
    // The target kept its own framing distance.
    EXPECT_NEAR((cam->target() - cam->eye()).length(), 4.0, 1e-6);
}

TEST(CameraMirrorTest, FullViewAdoptsSourcePose)
{
    auto src = intrusive_ptr<Camera>(new Camera());
    src->setViewMatrixAsLookAt(Vec3d(1, 2, 3), Vec3d(-1, 0, 0), Vec3d(0, 0, 1));

    auto cam = intrusive_ptr<Camera>(new Camera());
    cam->setViewMatrixAsLookAt(Vec3d(0, 0, 9), Vec3d(0, 0, 0), Vec3d(0, 1, 0));

    applyCameraMirror(cam.get(), src.get(), MirrorMode::FullView);
    EXPECT_NEAR(cam->eye().x, src->eye().x, 1e-9);
    EXPECT_NEAR(cam->eye().y, src->eye().y, 1e-9);
    EXPECT_NEAR(cam->target().x, src->target().x, 1e-9);
    EXPECT_NEAR(cam->target().y, src->target().y, 1e-9);
}

// ============ AxisGizmo ============

TEST(AxisGizmoTest, BuildsThreeColouredSticks)
{
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    ASSERT_NE(gizmo->content(), nullptr);
    const auto* content = dynamic_cast<const Group*>(gizmo->content()->root().get());
    ASSERT_NE(content, nullptr);
    ASSERT_EQ(content->children().size(), 3u);
    for (const auto& node : content->children()) {
        const auto* group = dynamic_cast<const Group*>(node.get());
        ASSERT_NE(group, nullptr);
        ASSERT_EQ(group->children().size(), 1u);
    }
    EXPECT_NE(gizmo->camera(), nullptr);
    // A HUD pass never clears the surface it draws over.
    EXPECT_FALSE(gizmo->clearEnabled());
}

TEST(AxisGizmoTest, ViewportPlacedBottomLeft)
{
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    gizmo->onSurfaceResized(800, 600);

    int x = 0, y = 0, w = 0, h = 0;
    gizmo->getViewport(x, y, w, h);
    EXPECT_EQ(w, 96);
    EXPECT_EQ(h, 96);
    EXPECT_EQ(x, 16);
    EXPECT_EQ(y, 600 - 16 - 96);
}

TEST(AxisGizmoTest, OrientationMirrorTracksSource)
{
    auto gizmo = intrusive_ptr<AxisGizmo>(new AxisGizmo());
    auto src = intrusive_ptr<Camera>(new Camera());
    src->setViewMatrixAsLookAt(Vec3d(2, 3, 6), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    gizmo->setSourceCamera(src.get());
    // The mirror is applied at draw time (execute()); no backend is needed to
    // observe the framing camera update.
    gizmo->execute(nullptr, nullptr);

    Camera* cam = gizmo->camera();
    const Vec3d sf = (src->target() - src->eye()).normalized();
    const Vec3d cf = (cam->target() - cam->eye()).normalized();
    EXPECT_NEAR(cf.x, sf.x, 1e-6);
    EXPECT_NEAR(cf.y, sf.y, 1e-6);
    EXPECT_NEAR(cf.z, sf.z, 1e-6);
}

// ============ RenderPass program override ============

TEST(RenderPassTest, ProgramOverrideReplacesEffectiveProgram)
{
    // A single visible triangle with no per-geometry program.
    auto mesh = makeUnitTriangle();
    auto geometry = intrusive_ptr<Geometry>(new Geometry());
    geometry->setShape(mesh);
    geometry->setMaterial(intrusive_ptr<Material>(new Material()));
    auto group = intrusive_ptr<Group>(new Group());
    group->addChild(geometry);
    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(group);

    auto camera = intrusive_ptr<Camera>(new Camera());
    camera->setViewMatrixAsLookAt(Vec3d(0, 0, 5), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
    camera->setProjectionMatrixAsPerspective(45.0, 1.0, 0.1, 100.0);

    auto backend = intrusive_ptr<MockBackend>(new MockBackend());

    // Without an override each command keeps its effective (per-geometry)
    // program; here it is null (no program set).
    auto pass = intrusive_ptr<RenderPass>(new RenderPass());
    pass->setCamera(camera.get());
    pass->execute(scene.get(), backend.get());
    ASSERT_EQ(backend->last_programs.size(), 1u);
    EXPECT_EQ(backend->last_programs[0], nullptr);

    // A pass-level override forces every command onto one program, so the
    // same content scene can be re-rendered with a different shader.
    auto override_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    auto overridden       = intrusive_ptr<RenderPass>(new RenderPass());
    overridden->setCamera(camera.get());
    overridden->setProgramOverride(override_program);
    EXPECT_EQ(overridden->programOverride(), override_program.get());
    overridden->execute(scene.get(), backend.get());
    ASSERT_EQ(backend->last_programs.size(), 1u);
    EXPECT_EQ(backend->last_programs[0], override_program.get());
}

// ============ RenderBackendRegistry ============

namespace
{

/**
 * @brief A named factory producing a MockBackend for registry tests.
 */
class MockBackendFactory : public RenderBackendFactory {
  public:
    MockBackendFactory() = default;

    explicit MockBackendFactory(const vine::String& name)
      : name_(name)
    {
    }

    RenderBackendInfo info() const override
    {
        const auto name = name_.empty() ? u8"mock3" : name_;
        return RenderBackendInfo{ name, name, u8"mock backend", u8"1.0.0", u8"test",
                                  RenderApi::Vulkan | RenderApi::OpenGL3 };
    }

    vine::intrusive_ptr<RenderBackend> create() override
    {
        return vine::intrusive_ptr<RenderBackend>(new MockBackend());
    }

  private:
    vine::String name_;
};

}  // namespace

TEST(RenderBackendRegistryTest, RegisterAndCreateByName)
{
    auto& registry = RenderBackendRegistry::instance();
    static MockBackendFactory factory(u8"mock");

    registry.registerFactory(&factory);
    EXPECT_TRUE(registry.has(u8"mock"));

    auto backend = registry.create(u8"mock");
    ASSERT_NE(backend, nullptr);

    // Unknown names return null.
    EXPECT_EQ(registry.create(u8"nope"), nullptr);
    EXPECT_FALSE(registry.has(u8"nope"));
}

TEST(RenderBackendRegistryTest, EnumerateNames)
{
    auto& registry = RenderBackendRegistry::instance();
    static MockBackendFactory factory(u8"mock2");
    registry.registerFactory(&factory);

    const auto names = registry.names();
    EXPECT_FALSE(names.empty());
    const bool found = std::find(names.begin(), names.end(), u8"mock2") != names.end();
    EXPECT_TRUE(found);
}

TEST(RenderBackendRegistryTest, EnumerateEntries)
{
    auto& registry = RenderBackendRegistry::instance();
    static MockBackendFactory factory(u8"mock4");
    registry.registerFactory(&factory);

    const auto entries = registry.entries();
    EXPECT_FALSE(entries.empty());

    // Iterate and query each registered backend by name/metadata.
    bool found = false;
    for (const auto& entry : entries) {
        if (entry.info.name == u8"mock4") {
            found = true;
            EXPECT_NE(entry.factory, nullptr);
            EXPECT_EQ(entry.factory->name(), u8"mock4");
            EXPECT_FALSE(entry.info.description.empty());
            EXPECT_TRUE(vine::testFlag(entry.info.api_flags, RenderApi::Vulkan));
            EXPECT_TRUE(vine::testFlag(entry.info.api_flags, RenderApi::OpenGL3));
            EXPECT_EQ(renderApiToString(entry.info.api_flags), u8"vulkan | opengl3");
        }
    }
    EXPECT_TRUE(found);
}

TEST(RenderBackendRegistryTest, RegistrarSelfRegisters)
{
    auto& registry = RenderBackendRegistry::instance();
    // Static: the registrar's embedded factory must outlive the process, as
    // the registry keeps a raw pointer to it.
    static RenderBackendRegistry::Registrar<MockBackendFactory> registrar;
    // The registrar's constructor self-registers; the factory is embedded.
    EXPECT_TRUE(registry.has(u8"mock3"));
}

namespace
{

/**
 * @brief Minimal MaterialManager for the SDK introspection contract.
 *
 * Registers materials in a set, mirroring the count / membership / iteration
 * semantics that any real backend manager (e.g. VsgMaterialManager) must
 * provide through the interface.
 */
class FakeMaterialManager : public MaterialManager {
  public:
    void updateMaterial(vine::raw_ptr<Material> material) override
    {
        if (material != nullptr) {
            registered_.insert(material);
        }
    }

    void releaseMaterial(vine::raw_ptr<Material> material) override
    {
        registered_.erase(material);
    }

    void clear() override
    {
        registered_.clear();
    }

    std::size_t materialCount() const override
    {
        return registered_.size();
    }

    bool hasMaterial(vine::raw_ptr<Material> material) const override
    {
        return registered_.count(material) != 0u;
    }

    void forEachMaterial(const std::function<void(vine::raw_ptr<Material>)>& visitor) const override
    {
        for (vine::raw_ptr<Material> m : registered_) {
            visitor(m);
        }
    }

  private:
    std::set<vine::raw_ptr<Material>> registered_;
};

}  // namespace

TEST(MaterialManagerTest, RegisteredMaterialIntrospection)
{
    FakeMaterialManager manager;

    auto a = intrusive_ptr<Material>(new Material());
    auto b = intrusive_ptr<Material>(new Material());

    // Nothing registered yet.
    EXPECT_EQ(manager.materialCount(), 0u);
    EXPECT_FALSE(manager.hasMaterial(a.get()));
    EXPECT_FALSE(manager.hasMaterial(nullptr));

    // Updating registers each material exactly once (deduplicated by pointer).
    manager.updateMaterial(a.get());
    manager.updateMaterial(a.get());
    manager.updateMaterial(b.get());
    EXPECT_EQ(manager.materialCount(), 2u);
    EXPECT_TRUE(manager.hasMaterial(a.get()));
    EXPECT_TRUE(manager.hasMaterial(b.get()));

    // forEachMaterial enumerates every registered material.
    std::set<vine::raw_ptr<Material>> seen;
    manager.forEachMaterial([&seen](vine::raw_ptr<Material> m) { seen.insert(m); });
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_TRUE(seen.count(a.get()) == 1u && seen.count(b.get()) == 1u);

    // Release drops a single registration.
    manager.releaseMaterial(a.get());
    EXPECT_EQ(manager.materialCount(), 1u);
    EXPECT_FALSE(manager.hasMaterial(a.get()));
    EXPECT_TRUE(manager.hasMaterial(b.get()));

    // Clear drops everything.
    manager.clear();
    EXPECT_EQ(manager.materialCount(), 0u);
    EXPECT_FALSE(manager.hasMaterial(b.get()));
}

TEST(StateNodeTest, NewNodeHasNoState)
{
    StateNode state;
    EXPECT_FALSE(state.hasState());
    EXPECT_TRUE(state.renderState().empty());
    EXPECT_FALSE(state.depth().has_value());
    EXPECT_FALSE(state.cullMode().has_value());
    EXPECT_FALSE(state.blend().has_value());
    EXPECT_FALSE(state.polygonMode().has_value());
    EXPECT_FALSE(state.topology().has_value());
}

TEST(StateNodeTest, TopologyIsStateAndDefaultsToTriangles)
{
    StateNode state;
    EXPECT_FALSE(state.topology().has_value());
    EXPECT_EQ(effectiveRenderState(&state).topology, Topology::Triangles);

    state.setTopology(Topology::Points);
    ASSERT_TRUE(state.topology().has_value());
    EXPECT_EQ(*state.topology(), Topology::Points);
    EXPECT_EQ(effectiveRenderState(&state).topology, Topology::Points);

    state.clearTopology();
    EXPECT_FALSE(state.topology().has_value());
    EXPECT_EQ(effectiveRenderState(&state).topology, Topology::Triangles);
}
TEST(StateNodeTest, SettersAndClears)
{
    StateNode state;

    const DepthState depth;
    state.setDepth(depth);
    EXPECT_TRUE(state.hasState());
    ASSERT_TRUE(state.depth().has_value());
    EXPECT_EQ(*state.depth(), depth);

    state.clearDepth();
    EXPECT_FALSE(state.depth().has_value());
    EXPECT_FALSE(state.hasState());

    state.setCullMode(CullMode::Front);
    state.setPolygonMode(PolygonMode::Line);
    ASSERT_TRUE(state.cullMode().has_value());
    ASSERT_TRUE(state.polygonMode().has_value());
    EXPECT_EQ(*state.cullMode(), CullMode::Front);
    EXPECT_EQ(*state.polygonMode(), PolygonMode::Line);

    state.clearCullMode();
    EXPECT_FALSE(state.cullMode().has_value());
    EXPECT_TRUE(state.polygonMode().has_value());

    BlendState blend;
    blend.enabled = true;
    blend.src     = BlendFactor::SrcColor;
    state.setBlend(blend);
    ASSERT_TRUE(state.blend().has_value());
    EXPECT_TRUE(state.blend()->enabled);
    EXPECT_EQ(state.blend()->src, BlendFactor::SrcColor);

    // clearState() resets every item at once.
    state.clearState();
    EXPECT_FALSE(state.hasState());
    EXPECT_FALSE(state.blend().has_value());
    EXPECT_FALSE(state.polygonMode().has_value());
}

TEST(StateNodeTest, ResolveAppliesDefaults)
{
    const ResolvedRenderState fallback = resolveRenderState(RenderState());
    EXPECT_TRUE(fallback.depth.test);
    EXPECT_TRUE(fallback.depth.write);
    EXPECT_EQ(fallback.depth.compare, CompareOp::Less);
    EXPECT_EQ(fallback.cullMode, CullMode::None);
    EXPECT_FALSE(fallback.blend.enabled);
    EXPECT_EQ(fallback.polygonMode, PolygonMode::Fill);
    EXPECT_EQ(fallback.topology, Topology::Triangles);

    RenderState partial;
    partial.cullMode    = CullMode::Back;
    partial.polygonMode = PolygonMode::Line;
    partial.topology    = Topology::Points;
    const ResolvedRenderState resolved = resolveRenderState(partial);
    EXPECT_EQ(resolved.cullMode, CullMode::Back);
    EXPECT_EQ(resolved.polygonMode, PolygonMode::Line);
    EXPECT_EQ(resolved.topology, Topology::Points);
    EXPECT_TRUE(resolved.depth.test);
    EXPECT_TRUE(resolved.depth.write);
}

TEST(StateNodeTest, FoldInheritsFromAncestorStateNode)
{
    auto root = intrusive_ptr<StateNode>(new StateNode());
    root->setCullMode(CullMode::Back);
    root->setPolygonMode(PolygonMode::Line);

    auto child = intrusive_ptr<Group>(new Group());
    auto leaf  = intrusive_ptr<Node>(new Node());
    child->addChild(leaf);
    root->addChild(child);

    const RenderState folded = collectRenderState(leaf.get());
    ASSERT_TRUE(folded.cullMode.has_value());
    ASSERT_TRUE(folded.polygonMode.has_value());
    EXPECT_EQ(*folded.cullMode, CullMode::Back);
    EXPECT_EQ(*folded.polygonMode, PolygonMode::Line);
    EXPECT_FALSE(folded.depth.has_value());

    const ResolvedRenderState effective = effectiveRenderState(leaf.get());
    EXPECT_EQ(effective.cullMode, CullMode::Back);
    EXPECT_EQ(effective.polygonMode, PolygonMode::Line);
    EXPECT_TRUE(effective.depth.test);
}

TEST(StateNodeTest, FoldDeeperNodeOverridesShallower)
{
    auto outer = intrusive_ptr<StateNode>(new StateNode());
    outer->setCullMode(CullMode::Back);

    auto inner = intrusive_ptr<StateNode>(new StateNode());
    inner->setCullMode(CullMode::Front);
    inner->setBlend(BlendState());
    outer->addChild(inner);

    const ResolvedRenderState effective = effectiveRenderState(inner.get());
    EXPECT_EQ(effective.cullMode, CullMode::Front);
    EXPECT_FALSE(effective.blend.enabled);
}

TEST(StateNodeTest, FoldCombinesDifferentItemsAcrossLevels)
{
    auto outer = intrusive_ptr<StateNode>(new StateNode());
    DepthState no_write;
    no_write.write = false;
    outer->setDepth(no_write);

    auto inner = intrusive_ptr<StateNode>(new StateNode());
    inner->setPolygonMode(PolygonMode::Line);
    outer->addChild(inner);

    auto leaf = intrusive_ptr<Node>(new Node());
    inner->addChild(leaf);

    const ResolvedRenderState effective = effectiveRenderState(leaf.get());
    EXPECT_TRUE(effective.depth.test);
    EXPECT_FALSE(effective.depth.write);
    EXPECT_EQ(effective.polygonMode, PolygonMode::Line);
    EXPECT_EQ(effective.cullMode, CullMode::None);
}

TEST(StateNodeTest, FoldEmptyWhenPathHasNoStateNode)
{
    auto root = intrusive_ptr<Group>(new Group());
    auto leaf = intrusive_ptr<Node>(new Node());
    root->addChild(leaf);

    EXPECT_TRUE(collectRenderState(leaf.get()).empty());
    const ResolvedRenderState effective = effectiveRenderState(leaf.get());
    EXPECT_TRUE(effective.depth.test);
    EXPECT_EQ(effective.cullMode, CullMode::None);
    EXPECT_EQ(effective.polygonMode, PolygonMode::Fill);
}

TEST(StateNodeTest, ProgramSlotSetClear)
{
    StateNode state;
    EXPECT_EQ(state.program(), nullptr);

    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"subtree");
    state.setProgram(program);
    EXPECT_EQ(state.program(), program.get());

    state.clearProgram();
    EXPECT_EQ(state.program(), nullptr);
}

TEST(StateNodeTest, EffectiveProgramLeafWinsOverAncestor)
{
    auto ancestor = intrusive_ptr<StateNode>(new StateNode());
    auto ancestor_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    ancestor_program->setName(u8"ancestor");
    ancestor->setProgram(ancestor_program);

    // No leaf program -> nearest ancestor StateNode program applies.
    auto holder = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    ancestor->addChild(holder);
    Geometry* geom = dynamic_cast<Geometry*>(holder->children().front().get());
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(effectiveProgram(geom).get(), ancestor_program.get());

    // A leaf program overrides the ancestor.
    auto leaf_program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    leaf_program->setName(u8"leaf");
    geom->setProgram(leaf_program);
    EXPECT_EQ(effectiveProgram(geom).get(), leaf_program.get());

    // Clearing the leaf falls back to the ancestor program.
    geom->setProgram(nullptr);
    EXPECT_EQ(effectiveProgram(geom).get(), ancestor_program.get());
}

TEST(StateNodeTest, EffectiveProgramDefaultsNull)
{
    auto root = intrusive_ptr<Group>(new Group());
    auto holder = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri");
    root->addChild(holder);
    Geometry* geom = dynamic_cast<Geometry*>(holder->children().front().get());
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(effectiveProgram(geom), nullptr);
}

TEST(SceneTest, CollectCommandsCarriesEffectiveProgram)
{
    Scene scene;
    auto state = intrusive_ptr<StateNode>(new StateNode());
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->setName(u8"scene-program");
    state->setProgram(program);

    auto holder = makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"geo");
    state->addChild(holder);
    scene.setRoot(state);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].program.get(), program.get());

    // Without any program the command carries the engine default (null).
    Scene plain;
    plain.setRoot(makeTriangleNode(Vec3d(0, 0, -3), nullptr, u8"tri"));
    auto plain_commands = plain.collectRenderCommands(&cam);
    ASSERT_EQ(plain_commands.size(), 1u);
    EXPECT_EQ(plain_commands[0].program, nullptr);
}

TEST(SceneTest, CollectCommandsCarriesStateFromAncestorStateNode)
{
    Scene scene;
    auto root = intrusive_ptr<StateNode>(new StateNode());
    root->setCullMode(CullMode::Back);
    DepthState no_write;
    no_write.write = false;
    root->setDepth(no_write);

    auto holder = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"geo");
    root->addChild(holder);
    scene.setRoot(root);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].renderState.cullMode, CullMode::Back);
    EXPECT_TRUE(commands[0].renderState.depth.test);
    EXPECT_FALSE(commands[0].renderState.depth.write);
}

TEST(SceneTest, CollectCommandsDeeperStateNodeOverrides)
{
    Scene scene;
    auto outer = intrusive_ptr<StateNode>(new StateNode());
    outer->setPolygonMode(PolygonMode::Line);

    auto inner = intrusive_ptr<StateNode>(new StateNode());
    inner->setPolygonMode(PolygonMode::Point);
    outer->addChild(inner);

    auto holder = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"geo");
    inner->addChild(holder);
    scene.setRoot(outer);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].renderState.polygonMode, PolygonMode::Point);
}

TEST(SceneTest, CollectCommandsDefaultsStateWithoutStateNode)
{
    Scene scene;
    auto holder = makeTriangleNode(Vec3d(0, 0, -2), nullptr, u8"geo");
    scene.setRoot(holder);

    Camera cam;
    setupLookAtCamera(cam);
    auto commands = scene.collectRenderCommands(&cam);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_TRUE(commands[0].renderState.depth.test);
    EXPECT_TRUE(commands[0].renderState.depth.write);
    EXPECT_EQ(commands[0].renderState.cullMode, CullMode::None);
    EXPECT_EQ(commands[0].renderState.polygonMode, PolygonMode::Fill);
}

TEST(GeometryTest, RawPositionsDriveCountsAndBounds)
{
    Geometry geom;
    EXPECT_FALSE(geom.hasPositions());
    EXPECT_FALSE(geom.hasIndices());
    EXPECT_EQ(geom.vertexCount(), 0u);
    EXPECT_TRUE(geom.boundingBox().isEmpty());

    vine::geometry::Vec3fArray points = {
        vine::math::Vec3f(0.0f, 0.0f, 0.0f),
        vine::math::Vec3f(2.0f, 0.0f, 0.0f),
        vine::math::Vec3f(0.0f, 3.0f, 0.0f),
    };
    geom.setPositions(points);
    EXPECT_TRUE(geom.hasPositions());
    EXPECT_EQ(geom.positionCount(), 3u);
    EXPECT_EQ(geom.vertexCount(), 3u);

    const Aabbd box = geom.boundingBox();
    EXPECT_TRUE(box.isValid());
    EXPECT_NEAR(box.min().x, 0.0, 1e-9);
    EXPECT_NEAR(box.max().x, 2.0, 1e-9);
    EXPECT_NEAR(box.max().y, 3.0, 1e-9);

    vine::geometry::UInt32Array indices = { 0u, 1u, 2u };
    geom.setIndices(indices);
    EXPECT_TRUE(geom.hasIndices());
    ASSERT_NE(geom.indices(), nullptr);
    EXPECT_EQ(geom.indices()->size(), 3u);
    EXPECT_EQ(geom.vertexCount(), 3u);
}

TEST(GeometryTest, NormalsChannelAndRevision)
{
    Geometry geom;
    EXPECT_FALSE(geom.hasNormals());
    EXPECT_EQ(geom.revision(), 0u);

    vine::geometry::Vec3fArray points = { vine::math::Vec3f(0, 0, 0), vine::math::Vec3f(1, 0, 0),
                                          vine::math::Vec3f(0, 1, 0) };
    geom.setPositions(points);
    EXPECT_EQ(geom.revision(), 1u);

    vine::geometry::Vec3fArray normals = { vine::math::Vec3f(0, 0, 1), vine::math::Vec3f(0, 0, 1),
                                           vine::math::Vec3f(0, 0, 1) };
    geom.setNormals(normals);
    EXPECT_TRUE(geom.hasNormals());
    EXPECT_EQ(geom.normalCount(), 3u);
    EXPECT_EQ(geom.revision(), 2u);
}

TEST(GeometryTest, ConverterFillsBuffersFromTriangleMesh)
{
    auto mesh = makeUnitTriangle();
    auto geom = geometryFromShape(*mesh);

    ASSERT_NE(geom.get(), nullptr);
    EXPECT_TRUE(geom->hasPositions());
    EXPECT_EQ(geom->positionCount(), 3u);
    EXPECT_EQ(geom->vertexCount(), 3u);
    // The triangle mesh carries no normals, so the converter leaves them empty
    // (the renderer derives normals from the positions when needed).
    EXPECT_FALSE(geom->hasNormals());
    EXPECT_FALSE(geom->hasIndices());

    // setShape on an empty geometry is equivalent to the converter.
    auto via_setter = intrusive_ptr<Geometry>(new Geometry());
    via_setter->setShape(mesh);
    EXPECT_TRUE(via_setter->hasPositions());
    EXPECT_EQ(via_setter->vertexCount(), 3u);
}

TEST(GeometryTest, OpenAttributeBufferList)
{
    Geometry geom;
    EXPECT_EQ(geom.bufferCount(), 0u);
    EXPECT_TRUE(geom.bufferLocations().empty());
    const std::uint64_t base = geom.revision();

    // Any number of custom channels can be added at arbitrary locations.
    AttributeBuffer colour;
    colour.components = 4;
    colour.data       = std::make_shared<std::vector<float>>(
        std::vector<float>{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f });
    geom.addBuffer(2, colour);
    AttributeBuffer size;
    size.components = 1;
    size.data       = std::make_shared<std::vector<float>>(
        std::vector<float>{ 1.0f, 2.0f, 3.0f });
    geom.addBuffer(4, size);

    EXPECT_EQ(geom.bufferCount(), 2u);
    ASSERT_NE(geom.buffer(2), nullptr);
    EXPECT_EQ(geom.buffer(2)->components, 4u);
    ASSERT_NE(geom.buffer(2)->data, nullptr);
    EXPECT_EQ(geom.buffer(2)->data->size(), 8u);
    ASSERT_NE(geom.buffer(4), nullptr);
    EXPECT_EQ(geom.buffer(4)->components, 1u);
    EXPECT_EQ(geom.buffer(4)->data->size(), 3u);
    EXPECT_EQ(geom.bufferLocations(), (std::vector<std::uint32_t>{ 2, 4 }));
    EXPECT_GT(geom.revision(), base);

    // Replacing a location keeps the count stable and bumps the revision.
    const std::uint64_t after_add = geom.revision();
    AttributeBuffer replaced = colour;
    geom.addBuffer(2, replaced);
    EXPECT_EQ(geom.bufferCount(), 2u);
    EXPECT_GT(geom.revision(), after_add);

    geom.removeBuffer(4);
    EXPECT_FALSE(geom.hasBuffer(4));
    EXPECT_EQ(geom.bufferLocations(), (std::vector<std::uint32_t>{ 2 }));
}

